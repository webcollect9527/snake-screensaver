// 贪吃蛇屏保 - 纯游戏逻辑（从 TypeScript 版逐行移植，算法/参数与 CLAUDE.md 一致）
// 不依赖 Win32/平台 API，可在无 GUI 环境单测（见 game_test.c）。
#ifndef GAME_H
#define GAME_H

#define MAX_BLOCKS 256   // 同屏方块上限的硬顶（效果 5 每吃一次 +1，clamp 到此值）
#define MAX_GRID 4096    // 单边最大格子数（防异常 DPI）

typedef struct { int x, y; } Point;

typedef struct { int x0, y0, x1, y1; } Rect;

typedef struct {
  int w, h;
  Rect operable;
} Grid;

typedef struct {
  Point *seg;   // 头在前 seg[0]
  int *color;   // 每节颜色索引（0..6）
  int len, cap;
  Point dir;
} Snake;

typedef struct {
  int x, y;
  int kind;       // 1..7
  double remaining; // 剩余秒
} Block;

typedef struct {
  int speedUp, speedDown;
  int lifetime;   // 秒
  int blockCap;
} Effects;

typedef struct {
  Grid grid;
  Snake snake;
  Effects fx;
  Block blocks[MAX_BLOCKS];
  int blockCount;
  int score;
  int colorCounts[7];
  int state;      // 0=running 1=over
  int reason;     // 0=none 1=success 2=wall 3=self 4=area
  double survival;
  double overTimer;
  int pendingShrink;
  double accumMs;
  unsigned rngState;
  // 寻路/缩场工作区（game_init 时按 w*h 分配）
  int *dist, *prev, *queue;
  unsigned char *visited;
  int *fstack;
  Point *tmpBody;
  int *path;
} Game;

void game_init(Game *g, int w, int h, unsigned seed);
void game_free(Game *g);
void game_reset(Game *g);
void game_update(Game *g, double dtMs);

// 供外部渲染/测试读取
int game_is_over(Game *g);
Point game_head(Game *g);

#endif
