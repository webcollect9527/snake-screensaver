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
// 离屏双缓冲：整帧先在内存 DC 画好，再一次性 BitBlt 上屏，避免清屏/绘制过程暴露到屏幕造成黑屏闪
static HDC s_memDc = NULL;
static HBITMAP s_memBmp = NULL;
static int s_memW = 0, s_memH = 0;
// 常亮元素（围墙/网格/HUD，除蛇与时钟）的呼吸亮度倍率，每帧由 paint_frame 计算（滞后方块 2s）
static double s_frameK = 1.0;

// tcc 自带头文件较老，缺少的宏/函数在此补齐
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#define NOW_MS ((double)GetTickCount())

// ---- AlphaBlend（半透明）：tcc 无 msimg32 导入库，运行时从 msimg32.dll 加载 ----
typedef BOOL(WINAPI *AlphaBlendProc)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
static AlphaBlendProc s_alphaBlend;
static BOOL alpha_ready(void) {
  if (!s_alphaBlend) {
    HMODULE h = LoadLibraryA("msimg32.dll");
    if (h) s_alphaBlend = (AlphaBlendProc)(void *)GetProcAddress(h, "AlphaBlend");
  }
  return s_alphaBlend != NULL;
}

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
static HWND g_parentHwnd = NULL; // Windows 设置对话框传入的父窗口句柄（/c:<hwnd>、/p <hwnd>）
static long g_lastX = -1, g_lastY = -1, g_moveDist = 0;
static int g_ended = 0;
static int g_timerMs = 16; // 逻辑/渲染定时器间隔（ms），启动时按显示器刷新率对齐（60Hz→16，120Hz→8）

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
}

static void config_save(void) {
  char dir[MAX_PATH];
  snprintf(dir, sizeof dir, "%s\\snake-screensaver", getenv("APPDATA") ? getenv("APPDATA") : "");
  CreateDirectoryA(dir, NULL);
  FILE *f = fopen(g_cfgPath, "wb");
  if (!f) return;
  fprintf(f, "{\n");
  fprintf(f, "  \"cellSizePx\": %d,\n", g_cfg.cellSizePx);
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

// ---- 渲染（整帧画进内存 DC，由 paint_frame 一次上屏）----
// 亮度倍率：k=1 满色，k 越小越暗
static COLORREF dim_color(COLORREF c, double k) {
  return RGB((int)(GetRValue(c) * k), (int)(GetGValue(c) * k), (int)(GetBValue(c) * k));
}
// 方块呼吸系数：10s 周期（亮→暗 5s、暗→亮 5s），k∈[0.15,1.0]，最暗仍可见
static double breath_k(double secs) {
  return 0.15 + 0.85 * (0.5 + 0.5 * cos(2 * 3.14159265358979 * secs / 10.0));
}

// 复古点阵 5x7 电子表字库（每字符 7 行，bit4..bit0 = 左..右）
static const unsigned char FONT5x7[10][7] = {
  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
  {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
  {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
  {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
  {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
  {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
  {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
  {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
};

// 数字时钟：左上角，距上/左各 2 个单元格；颜色 = 蛇头颜色；呼吸豁免
static void draw_clock(HDC dc) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char time[16];
  snprintf(time, sizeof time, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
  COLORREF c = COLORS[g_game.snake.color[0]]; // 与蛇头颜色一致（随吃到颜色变化）
  const int dot = 2, gap = 1, dx = dot + gap, dy = dot + gap; // 点 2px、间距 1px
  int x = 2 * g_cell, y0 = 2 * g_cell;
  HBRUSH br = CreateSolidBrush(c);
  for (char *p = time; *p; p++) {
    if (*p == ':') { // 冒号 2 点宽
      for (int r = 0; r < 7; r++) {
        if (r == 2 || r == 5) {
          RECT rr = {x, y0 + r * dy, x + dot, y0 + r * dy + dot};
          FillRect(dc, &rr, br);
        }
      }
      x += 2 * dx - gap;
    } else {
      const unsigned char *pat = FONT5x7[*p - '0'];
      for (int r = 0; r < 7; r++)
        for (int col = 0; col < 5; col++)
          if (pat[r] & (1 << (4 - col))) {
            RECT rr = {x + col * dx, y0 + r * dy, x + col * dx + dot, y0 + r * dy + dot};
            FillRect(dc, &rr, br);
          }
      x += 5 * dx - gap; // 数字 14px 宽；6 位 + 2 冒号共 94px ≤ 10 个单元格
    }
  }
  DeleteObject(br);
}

static void draw_game(HDC dc, int w, int h, double nowMs) {
  Rect r = g_game.grid.operable;

  // 围墙（可操作区域外，参与呼吸，滞后方块 2s）
  HBRUSH wall = CreateSolidBrush(dim_color(WALL_COLOR, s_frameK));
  RECT top = {0, 0, w, r.y0 * g_cell};
  RECT bot = {0, r.y1 * g_cell, w, h - r.y1 * g_cell};
  RECT lef = {0, r.y0 * g_cell, r.x0 * g_cell, (r.y1 - r.y0) * g_cell};
  RECT rig = {r.x1 * g_cell, r.y0 * g_cell, w - r.x1 * g_cell, (r.y1 - r.y0) * g_cell};
  FillRect(dc, &top, wall);
  FillRect(dc, &bot, wall);
  FillRect(dc, &lef, wall);
  FillRect(dc, &rig, wall);
  DeleteObject(wall);

  // 网格线（参与呼吸，滞后方块 2s）
  HPEN pen = CreatePen(PS_SOLID, 1, dim_color(RGB(20, 20, 20), s_frameK));
  HGDIOBJ oldPen = SelectObject(dc, pen);
  for (int x = r.x0; x <= r.x1; x++) { MoveToEx(dc, x * g_cell, r.y0 * g_cell, NULL); LineTo(dc, x * g_cell, r.y1 * g_cell); }
  for (int y = r.y0; y <= r.y1; y++) { MoveToEx(dc, r.x0 * g_cell, y * g_cell, NULL); LineTo(dc, r.x1 * g_cell, y * g_cell); }
  SelectObject(dc, oldPen);
  DeleteObject(pen);

  // 方块：呼吸式缓慢明暗（周期 10s：亮→暗 5s、暗→亮 5s）；剩余 <10s 快速闪烁预警
  double secs = nowMs / 1000.0;
  for (int i = 0; i < g_game.blockCount; i++) {
    Block *b = &g_game.blocks[i];
    COLORREF full = COLORS[b->kind - 1];
    double k; // 亮度倍率（1=满色，0.15=最暗仍可见）
    if (b->remaining < 10) {
      k = (sin(2 * 3.14159265358979 * secs / 0.5) < 0) ? 0.15 : 1.0; // 临期快速闪烁
    } else {
      k = breath_k(secs); // 10s 呼吸，起点最亮
    }
    HBRUSH br = CreateSolidBrush(dim_color(full, k));
    RECT rr = {b->x * g_cell + 1, b->y * g_cell + 1, (b->x + 1) * g_cell - 1, (b->y + 1) * g_cell - 1};
    FillRect(dc, &rr, br);
    DeleteObject(br);
  }

  // 蛇（尾先画，蛇自身不参与呼吸）
  for (int i = g_game.snake.len - 1; i >= 0; i--) {
    Point p = g_game.snake.seg[i];
    HBRUSH br = CreateSolidBrush(COLORS[g_game.snake.color[i]]);
    RECT rr = {p.x * g_cell + 1, p.y * g_cell + 1, (p.x + 1) * g_cell - 1, (p.y + 1) * g_cell - 1};
    FillRect(dc, &rr, br);
    DeleteObject(br);
  }

  // 数字时钟（左上角，呼吸豁免）
  draw_clock(dc);
}

static void draw_hud(HDC dc, int w) {
  int rowH = 16, bw = 200, bh = 12 + rowH * 12; // 12 行：7 色 + 得分/速度/时间/最佳/成败
  int bx = w - bw - 12, by = 10;
  // 面板背景半透明（50%）：先画到内存 DC 再 AlphaBlend，文字直接画在主 DC 保持可读
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, bw, bh);
  HGDIOBJ ob = SelectObject(mem, bmp);
  HBRUSH bg = CreateSolidBrush(dim_color(RGB(10, 10, 10), s_frameK)); // HUD 参与呼吸（滞后方块 2s）
  RECT r = {0, 0, bw, bh};
  FillRect(mem, &r, bg);
  DeleteObject(bg);
  HPEN pen = CreatePen(PS_SOLID, 1, dim_color(RGB(70, 70, 70), s_frameK));
  HGDIOBJ op = SelectObject(mem, pen);
  SelectObject(mem, GetStockObject(NULL_BRUSH));
  Rectangle(mem, 0, 0, bw, bh);
  SelectObject(mem, op);
  DeleteObject(pen);
  if (alpha_ready()) {
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 128, 0 }; // 50% 透明
    s_alphaBlend(dc, bx, by, bw, bh, mem, 0, 0, bw, bh, bf);
  } else {
    BitBlt(dc, bx, by, bw, bh, mem, 0, 0, SRCCOPY);
  }
  SelectObject(mem, ob);
  DeleteObject(bmp);
  DeleteDC(mem);

  HFONT f = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
  HGDIOBJ of = SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, dim_color(RGB(230, 230, 230), s_frameK));
  int ly = by + 8;
  char buf[64];
  for (int i = 0; i < 7; i++) {
    HBRUSH cbr = CreateSolidBrush(dim_color(COLORS[i], s_frameK));
    RECT sq = {bx + 10, ly + 3, bx + 20, ly + 13};
    FillRect(dc, &sq, cbr);
    DeleteObject(cbr);
    snprintf(buf, sizeof buf, "x%d", g_game.colorCounts[i]);
    TextOutA(dc, bx + 26, ly, buf, (int)strlen(buf));
    ly += rowH;
  }
  snprintf(buf, sizeof buf, "Score %d", g_game.score);
  TextOutA(dc, bx + 10, ly, buf, (int)strlen(buf)); ly += rowH;
  snprintf(buf, sizeof buf, "Speed %.1f", game_speed(&g_game));
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

// 复用离屏缓冲（窗口尺寸变化时重建，避免每次重绘分配全屏位图）
static HDC ensure_memdc(HDC ref, int w, int h) {
  if (s_memDc && s_memW == w && s_memH == h) return s_memDc;
  if (s_memBmp) DeleteObject(s_memBmp);
  if (s_memDc) DeleteDC(s_memDc);
  s_memDc = CreateCompatibleDC(ref);
  s_memBmp = CreateCompatibleBitmap(ref, w, h);
  if (s_memDc) SelectObject(s_memDc, s_memBmp);
  s_memW = w; s_memH = h;
  return s_memDc;
}

// 离屏双缓冲渲染：清背景→画游戏→画 HUD 全在内存 DC 完成，最后一次性 BitBlt 上屏
static void paint_frame(HWND hwnd, int showHud, double nowMs) {
  PAINTSTRUCT ps;
  HDC wdc = BeginPaint(hwnd, &ps);
  RECT rc;
  GetClientRect(hwnd, &rc);
  int w = rc.right, h = rc.bottom;
  if (w > 0 && h > 0) {
    HDC mdc = ensure_memdc(wdc, w, h);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    RECT all = {0, 0, w, h};
    FillRect(mdc, &all, black);
    DeleteObject(black);
    s_frameK = breath_k(nowMs / 1000.0 - 2.0); // 常亮元素呼吸，滞后方块 2s
    draw_game(mdc, w, h, nowMs);
    if (showHud) draw_hud(mdc, w);
    BitBlt(wdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
  }
  EndPaint(hwnd, &ps);
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
    case WM_ERASEBKGND:
      return 1; // 禁止系统用背景刷先擦黑，避免每次重绘前出现黑屏闪（整帧由离屏缓冲完整覆盖）
    case WM_TIMER:
      game_update(&g_game, g_timerMs);
      record_stats_if_ended();
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_PAINT:
      paint_frame(hwnd, 1, NOW_MS);
      return 0;
    case WM_DESTROY:
      DBG("[wnd] WM_DESTROY\n");
      KillTimer(hwnd, 1);
      if (s_memBmp) DeleteObject(s_memBmp);
      if (s_memDc) DeleteDC(s_memDc);
      s_memBmp = NULL; s_memDc = NULL;
      if (g_saverMode) {
        ShowCursor(TRUE);
        SystemParametersInfo(SPI_SETSCREENSAVERRUNNING, FALSE, NULL, 0);
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

// ---- 配置窗口（/c，设置窗口：可调参数 + 方块占比）----
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
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint_frame(hwnd, 0, 0);
      return 0;
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
  // 定时器间隔对齐显示器刷新率（对应 requestAnimationFrame 对齐刷新）
  DEVMODEA dm = {0};
  dm.dmSize = sizeof dm;
  int hz = 60;
  if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30) hz = dm.dmDisplayFrequency;
  g_timerMs = 1000 / hz;
  SetTimer(g_hwnd, 1, g_timerMs, NULL);
  DBG("[main] entering loop (timer=%dms)\n", g_timerMs);
  loop(g_hwnd);
  DBG("[main] loop exited\n");
}

static void run_config(void) {
  config_load();
  HWND hwnd = CreateWindowExA(0, "SnakeSaverCfgClass", "Snake Screensaver Settings",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT, 570, 336,
                              g_parentHwnd, NULL, g_hInst, NULL); // 归属设置对话框（若有）
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
  // Windows 屏保协议：/s、/c、/p <hwnd>；也兼容冒号形式 /c:<hwnd>、/p:<hwnd>（设置对话框可能这样传参）
  int expectParent = 0;
  for (char *t = strtok(cmd, " \t"); t; t = strtok(NULL, " \t")) {
    int len = (int)strlen(t);
    if (len >= 2 && (t[0] == '"' || t[len - 1] == '"')) { t[len - 1] = 0; t++; }
    for (char *q = t; *q; q++) *q = (char)tolower((unsigned char)*q);
    if (strncmp(t, "/c", 2) == 0 && (t[2] == 0 || t[2] == ':')) {
      g_mode = 2;
      if (t[2] == ':') g_parentHwnd = (HWND)(INT_PTR)strtol(t + 3, NULL, 10); // /c:<hwnd> 归属设置对话框
    }
    else if (strncmp(t, "/p", 2) == 0 && (t[2] == 0 || t[2] == ':')) {
      g_mode = 3;
      if (t[2] == ':') { g_parentHwnd = (HWND)(INT_PTR)strtol(t + 3, NULL, 10); expectParent = 0; }
      else expectParent = 1; // /p <hwnd> 空格形式：下一 token 是父窗口句柄
    }
    else if (strncmp(t, "/s", 2) == 0 && (t[2] == 0 || t[2] == ':')) g_mode = 1;
    else if (strcmp(t, "--windowed") == 0) g_windowed = 1;
    if (expectParent) {
      g_parentHwnd = (HWND)(INT_PTR)strtol(t, NULL, 10);
      expectParent = 0;
    }
  }
  // 无参数（双击/右键"测试"）→ 窗口化测试，避免直接弹出全屏屏保；正式激活走 /s 全屏
  if (g_mode == 0) { g_mode = 1; g_windowed = 1; }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
  (void)hPrev; (void)lpCmd; (void)nShow;
  g_hInst = hInst;
  register_classes();
  parse_args();
  if (g_mode == 2) run_config();
  else if (g_mode == 3) {
    run_preview(g_parentHwnd); // 父窗口句柄已在 parse_args 解析（/p <hwnd> 或 /p:<hwnd>）
  } else {
    run_screensaver();
  }
  return 0;
}
