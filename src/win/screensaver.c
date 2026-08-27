// 原生 Win32 屏幕保护程序壳：/s 全屏、/c 配置、/p 预览、鼠标/键盘/点击退出、GDI 渲染。
// 编译：gcc src/win/screensaver.c src/win/game.c -o SnakeScreensaver.scr -Os -s -mwindows
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "game.h"

#define CELL 10 // 单元格边长（px）

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
  RECT top = {0, 0, w, r.y0 * CELL};
  RECT bot = {0, r.y1 * CELL, w, h - r.y1 * CELL};
  RECT lef = {0, r.y0 * CELL, r.x0 * CELL, (r.y1 - r.y0) * CELL};
  RECT rig = {r.x1 * CELL, r.y0 * CELL, w - r.x1 * CELL, (r.y1 - r.y0) * CELL};
  FillRect(dc, &top, wall);
  FillRect(dc, &bot, wall);
  FillRect(dc, &lef, wall);
  FillRect(dc, &rig, wall);
  DeleteObject(wall);

  // 网格线
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(20, 20, 20));
  HGDIOBJ oldPen = SelectObject(dc, pen);
  for (int x = r.x0; x <= r.x1; x++) { MoveToEx(dc, x * CELL, r.y0 * CELL, NULL); LineTo(dc, x * CELL, r.y1 * CELL); }
  for (int y = r.y0; y <= r.y1; y++) { MoveToEx(dc, r.x0 * CELL, y * CELL, NULL); LineTo(dc, r.x1 * CELL, y * CELL); }
  SelectObject(dc, oldPen);
  DeleteObject(pen);

  // 方块（闪烁：sin 相位决定亮/暗，剩余 <10s 加快）
  for (int i = 0; i < g_game.blockCount; i++) {
    Block *b = &g_game.blocks[i];
    double period = b->remaining < 10 ? 0.25 : 1.0;
    COLORREF c = COLORS[b->kind - 1];
    if (sin(2 * 3.14159265358979 * nowMs / 1000.0 / period) < 0) c = dim(c);
    HBRUSH br = CreateSolidBrush(c);
    RECT rr = {b->x * CELL + 1, b->y * CELL + 1, (b->x + 1) * CELL - 1, (b->y + 1) * CELL - 1};
    FillRect(dc, &rr, br);
    DeleteObject(br);
  }

  // 蛇（尾先画）
  for (int i = g_game.snake.len - 1; i >= 0; i--) {
    Point p = g_game.snake.seg[i];
    HBRUSH br = CreateSolidBrush(COLORS[g_game.snake.color[i]]);
    RECT rr = {p.x * CELL + 1, p.y * CELL + 1, (p.x + 1) * CELL - 1, (p.y + 1) * CELL - 1};
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

// ---- 配置窗口（/c，MVP 最小版）----
static LRESULT CALLBACK CfgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      const char *lines[] = {
        "Snake Screensaver - config",
        "MVP minimal config (read-only):",
        "Base speed : 10 cells/sec",
        "Initial blocks : 3",
        "Block lifetime : 60 sec",
        "Shrink per timeout : 1/10 area",
        "",
        "Click Close or press Esc to exit.",
      };
      int n = sizeof(lines) / sizeof(lines[0]);
      for (int i = 0; i < n; i++) {
        CreateWindowExA(0, "static", lines[i], WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, 14 + i * 22, 380, 20, hwnd, NULL, g_hInst, NULL);
      }
      CreateWindowExA(0, "button", "Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      16, 14 + n * 22 + 8, 100, 26, hwnd, (HMENU)1, g_hInst, NULL);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == 1) DestroyWindow(hwnd);
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

static void loop(HWND hwnd) {
  (void)hwnd;
  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

static void run_screensaver(void) {
  g_saverMode = !g_windowed;
  int sw = g_saverMode ? GetSystemMetrics(SM_CXSCREEN) : 720;
  int sh = g_saverMode ? GetSystemMetrics(SM_CYSCREEN) : 540;
  g_cols = sw / CELL;
  g_rows = sh / CELL;
  stats_load();
  game_init(&g_game, g_cols, g_rows, (unsigned)GetTickCount());

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
  HWND hwnd = CreateWindowExA(0, "SnakeSaverCfgClass", "Snake Screensaver Settings",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT, 420, 300,
                              NULL, NULL, g_hInst, NULL);
  ShowWindow(hwnd, SW_SHOW);
  loop(hwnd);
}

static void run_preview(HWND parent) {
  RECT rc;
  GetClientRect(parent, &rc);
  if (rc.right < 10 || rc.bottom < 10) { rc.right = 240; rc.bottom = 180; }
  g_cols = rc.right / CELL;
  g_rows = rc.bottom / CELL;
  game_init(&g_game, g_cols, g_rows, 12345u);
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
