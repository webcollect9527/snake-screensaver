// 原生 Win32 屏幕保护程序壳：/s 全屏、/c 配置、/p 预览、鼠标/键盘/点击退出、GDI 渲染。
// 编译：gcc src/win/screensaver.c src/win/game.c -o SnakeScreensaver.scr -Os -s -mwindows
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "game.h"

// 渲染参数（可由 /c 配置窗口写入 config.json 覆盖，见 config_load）
static int g_cell = 10;          // 单元格边长（px）
static double g_blinkPeriod = 5.0; // 方块呼吸闪烁周期（秒）

// tcc 自带头文件较老，缺少的宏/函数在此补齐
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#define NOW_MS ((double)GetTickCount())

#ifdef SAVER_DEBUG
#include <stdio.h>
#define DBG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define DBG(...) ((void)0)
#endif

// ---- 颜色表（与 CLAUDE.md 4.2 一致）----
static const COLORREF COLORS[7] = {
  RGB(0xE5, 0x48, 0x4D), RGB(0x46, 0xA7, 0x58), RGB(0x3E, 0x63, 0xDD), RGB(0xFF, 0xB2, 0x24),
  RGB(0x8E, 0x4E, 0xC6), RGB(0x00, 0xA2, 0xC7), RGB(0xFF, 0xFF, 0xFF),
};
static const COLORREF WALL_COLOR = RGB(0x2B, 0x2B, 0x2B);

static HINSTANCE g_hInst;
static Game g_game;
static int g_mode = 0;        // 0 未定 1 屏保 2 配置 3 预览
static int g_windowed = 0;    // --windowed 调试模式（普通窗口、不监听退出）
static int g_saverMode = 0;   // 正式屏保（全屏 + 退出监听 + 隐藏光标）
static int g_cols, g_rows;
static HWND g_hwnd;
static long g_lastX = -1, g_lastY = -1, g_moveDist = 0;
static int g_ended = 0;

// ---- 跨会话统计（%APPDATA%\snake-screensaver\stats.json，与 TS 版同格式）----
typedef struct { int maxSurvival, successCount, failCount, colorCounts[7], totalScore; } SaveStats;
static SaveStats g_stats;
static char g_statsPath[MAX_PATH];
static double g_maxSurvival = 0;

static int parse_int(const char *buf, const char *key) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(buf, pat);
  if (!p) return 0;
  p = strchr(p, ':');
  if (!p) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  return atoi(p);
}
static void parse_counts(const char *buf, int out[7]) {
  const char *p = strstr(buf, "\"colorCounts\"");
  if (!p) return;
  p = strchr(p, '[');
  if (!p) return;
  p++;
  for (int i = 0; i < 7 && *p; i++) {
    while (*p && (*p < '0' || *p > '9') && *p != '-') p++;
    out[i] = atoi(p);
    while (*p && *p != ',') p++;
  }
}
static void stats_load(void) {
  const char *ap = getenv("APPDATA");
  if (!ap) ap = "";
  snprintf(g_statsPath, MAX_PATH, "%s\\snake-screensaver\\stats.json", ap);
  g_stats = (SaveStats){0};
  FILE *f = fopen(g_statsPath, "rb");
  if (f) {
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    g_stats.maxSurvival = parse_int(buf, "maxSurvivalSeconds");
    g_stats.successCount = parse_int(buf, "successCount");
    g_stats.failCount = parse_int(buf, "failCount");
    g_stats.totalScore = parse_int(buf, "totalScore");
    parse_counts(buf, g_stats.colorCounts);
  }
  g_maxSurvival = g_stats.maxSurvival;
}
static void stats_save(void) {
  char dir[MAX_PATH];
  snprintf(dir, sizeof dir, "%s\\snake-screensaver", getenv("APPDATA") ? getenv("APPDATA") : "");
  CreateDirectoryA(dir, NULL);
  FILE *f = fopen(g_statsPath, "wb");
  if (!f) return;
  fprintf(f, "{\n");
  fprintf(f, "  \"maxSurvivalSeconds\": %d,\n", g_stats.maxSurvival);
  fprintf(f, "  \"successCount\": %d,\n", g_stats.successCount);
  fprintf(f, "  \"failCount\": %d,\n", g_stats.failCount);
  fprintf(f, "  \"colorCounts\": [%d,%d,%d,%d,%d,%d,%d],\n",
          g_stats.colorCounts[0], g_stats.colorCounts[1], g_stats.colorCounts[2],
          g_stats.colorCounts[3], g_stats.colorCounts[4], g_stats.colorCounts[5], g_stats.colorCounts[6]);
  fprintf(f, "  \"totalScore\": %d\n", g_stats.totalScore);
  fprintf(f, "}\n");
  fclose(f);
}
static void record_stats_if_ended(void) {
  if (!g_ended && g_game.state == 1) {
    g_ended = 1;
    if (g_game.survival > g_maxSurvival) g_maxSurvival = g_game.survival;
    int success = g_game.reason == 1;
    g_stats.successCount += success;
    g_stats.failCount += 1 - success;
    for (int i = 0; i < 7; i++) g_stats.colorCounts[i] += g_game.colorCounts[i];
    g_stats.totalScore += g_game.score;
    g_stats.maxSurvival = (int)g_maxSurvival;
    stats_save();
  }
  if (g_ended && g_game.state == 0) g_ended = 0; // 已自动重开
}

// ---- 用户配置（%APPDATA%\snake-screensaver\config.json，由 /c 设置窗口读写）----
typedef struct {
  int cellSizePx;        // 单元格边长（px）
  double blinkPeriodSec; // 方块呼吸闪烁周期（秒）
  double baseSpeed;      // 基础速度（格/秒）
  int initialBlockCap;   // 初始同屏方块数上限
  int blockLifetime;     // 方块初始生存秒数
  double shrinkFraction; // 场地缩小比例
  double urgencyThreshold;
  double urgencyFactor;
  double endFreezeMs;
  int weights[7];        // 7 种方块生成占比
} Config;
static Config g_cfg;
static char g_cfgPath[MAX_PATH];

static void config_default(void) {
  Params p = params_default();
  g_cfg.cellSizePx = 10;
  g_cfg.blinkPeriodSec = 5.0;
  g_cfg.baseSpeed = p.baseSpeed;
  g_cfg.initialBlockCap = p.initialBlockCap;
  g_cfg.blockLifetime = p.blockLifetime;
  g_cfg.shrinkFraction = p.shrinkFraction;
  g_cfg.urgencyThreshold = p.urgencyThreshold;
  g_cfg.urgencyFactor = p.urgencyFactor;
  g_cfg.endFreezeMs = p.endFreezeMs;
  memcpy(g_cfg.weights, p.weights, sizeof g_cfg.weights);
}

// 由配置构造游戏参数（非可配置项取默认值）
static Params cfg_to_params(void) {
  Params p = params_default();
  p.baseSpeed = g_cfg.baseSpeed;
  p.initialBlockCap = g_cfg.initialBlockCap;
  p.blockLifetime = g_cfg.blockLifetime;
  p.shrinkFraction = g_cfg.shrinkFraction;
  p.urgencyThreshold = g_cfg.urgencyThreshold;
  p.urgencyFactor = g_cfg.urgencyFactor;
  p.endFreezeMs = g_cfg.endFreezeMs;
  memcpy(p.weights, g_cfg.weights, sizeof p.weights);
  return p;
}

// 在 JSON 文本中定位 "key": <value>，返回 value 起始指针（找不到返回 NULL）
static const char *find_val(const char *buf, const char *key) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(buf, pat);
  if (!p) return NULL;
  p = strchr(p, ':');
  if (!p) return NULL;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}
static void parse_weights(const char *v, int out[7]) {
  const char *p = strchr(v, '[');
  if (!p) return;
  p++;
  for (int i = 0; i < 7 && *p; i++) {
    while (*p && (*p < '0' || *p > '9') && *p != '-') p++;
    out[i] = atoi(p);
    while (*p && *p != ',') p++;
  }
}

static void config_load(void) {
  const char *ap = getenv("APPDATA");
  if (!ap) ap = "";
  snprintf(g_cfgPath, MAX_PATH, "%s\\snake-screensaver\\config.json", ap);
  config_default();
  FILE *f = fopen(g_cfgPath, "rb");
  if (f) {
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *v;
    if ((v = find_val(buf, "cellSizePx")) && atoi(v) >= 4 && atoi(v) <= 64) g_cfg.cellSizePx = atoi(v);
    if ((v = find_val(buf, "blinkPeriodSec")) && strtod(v, NULL) >= 0.5) g_cfg.blinkPeriodSec = strtod(v, NULL);
    if ((v = find_val(buf, "baseSpeed")) && strtod(v, NULL) > 0) g_cfg.baseSpeed = strtod(v, NULL);
    if ((v = find_val(buf, "initialBlockCap")) && atoi(v) >= 1) g_cfg.initialBlockCap = atoi(v);
    if ((v = find_val(buf, "blockLifetime")) && atoi(v) >= 1) g_cfg.blockLifetime = atoi(v);
    if ((v = find_val(buf, "shrinkFraction"))) { double d = strtod(v, NULL); if (d > 0 && d < 1) g_cfg.shrinkFraction = d; }
    if ((v = find_val(buf, "urgencyThreshold")) && strtod(v, NULL) >= 0) g_cfg.urgencyThreshold = strtod(v, NULL);
    if ((v = find_val(buf, "urgencyFactor")) && strtod(v, NULL) >= 0) g_cfg.urgencyFactor = strtod(v, NULL);
    if ((v = find_val(buf, "endFreezeMs")) && strtod(v, NULL) >= 0) g_cfg.endFreezeMs = strtod(v, NULL);
    if ((v = find_val(buf, "weights"))) parse_weights(v, g_cfg.weights);
  }
  g_cell = g_cfg.cellSizePx;
  g_blinkPeriod = g_cfg.blinkPeriodSec;
}

static void config_save(void) {
  char dir[MAX_PATH];
  snprintf(dir, sizeof dir, "%s\\snake-screensaver", getenv("APPDATA") ? getenv("APPDATA") : "");
  CreateDirectoryA(dir, NULL);
  FILE *f = fopen(g_cfgPath, "wb");
  if (!f) return;
  fprintf(f, "{\n");
  fprintf(f, "  \"cellSizePx\": %d,\n", g_cfg.cellSizePx);
  fprintf(f, "  \"blinkPeriodSec\": %g,\n", g_cfg.blinkPeriodSec);
  fprintf(f, "  \"baseSpeed\": %g,\n", g_cfg.baseSpeed);
  fprintf(f, "  \"initialBlockCap\": %d,\n", g_cfg.initialBlockCap);
  fprintf(f, "  \"blockLifetime\": %d,\n", g_cfg.blockLifetime);
  fprintf(f, "  \"shrinkFraction\": %g,\n", g_cfg.shrinkFraction);
  fprintf(f, "  \"urgencyThreshold\": %g,\n", g_cfg.urgencyThreshold);
  fprintf(f, "  \"urgencyFactor\": %g,\n", g_cfg.urgencyFactor);
  fprintf(f, "  \"endFreezeMs\": %g,\n", g_cfg.endFreezeMs);
  fprintf(f, "  \"weights\": [%d,%d,%d,%d,%d,%d,%d]\n",
          g_cfg.weights[0], g_cfg.weights[1], g_cfg.weights[2],
          g_cfg.weights[3], g_cfg.weights[4], g_cfg.weights[5], g_cfg.weights[6]);
  fprintf(f, "}\n");
  fclose(f);
}

// 呼吸闪烁：亮度因子 f∈[0,1] 在满色与 40% 暗色间平滑过渡
static COLORREF blend(COLORREF full, COLORREF dimc, double t) {
  return RGB((int)(GetRValue(full) * t + GetRValue(dimc) * (1 - t)),
             (int)(GetGValue(full) * t + GetGValue(dimc) * (1 - t)),
             (int)(GetBValue(full) * t + GetBValue(dimc) * (1 - t)));
}
static COLORREF dim(COLORREF c) { return RGB(GetRValue(c) * 2 / 5, GetGValue(c) * 2 / 5, GetBValue(c) * 2 / 5); }

// ---- 渲染 ----
static void draw_game(HDC dc, int w, int h, double nowMs) {
  Rect r = g_game.grid.operable;
  RECT all = {0, 0, w, h};
  HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
  FillRect(dc, &all, black);
  DeleteObject(black);

  // 围墙（可操作区域外）
  HBRUSH wall = CreateSolidBrush(WALL_COLOR);
  RECT top = {0, 0, w, r.y0 * g_cell};
  RECT bot = {0, r.y1 * g_cell, w, h - r.y1 * g_cell};
  RECT lef = {0, r.y0 * g_cell, r.x0 * g_cell, (r.y1 - r.y0) * g_cell};
  RECT rig = {r.x1 * g_cell, r.y0 * g_cell, w - r.x1 * g_cell, (r.y1 - r.y0) * g_cell};
  FillRect(dc, &top, wall);
  FillRect(dc, &bot, wall);
  FillRect(dc, &lef, wall);
  FillRect(dc, &rig, wall);
  DeleteObject(wall);

  // 网格线
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(20, 20, 20));
  HGDIOBJ oldPen = SelectObject(dc, pen);
  for (int x = r.x0; x <= r.x1; x++) { MoveToEx(dc, x * g_cell, r.y0 * g_cell, NULL); LineTo(dc, x * g_cell, r.y1 * g_cell); }
  for (int y = r.y0; y <= r.y1; y++) { MoveToEx(dc, r.x0 * g_cell, y * g_cell, NULL); LineTo(dc, r.x1 * g_cell, y * g_cell); }
  SelectObject(dc, oldPen);
  DeleteObject(pen);

  // 方块（呼吸灯：亮度在满色/暗色间按 sin 平滑过渡，周期 g_blinkPeriod；剩余 <10s 加快预警）
  for (int i = 0; i < g_game.blockCount; i++) {
    Block *b = &g_game.blocks[i];
    double period = b->remaining < 10 ? g_blinkPeriod / 3.0 : g_blinkPeriod;
    double f = 0.5 + 0.5 * sin(2 * 3.14159265358979 * nowMs / 1000.0 / period);
    COLORREF full = COLORS[b->kind - 1];
    COLORREF c = blend(full, dim(full), f);
    HBRUSH br = CreateSolidBrush(c);
    RECT rr = {b->x * g_cell + 1, b->y * g_cell + 1, (b->x + 1) * g_cell - 1, (b->y + 1) * g_cell - 1};
    FillRect(dc, &rr, br);
    DeleteObject(br);
  }

  // 蛇（尾先画）
  for (int i = g_game.snake.len - 1; i >= 0; i--) {
    Point p = g_game.snake.seg[i];
    HBRUSH br = CreateSolidBrush(COLORS[g_game.snake.color[i]]);
    RECT rr = {p.x * g_cell + 1, p.y * g_cell + 1, (p.x + 1) * g_cell - 1, (p.y + 1) * g_cell - 1};
    FillRect(dc, &rr, br);
    DeleteObject(br);
  }
}

static void draw_hud(HDC dc, int w) {
  int rowH = 16, bw = 200, bh = 12 + rowH * 11;
  int bx = w - bw - 12, by = 10;
  HBRUSH bg = CreateSolidBrush(RGB(10, 10, 10));
  RECT r = {bx, by, bx + bw, by + bh};
  FillRect(dc, &r, bg);
  DeleteObject(bg);
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
  HGDIOBJ op = SelectObject(dc, pen);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, bx, by, bx + bw, by + bh);
  SelectObject(dc, op);
  DeleteObject(pen);

  HFONT f = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
  HGDIOBJ of = SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(230, 230, 230));
  int ly = by + 8;
  char buf[64];
  for (int i = 0; i < 7; i++) {
    HBRUSH cbr = CreateSolidBrush(COLORS[i]);
    RECT sq = {bx + 10, ly + 3, bx + 20, ly + 13};
    FillRect(dc, &sq, cbr);
    DeleteObject(cbr);
    snprintf(buf, sizeof buf, "x%d", g_game.colorCounts[i]);
    TextOutA(dc, bx + 26, ly, buf, (int)strlen(buf));
    ly += rowH;
  }
  snprintf(buf, sizeof buf, "Score %d", g_game.score);
  TextOutA(dc, bx + 10, ly, buf, (int)strlen(buf)); ly += rowH;
  int t = (int)g_game.survival;
  snprintf(buf, sizeof buf, "Time %02d:%02d", t / 60, t % 60);
  TextOutA(dc, bx + 10, ly, buf, (int)strlen(buf)); ly += rowH;
  double m = g_game.survival > g_maxSurvival ? g_game.survival : g_maxSurvival;
  int mm = (int)m;
  snprintf(buf, sizeof buf, "Best %02d:%02d", mm / 60, mm % 60);
  TextOutA(dc, bx + 10, ly, buf, (int)strlen(buf)); ly += rowH;
  snprintf(buf, sizeof buf, "OK %d / FAIL %d", g_stats.successCount, g_stats.failCount);
  TextOutA(dc, bx + 10, ly, buf, (int)strlen(buf));
  SelectObject(dc, of);
  DeleteObject(f);
}

// ---- 主窗口（屏保/调试）----
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: DBG("[wnd] created\n"); return 0;
    case WM_CLOSE: DBG("[wnd] WM_CLOSE\n"); break;
    case WM_MOUSEMOVE: {
      if (!g_saverMode) break;
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (g_lastX < 0) { g_lastX = x; g_lastY = y; }
      g_moveDist += abs(x - g_lastX) + abs(y - g_lastY);
      g_lastX = x; g_lastY = y;
      if (g_moveDist > 10) PostMessage(hwnd, WM_CLOSE, 0, 0); // 阈值 >10px 避免误触
      return 0;
    }
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
      if (g_saverMode) PostMessage(hwnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_TIMER:
      game_update(&g_game, 25.0);
      record_stats_if_ended();
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      draw_game(dc, rc.right, rc.bottom, NOW_MS);
      draw_hud(dc, rc.right);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      DBG("[wnd] WM_DESTROY\n");
      KillTimer(hwnd, 1);
      if (g_saverMode) {
        ShowCursor(TRUE);
        SystemParametersInfo(SPI_SETSCREENSAVERRUNNING, FALSE, NULL, 0);
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

// ---- 配置窗口（/c，设置窗口：可调参数 + 方块占比 + 呼吸周期）----
// 控件 ID
#define IDC_OK 1
#define IDC_CANCEL 2
#define IDC_BASE_SPEED 100
#define IDC_INIT_CAP 101
#define IDC_LIFETIME 102
#define IDC_SHRINK 103
#define IDC_URG_T 104
#define IDC_URG_F 105
#define IDC_FREEZE 106
#define IDC_BLINK 107
#define IDC_CELL 108
#define IDC_W1 110 // 占比 1..7 依次 +i

#define CFG_ROWH 24
#define CFG_LW 170 // 标签宽度

// 创建一行「标签 + 编辑框」，edit 用 id 标识
static void cfg_row(HWND hwnd, int id, const char *label, const char *val, int x, int y) {
  CreateWindowExA(0, "static", label, WS_CHILD | WS_VISIBLE | SS_LEFT,
                  x, y + 2, CFG_LW, 20, hwnd, NULL, g_hInst, NULL);
  CreateWindowExA(0, "edit", val, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                  x + CFG_LW + 2, y, 64, 20, hwnd, (HMENU)(INT_PTR)id, g_hInst, NULL);
}
static double cfg_double(HWND hwnd, int id, double def) {
  char b[32];
  GetDlgItemTextA(hwnd, id, b, sizeof b);
  char *end;
  double v = strtod(b, &end);
  return (end != b) ? v : def;
}
static int cfg_int(HWND hwnd, int id, int def) {
  char b[32];
  GetDlgItemTextA(hwnd, id, b, sizeof b);
  char *end;
  long v = strtol(b, &end, 10);
  return (end != b && *end == 0) ? (int)v : def;
}

// 从窗口读取全部字段 → 校验/夹取 → 写回 g_cfg 并保存
static void cfg_apply(HWND hwnd) {
  double v;
  v = cfg_double(hwnd, IDC_BASE_SPEED, g_cfg.baseSpeed);
  if (v > 0) g_cfg.baseSpeed = v;
  v = cfg_int(hwnd, IDC_INIT_CAP, g_cfg.initialBlockCap);
  if (v >= 1 && v <= MAX_BLOCKS) g_cfg.initialBlockCap = (int)v;
  v = cfg_int(hwnd, IDC_LIFETIME, g_cfg.blockLifetime);
  if (v >= 1) g_cfg.blockLifetime = (int)v;
  v = cfg_double(hwnd, IDC_SHRINK, g_cfg.shrinkFraction);
  if (v > 0 && v < 1) g_cfg.shrinkFraction = v;
  v = cfg_double(hwnd, IDC_URG_T, g_cfg.urgencyThreshold);
  if (v >= 0) g_cfg.urgencyThreshold = v;
  v = cfg_double(hwnd, IDC_URG_F, g_cfg.urgencyFactor);
  if (v >= 0) g_cfg.urgencyFactor = v;
  v = cfg_double(hwnd, IDC_FREEZE, g_cfg.endFreezeMs);
  if (v >= 0) g_cfg.endFreezeMs = v;
  v = cfg_double(hwnd, IDC_BLINK, g_cfg.blinkPeriodSec);
  if (v >= 0.5) g_cfg.blinkPeriodSec = v;
  v = cfg_int(hwnd, IDC_CELL, g_cfg.cellSizePx);
  if (v >= 4 && v <= 64) g_cfg.cellSizePx = (int)v;
  int sum = 0;
  for (int i = 0; i < 7; i++) {
    v = cfg_int(hwnd, IDC_W1 + i, g_cfg.weights[i]);
    if (v < 0) v = 0;
    g_cfg.weights[i] = (int)v;
    sum += (int)v;
  }
  if (sum == 0) memcpy(g_cfg.weights, params_default().weights, sizeof g_cfg.weights);
  config_save();
}

static LRESULT CALLBACK CfgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      char b[32];
      int y = 12;
      const char *names[7] = { "red", "green", "blue", "yellow", "purple", "cyan", "white" };
      snprintf(b, sizeof b, "%g", g_cfg.baseSpeed);
      cfg_row(hwnd, IDC_BASE_SPEED, "Base speed (cells/s)", b, 16, y); y += CFG_ROWH;
      snprintf(b, sizeof b, "%d", g_cfg.initialBlockCap);
      cfg_row(hwnd, IDC_INIT_CAP, "Initial block cap", b, 16, y); y += CFG_ROWH;
      snprintf(b, sizeof b, "%d", g_cfg.blockLifetime);
      cfg_row(hwnd, IDC_LIFETIME, "Block lifetime (sec)", b, 16, y); y += CFG_ROWH;
      snprintf(b, sizeof b, "%g", g_cfg.shrinkFraction);
      cfg_row(hwnd, IDC_SHRINK, "Shrink per timeout", b, 16, y); y += CFG_ROWH;
      snprintf(b, sizeof b, "%g", g_cfg.urgencyThreshold);
      cfg_row(hwnd, IDC_URG_T, "Urgency threshold (sec)", b, 16, y); y += CFG_ROWH;
      snprintf(b, sizeof b, "%g", g_cfg.urgencyFactor);
      cfg_row(hwnd, IDC_URG_F, "Urgency factor", b, 16, y); y += CFG_ROWH;
      snprintf(b, sizeof b, "%g", g_cfg.endFreezeMs);
      cfg_row(hwnd, IDC_FREEZE, "End freeze (sec)", b, 16, y); y += CFG_ROWH;
      snprintf(b, sizeof b, "%g", g_cfg.blinkPeriodSec);
      cfg_row(hwnd, IDC_BLINK, "Blink period (sec)", b, 16, y);
      // 右列：7 种占比 + 单元格
      y = 12;
      char lbl[32], val[32];
      for (int i = 0; i < 7; i++) {
        snprintf(lbl, sizeof lbl, "Weight #%d %s", i + 1, names[i]);
        snprintf(val, sizeof val, "%d", g_cfg.weights[i]);
        cfg_row(hwnd, IDC_W1 + i, lbl, val, 300, y);
        y += CFG_ROWH;
      }
      snprintf(b, sizeof b, "%d", g_cfg.cellSizePx);
      cfg_row(hwnd, IDC_CELL, "Cell size (px)", b, 300, y);
      CreateWindowExA(0, "button", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      16, y + CFG_ROWH + 4, 80, 26, hwnd, (HMENU)IDC_OK, g_hInst, NULL);
      CreateWindowExA(0, "button", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      104, y + CFG_ROWH + 4, 80, 26, hwnd, (HMENU)IDC_CANCEL, g_hInst, NULL);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDC_OK) { cfg_apply(hwnd); DestroyWindow(hwnd); }
      else if (LOWORD(wp) == IDC_CANCEL) DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

// ---- 预览窗口（/p，MVP 静态帧）----
static LRESULT CALLBACK PrevProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      draw_game(dc, rc.right, rc.bottom, 0);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

static void register_classes(void) {
  WNDCLASSA wc = {0};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.hInstance = g_hInst;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = "SnakeSaverClass";
  wc.lpfnWndProc = WndProc;
  RegisterClassA(&wc);
  wc.lpszClassName = "SnakeSaverCfgClass";
  wc.lpfnWndProc = CfgProc;
  RegisterClassA(&wc);
  wc.lpszClassName = "SnakeSaverPrevClass";
  wc.lpfnWndProc = PrevProc;
  RegisterClassA(&wc);
}

static int g_dialogKeys = 0; // 配置窗口启用 Tab/Enter/Esc 键盘导航
static void loop(HWND hwnd) {
  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    if (g_dialogKeys && IsDialogMessage(hwnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

static void run_screensaver(void) {
  g_saverMode = !g_windowed;
  config_load();
  int sw = g_saverMode ? GetSystemMetrics(SM_CXSCREEN) : 720;
  int sh = g_saverMode ? GetSystemMetrics(SM_CYSCREEN) : 540;
  g_cols = sw / g_cell;
  g_rows = sh / g_cell;
  stats_load();
  game_init(&g_game, g_cols, g_rows, (unsigned)GetTickCount());
  { Params p = cfg_to_params(); game_set_params(&g_game, &p); game_reset(&g_game); }

  DWORD ex = g_saverMode ? WS_EX_TOPMOST : 0;
  DWORD style = g_saverMode ? WS_POPUP : WS_OVERLAPPEDWINDOW;
  g_hwnd = CreateWindowExA(ex, "SnakeSaverClass", "Snake Screensaver", style,
                           0, 0, sw, sh, NULL, NULL, g_hInst, NULL);
  DBG("[main] hwnd=%p saver=%d\n", (void *)g_hwnd, g_saverMode);
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);
  if (g_saverMode) {
    SystemParametersInfo(SPI_SETSCREENSAVERRUNNING, TRUE, NULL, 0);
    ShowCursor(FALSE);
  }
  SetTimer(g_hwnd, 1, 25, NULL);
  DBG("[main] entering loop\n");
  loop(g_hwnd);
  DBG("[main] loop exited\n");
}

static void run_config(void) {
  config_load();
  HWND hwnd = CreateWindowExA(0, "SnakeSaverCfgClass", "Snake Screensaver Settings",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT, 570, 336,
                              NULL, NULL, g_hInst, NULL);
  ShowWindow(hwnd, SW_SHOW);
  g_dialogKeys = 1;
  loop(hwnd);
  g_dialogKeys = 0;
}

static void run_preview(HWND parent) {
  RECT rc;
  GetClientRect(parent, &rc);
  if (rc.right < 10 || rc.bottom < 10) { rc.right = 240; rc.bottom = 180; }
  config_load();
  g_cols = rc.right / g_cell;
  g_rows = rc.bottom / g_cell;
  game_init(&g_game, g_cols, g_rows, 12345u);
  { Params p = cfg_to_params(); game_set_params(&g_game, &p); game_reset(&g_game); }
  HWND hwnd = CreateWindowExA(0, "SnakeSaverPrevClass", "", WS_CHILD | WS_VISIBLE,
                              0, 0, rc.right, rc.bottom, parent, NULL, g_hInst, NULL);
  UpdateWindow(hwnd);
  loop(hwnd);
}

static void parse_args(void) {
  char cmd[1024];
  char *src = GetCommandLineA();
  strncpy(cmd, src, sizeof(cmd) - 1);
  cmd[sizeof(cmd) - 1] = 0;
  // 按空格分词并精确匹配独立参数，避免路径中误含 "/c" 等子串
  for (char *t = strtok(cmd, " \t"); t; t = strtok(NULL, " \t")) {
    int len = (int)strlen(t);
    if (len >= 2 && (t[0] == '"' || t[len - 1] == '"')) { t[len - 1] = 0; t++; }
    for (char *q = t; *q; q++) *q = (char)tolower((unsigned char)*q);
    if (strcmp(t, "/c") == 0) g_mode = 2;
    else if (strcmp(t, "/p") == 0) g_mode = 3;
    else if (strcmp(t, "/s") == 0) g_mode = 1;
    else if (strcmp(t, "--windowed") == 0) g_windowed = 1;
  }
  if (g_mode == 0) g_mode = 1; // /s 或无参数 = 正式运行
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
  (void)hPrev; (void)lpCmd; (void)nShow;
  g_hInst = hInst;
  register_classes();
  parse_args();
  if (g_mode == 2) run_config();
  else if (g_mode == 3) {
    // Windows 传 /p <hwnd>：取最后一个 token 作为父窗口
    char *cmd = GetCommandLineA();
    char *last = NULL, *tok = cmd;
    while (tok && *tok) {
      if (tok[0] == '/' || tok[0] == ' ' || tok[0] == '"') { tok++; continue; }
      char *sp = strchr(tok, ' ');
      last = tok;
      tok = sp ? sp + 1 : NULL;
    }
    HWND parent = NULL;
    if (last) parent = (HWND)(INT_PTR)strtol(last, NULL, 10);
    run_preview(parent);
  } else {
    run_screensaver();
  }
  return 0;
}
