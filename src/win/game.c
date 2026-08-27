// 纯游戏逻辑实现（TypeScript 版逐行移植）。全部可调参数在此集中管理，禁止魔法数字散落。
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "game.h"

// ---- 可调参数（与 src/config.ts 一致；运行时可被 /c 配置覆盖）----
Params params_default(void) {
  Params p;
  p.baseSpeed = 10.0;        // 基础速度：格/秒
  p.initialBlockCap = 3;     // 初始同屏方块数上限
  p.blockLifetime = 60;      // 方块初始生存秒数
  p.blockCapMin = 1;
  p.lifetimeMin = 1;
  p.shrinkFraction = 0.1;    // 场地每次缩小比例
  p.urgencyThreshold = 5.0;  // 临期方块判定（松弛 <5s）
  p.urgencyFactor = 3.0;
  p.speedUpRate = 0.01;
  p.speedDownRate = 0.01;
  p.endFreezeMs = 3000.0;
  int w[7] = { 10, 9, 4, 4, 3, 3, 1 };
  memcpy(p.weights, w, sizeof w);
  return p;
}

void game_set_params(Game *g, const Params *p) {
  g->params = *p;
  if (g->params.initialBlockCap < 1) g->params.initialBlockCap = 1;
  if (g->params.initialBlockCap > MAX_BLOCKS) g->params.initialBlockCap = MAX_BLOCKS;
  if (g->params.blockLifetime < g->params.lifetimeMin) g->params.blockLifetime = g->params.lifetimeMin;
  if (g->params.shrinkFraction <= 0 || g->params.shrinkFraction >= 1) g->params.shrinkFraction = 0.1;
  if (g->params.endFreezeMs < 0) g->params.endFreezeMs = 0;
  int total = 0;
  for (int i = 0; i < 7; i++) {
    if (g->params.weights[i] < 0) g->params.weights[i] = 0;
    total += g->params.weights[i];
  }
  if (total == 0) { int d[7] = { 10, 9, 4, 4, 3, 3, 1 }; memcpy(g->params.weights, d, sizeof d); }
}

static const int SCORES[7] = { 1, 2, 1, 2, 5, 1, 5 };

static const Point DIRS[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

// ---- 随机（mulberry32，与 TS 测试同算法）----
static unsigned rng_raw(Game *g) {
  g->rngState += 0x6D2B79F5u;
  unsigned t = g->rngState;
  t = (t ^ (t >> 15)) * (t | 1u);
  t ^= t + (t ^ (t >> 7)) * (t | 61u);
  return t ^ (t >> 14);
}
static double rng_unit(Game *g) { return rng_raw(g) / 4294967296.0; }
static int rng_int(Game *g, int n) { return n <= 0 ? 0 : (int)(rng_unit(g) * n); }

// ---- Grid ----
static int is_wall(const Grid *g, int x, int y) {
  const Rect *r = &g->operable;
  return x < r->x0 || x >= r->x1 || y < r->y0 || y >= r->y1;
}
static int contains(const Grid *g, int x, int y) {
  return x >= 0 && x < g->w && y >= 0 && y < g->h;
}
static int area(const Grid *g) {
  const Rect *r = &g->operable;
  return (r->x1 - r->x0) * (r->y1 - r->y0);
}

// ---- Snake ----
static int snake_collides(const Snake *s, int grow) {
  Point nh = { s->seg[0].x + s->dir.x, s->seg[0].y + s->dir.y };
  int skip = grow ? 0 : 1;
  for (int i = 0; i < s->len - skip; i++)
    if (s->seg[i].x == nh.x && s->seg[i].y == nh.y) return 1;
  return 0;
}

// 走一步。grow=true 时长度 +1，新增头部单元颜色 = newColor（被吃方块颜色）。
static Point snake_step(Snake *s, Point dir, int grow, int newColor) {
  s->dir = dir;
  Point nh = { s->seg[0].x + dir.x, s->seg[0].y + dir.y };
  int headColor = s->color[0];
  if (grow) {
    for (int i = s->len; i >= 1; i--) { s->seg[i] = s->seg[i-1]; s->color[i] = s->color[i-1]; }
    s->len++;
  } else {
    for (int i = s->len - 1; i >= 1; i--) { s->seg[i] = s->seg[i-1]; s->color[i] = s->color[i-1]; }
  }
  s->seg[0] = nh;
  s->color[0] = grow ? newColor : headColor;
  return nh;
}

// ---- Effects ----
static double speed_of(const Game *g) {
  const Effects *fx = &g->fx;
  // 对数计数避免浮点连乘误差：base * (1+rate)^up * (1-rate)^down
  double ln = log(g->params.baseSpeed)
            + fx->speedUp * log(1 + g->params.speedUpRate)
            + fx->speedDown * log(1 - g->params.speedDownRate);
  return exp(ln);
}

static void effects_apply(Effects *fx, int kind, Game *g) {
  int eff;
  if (kind == 7) eff = 1 + rng_int(g, 6);
  else eff = kind;
  switch (eff) {
    case 1: fx->speedUp++; break;
    case 2: fx->speedDown++; break;
    case 3: fx->lifetime += 1; break;
    case 4: if (fx->lifetime > g->params.lifetimeMin) fx->lifetime--; break;
    case 5: if (fx->blockCap < MAX_BLOCKS - 1) fx->blockCap++; break;
    case 6: if (fx->blockCap > g->params.blockCapMin) fx->blockCap--; break;
  }
}

// 按占比抽样方块类型（1..7）
static int pick_kind(Game *g) {
  int total = 0;
  for (int i = 0; i < 7; i++) total += g->params.weights[i];
  int r = (int)(rng_unit(g) * total);
  for (int i = 0; i < 7; i++) {
    r -= g->params.weights[i];
    if (r < 0) return i + 1;
  }
  return 7;
}

// ---- 寻路（BFS + flood fill）----

// BFS：dist=-1 未达，-2 身体，>=0 距离；prev=前驱格索引（头 -1）。
static void bfs_dist(Game *g) {
  int w = g->grid.w, h = g->grid.h, n = w * h;
  for (int i = 0; i < n; i++) { g->dist[i] = -1; g->prev[i] = -1; }
  Snake *s = &g->snake;
  for (int i = 1; i < s->len; i++) {
    Point p = s->seg[i];
    if (p.x >= 0 && p.x < w && p.y >= 0 && p.y < h) g->dist[p.y * w + p.x] = -2;
  }
  Point head = s->seg[0];
  int si = head.y * w + head.x;
  g->dist[si] = 0;
  int qh = 0, qt = 0;
  g->queue[qt++] = si;
  while (qh < qt) {
    int ci = g->queue[qh++];
    int cx = ci % w, cy = ci / w;
    int nd = g->dist[ci] + 1;
    for (int d = 0; d < 4; d++) {
      int nx = cx + DIRS[d].x, ny = cy + DIRS[d].y;
      if (!contains(&g->grid, nx, ny) || is_wall(&g->grid, nx, ny)) continue;
      int ni = ny * w + nx;
      if (g->dist[ni] != -1) continue;
      g->dist[ni] = nd;
      g->prev[ni] = ci;
      g->queue[qt++] = ni;
    }
  }
}

// flood fill：返回 start 可达空格数；body[1..len-1] 视为占用（body 可含 start 之外任意格）。
static int flood_fill(Game *g, Point start, Point *body, int bodyLen) {
  int w = g->grid.w, h = g->grid.h, n = w * h;
  memset(g->visited, 0, n);
  for (int i = 1; i < bodyLen; i++) {
    Point p = body[i];
    if (p.x >= 0 && p.x < w && p.y >= 0 && p.y < h) g->visited[p.y * w + p.x] = 1;
  }
  if (is_wall(&g->grid, start.x, start.y)) return 0;
  int si = start.y * w + start.x;
  if (g->visited[si]) return 0;
  int count = 0, top = 0;
  g->visited[si] = 2;
  g->fstack[top++] = si;
  while (top > 0) {
    int ci = g->fstack[--top];
    count++;
    int cx = ci % w, cy = ci / w;
    for (int d = 0; d < 4; d++) {
      int nx = cx + DIRS[d].x, ny = cy + DIRS[d].y;
      if (!contains(&g->grid, nx, ny) || is_wall(&g->grid, nx, ny)) continue;
      int ni = ny * w + nx;
      if (g->visited[ni]) continue;
      g->visited[ni] = 2;
      g->fstack[top++] = ni;
    }
  }
  return count;
}

// 模拟蛇沿 path（头→目标）走到目标（不增长），检查到达后是否有逃生空间。
static int is_target_safe(Game *g, int targetIdx) {
  int w = g->grid.w;
  int pathLen = 0;
  int cur = targetIdx;
  while (cur != -1) { g->path[pathLen++] = cur; cur = g->prev[cur]; }
  for (int i = 0, j = pathLen - 1; i < j; i++, j--) {
    int t = g->path[i]; g->path[i] = g->path[j]; g->path[j] = t;
  }
  int len = g->snake.len;
  for (int i = 0; i < len; i++) g->tmpBody[i] = g->snake.seg[i];
  for (int i = 1; i < pathLen; i++) {
    for (int k = len; k >= 1; k--) g->tmpBody[k] = g->tmpBody[k-1];
    g->tmpBody[0].x = g->path[i] % w;
    g->tmpBody[0].y = g->path[i] / w;
  }
  Point head = g->tmpBody[0];
  int area = flood_fill(g, head, g->tmpBody, len);
  return area >= len; // 可达空格数 < 蛇长 → 无逃生空间
}

// 逃生模式：贪心选择 flood fill 面积最大的相邻格；平局优先当前方向。
static Point escape_dir(Game *g) {
  Snake *s = &g->snake;
  Point head = s->seg[0];
  Point best = s->dir;
  int bestArea = -1;
  for (int d = 0; d < 4; d++) {
    Point nd = DIRS[d];
    int nx = head.x + nd.x, ny = head.y + nd.y;
    int blocked = is_wall(&g->grid, nx, ny);
    if (!blocked) {
      for (int i = 1; i < s->len && !blocked; i++)
        if (s->seg[i].x == nx && s->seg[i].y == ny) blocked = 1;
    }
    if (blocked) continue;
    g->tmpBody[0] = (Point){nx, ny};
    for (int i = 1; i < s->len; i++) g->tmpBody[i] = s->seg[i-1]; // 模拟走一步（尾部挪走）
    Point start = {nx, ny};
    int area = flood_fill(g, start, g->tmpBody, s->len);
    int prefer = (nd.x == s->dir.x && nd.y == s->dir.y) ? 1 : 0;
    if (area * 2 + prefer > bestArea) { bestArea = area * 2 + prefer; best = nd; }
  }
  return best;
}

// 每步决策：返回下一步方向。
static Point decide(Game *g) {
  int w = g->grid.w, h = g->grid.h;
  bfs_dist(g);
  Point head = g->snake.seg[0];
  int headIdx = head.y * w + head.x;
  int reach[MAX_BLOCKS], rc = 0;
  double rdist[MAX_BLOCKS], rscore[MAX_BLOCKS];
  for (int i = 0; i < g->blockCount; i++) {
    Block *b = &g->blocks[i];
    if (!contains(&g->grid, b->x, b->y)) continue;
    int bi = b->y * w + b->x;
    int d = g->dist[bi];
    if (d > 0) {
      reach[rc] = i;
      rdist[rc] = d;
      double t = d / speed_of(g);
      double slack = b->remaining - t;
      double urgency = slack < g->params.urgencyThreshold ? g->params.urgencyFactor : 1.0;
      rscore[rc] = (SCORES[b->kind - 1] / (d + 1.0)) * urgency;
      rc++;
    }
  }
  if (rc == 0) return escape_dir(g);
  // 评分从高到低（简单插入排序）
  int order[MAX_BLOCKS];
  for (int i = 0; i < rc; i++) {
    int j = i;
    while (j > 0 && rscore[order[j-1]] < rscore[i]) { order[j] = order[j-1]; j--; }
    order[j] = i;
  }
  for (int o = 0; o < rc; o++) {
    int i = order[o];
    Block *b = &g->blocks[reach[i]];
    int bi = b->y * w + b->x;
    if (is_target_safe(g, bi)) {
      // 从头到目标的下一步方向
      int cur = bi;
      while (g->prev[cur] != headIdx) {
        if (g->prev[cur] == -1) break;
        cur = g->prev[cur];
      }
      Point d = { cur % w - head.x, cur / w - head.y };
      return d;
    }
  }
  return escape_dir(g);
}

// ---- 场地缩小（arena）----
static int rect_contains(const Rect *r, int x, int y) {
  return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

// 返回 0=缩场成功，1=场地耗尽判负，2=被阻挡延迟
static int try_shrink(Game *g) {
  const Rect *r = &g->grid.operable;
  int ar = area(&g->grid);
  int w = r->x1 - r->x0, h = r->y1 - r->y0;
  struct Edge { int idx; int depth; } edges[4] = {
    {0, (int)ceil(ar * g->params.shrinkFraction / h)},
    {1, (int)ceil(ar * g->params.shrinkFraction / h)},
    {2, (int)ceil(ar * g->params.shrinkFraction / w)},
    {3, (int)ceil(ar * g->params.shrinkFraction / w)},
  };
  // 随机顺序（Fisher-Yates）
  for (int i = 3; i > 0; i--) {
    int j = rng_int(g, i + 1);
    struct Edge t = edges[i]; edges[i] = edges[j]; edges[j] = t;
  }
  for (int e = 0; e < 4; e++) {
    Rect nr = *r;
    switch (edges[e].idx) {
      case 0: nr.x0 = r->x0 + edges[e].depth; break;
      case 1: nr.x1 = r->x1 - edges[e].depth; break;
      case 2: nr.y0 = r->y0 + edges[e].depth; break;
      case 3: nr.y1 = r->y1 - edges[e].depth; break;
    }
    if (nr.x1 <= nr.x0 || nr.y1 <= nr.y0) continue;
    int ok = 1;
    for (int i = 0; i < g->snake.len && ok; i++)
      if (!rect_contains(&nr, g->snake.seg[i].x, g->snake.seg[i].y)) ok = 0;
    for (int i = 0; i < g->blockCount && ok; i++)
      if (!rect_contains(&nr, g->blocks[i].x, g->blocks[i].y)) ok = 0;
    if (!ok) continue;
    int newArea = (nr.x1 - nr.x0) * (nr.y1 - nr.y0);
    // 剩余空地 < 当前可操作面积 1/10 → 无法再缩 → 场地耗尽
    if (newArea - g->snake.len < newArea / 10) return 1;
    g->grid.operable = nr;
    return 0;
  }
  return 2;
}

// ---- 引擎 ----
static void spawn_to_cap(Game *g);

static void end_game(Game *g, int reason) {
  g->state = 1;
  g->reason = reason;
  g->overTimer = g->params.endFreezeMs;
}

static void free_cells(Game *g, Point *out, int *n) {
  int w = g->grid.w, h = g->grid.h;
  memset(g->visited, 0, (size_t)w * h);
  for (int i = 0; i < g->snake.len; i++) {
    Point p = g->snake.seg[i];
    if (p.x >= 0 && p.x < w && p.y >= 0 && p.y < h) g->visited[p.y * w + p.x] = 1;
  }
  for (int i = 0; i < g->blockCount; i++) {
    Block *b = &g->blocks[i];
    if (b->x >= 0 && b->x < w && b->y >= 0 && b->y < h) g->visited[b->y * w + b->x] = 1;
  }
  *n = 0;
  for (int y = g->grid.operable.y0; y < g->grid.operable.y1; y++)
    for (int x = g->grid.operable.x0; x < g->grid.operable.x1; x++)
      if (!g->visited[y * w + x]) { out[(*n)++] = (Point){x, y}; }
}

static void spawn_to_cap(Game *g) {
  int cap = g->fx.blockCap;
  int guard = 0;
  while (g->blockCount < cap && guard++ < 10000) {
    int n = 0;
    free_cells(g, g->tmpBody, &n); // 复用 tmpBody 作为空格列表
    if (n == 0) return;
    Point c = g->tmpBody[rng_int(g, n)];
    Block *b = &g->blocks[g->blockCount++];
    b->x = c.x; b->y = c.y;
    b->kind = pick_kind(g);
    b->remaining = g->fx.lifetime; // 新方块按当前全局生存时间生成
  }
}

static void reconcile_cap(Game *g) {
  while (g->blockCount > g->fx.blockCap) {
    int i = rng_int(g, g->blockCount);
    g->blocks[i] = g->blocks[g->blockCount - 1];
    g->blockCount--;
  }
}

static void move_step(Game *g) {
  Point dir = decide(g);
  Point nh = { g->snake.seg[0].x + dir.x, g->snake.seg[0].y + dir.y };
  if (is_wall(&g->grid, nh.x, nh.y)) { end_game(g, 2); return; }
  int eatenIdx = -1;
  for (int i = 0; i < g->blockCount; i++)
    if (g->blocks[i].x == nh.x && g->blocks[i].y == nh.y) { eatenIdx = i; break; }
  g->snake.dir = dir;
  if (snake_collides(&g->snake, eatenIdx >= 0)) { end_game(g, 3); return; }
  int newColor = g->snake.color[0];
  if (eatenIdx >= 0) newColor = g->blocks[eatenIdx].kind - 1;
  snake_step(&g->snake, dir, eatenIdx >= 0, newColor);
  if (eatenIdx >= 0) {
    int kind = g->blocks[eatenIdx].kind;
    g->blocks[eatenIdx] = g->blocks[g->blockCount - 1];
    g->blockCount--;
    effects_apply(&g->fx, kind, g);
    g->score += SCORES[newColor];
    g->colorCounts[newColor]++;
    reconcile_cap(g);
    spawn_to_cap(g);
  }
  // 成功：蛇身铺满整个可操作区域
  if (g->snake.len == area(&g->grid)) end_game(g, 1);
}

void game_update(Game *g, double dtMs) {
  if (g->state == 1) {
    g->overTimer -= dtMs;
    if (g->overTimer <= 0) game_reset(g);
    return;
  }
  g->survival += dtMs / 1000.0;

  // 方块存活计时与超时
  int expired = 0;
  for (int i = 0; i < g->blockCount; i++) {
    g->blocks[i].remaining -= dtMs / 1000.0;
    if (g->blocks[i].remaining <= 0) expired = 1;
  }
  if (expired) {
    int w = 0;
    for (int i = 0; i < g->blockCount; i++)
      if (g->blocks[i].remaining > 0) g->blocks[w++] = g->blocks[i];
    g->blockCount = w;
    g->pendingShrink = 1;
  }

  // 场地缩小（可能延迟到某条边可行）
  if (g->pendingShrink) {
    int res = try_shrink(g);
    if (res == 0) g->pendingShrink = 0;
    else if (res == 1) { end_game(g, 4); return; }
  }

  // 按当前速度走步
  g->accumMs += dtMs;
  double stepMs = 1000.0 / speed_of(g);
  while (g->accumMs >= stepMs && g->state == 0) {
    g->accumMs -= stepMs;
    move_step(g);
  }
}

void game_reset(Game *g) {
  g->grid.operable = (Rect){ 0, 0, g->grid.w, g->grid.h };
  g->fx = (Effects){ 0, 0, g->params.blockLifetime, g->params.initialBlockCap };
  g->blockCount = 0;
  g->score = 0;
  memset(g->colorCounts, 0, sizeof(g->colorCounts));
  g->survival = 0;
  g->state = 0;
  g->reason = 0;
  g->overTimer = 0;
  g->pendingShrink = 0;
  g->accumMs = 0;
  int cx = g->grid.w / 2, cy = g->grid.h / 2;
  Point dir = DIRS[rng_int(g, 4)];
  g->snake.len = 3;
  g->snake.dir = dir;
  for (int i = 0; i < 3; i++) {
    g->snake.seg[i] = (Point){ cx - dir.x * i, cy - dir.y * i };
    g->snake.color[i] = 0; // 初始颜色 = 颜色表[0]（红）
  }
  spawn_to_cap(g);
}

void game_init(Game *g, int w, int h, unsigned seed) {
  int n = w * h;
  g->grid.w = w;
  g->grid.h = h;
  g->grid.operable = (Rect){ 0, 0, w, h };
  g->snake.seg = (Point *)malloc(sizeof(Point) * (n + 2));
  g->snake.color = (int *)malloc(sizeof(int) * (n + 2));
  g->snake.cap = n + 2;
  g->snake.len = 0;
  g->dist = (int *)malloc(sizeof(int) * n);
  g->prev = (int *)malloc(sizeof(int) * n);
  g->queue = (int *)malloc(sizeof(int) * n);
  g->visited = (unsigned char *)malloc(n);
  g->fstack = (int *)malloc(sizeof(int) * n);
  g->tmpBody = (Point *)malloc(sizeof(Point) * (n + 2));
  g->path = (int *)malloc(sizeof(int) * n);
  g->rngState = seed ? seed : 0x9E3779B9u;
  g->params = params_default();
  game_reset(g);
}

void game_free(Game *g) {
  free(g->snake.seg); free(g->snake.color);
  free(g->dist); free(g->prev); free(g->queue);
  free(g->visited); free(g->fstack); free(g->tmpBody); free(g->path);
}

int game_is_over(Game *g) { return g->state == 1; }
Point game_head(Game *g) { return g->snake.seg[0]; }
double game_speed(const Game *g) { return speed_of(g); }
