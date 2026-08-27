// 寻路 AI：多目标 BFS + 时间成本评分 + flood fill 安全/逃生检查（4.3 节）。
// 热路径用 Int32Array 做 visited/prev（格索引直接寻址），避免字符串键哈希开销。
import { Grid, Point } from './grid';
import { Snake } from './snake';
import type { Block } from './block';
import { config } from '../config';

const DIRS: Point[] = [
  { x: 1, y: 0 },
  { x: -1, y: 0 },
  { x: 0, y: 1 },
  { x: 0, y: -1 },
];

const idx = (x: number, y: number, w: number) => y * w + x;

export interface DistResult {
  dist: Int32Array; // -1 未达，-2 身体，>=0 到头的距离
  prev: Int32Array; // 前驱格索引（头为 -1）
  w: number;
  h: number;
}

// 等权网格 BFS，返回每个可达格子的距离与前驱。身体（除头）视为障碍。
export function bfsDistances(grid: Grid, snake: Snake): DistResult {
  const { w, h } = grid;
  const dist = new Int32Array(w * h).fill(-1);
  const prev = new Int32Array(w * h).fill(-1);
  const head = snake.head;
  for (const s of snake.positions()) {
    if (s.x === head.x && s.y === head.y) continue;
    if (s.x >= 0 && s.x < w && s.y >= 0 && s.y < h) dist[idx(s.x, s.y, w)] = -2;
  }
  const queue = new Int32Array(w * h);
  let qh = 0;
  let qt = 0;
  const si = idx(head.x, head.y, w);
  dist[si] = 0;
  queue[qt++] = si;
  while (qh < qt) {
    const ci = queue[qh++];
    const cx = ci % w;
    const cy = (ci / w) | 0;
    const nd = dist[ci] + 1;
    for (const d of DIRS) {
      const nx = cx + d.x;
      const ny = cy + d.y;
      if (!grid.contains(nx, ny) || grid.isWall(nx, ny)) continue;
      const ni = idx(nx, ny, w);
      if (dist[ni] !== -1) continue; // 已访问或身体
      dist[ni] = nd;
      prev[ni] = ci;
      queue[qt++] = ni;
    }
  }
  return { dist, prev, w, h };
}

// 从头到目标的格索引路径（含头）。
function pathToTarget(d: DistResult, targetIdx: number): number[] {
  const path: number[] = [];
  let cur = targetIdx;
  while (cur !== -1) {
    path.push(cur);
    cur = d.prev[cur];
  }
  path.reverse();
  return path;
}

// 从头到目标的下一步方向。
function firstStep(d: DistResult, headIdx: number, targetIdx: number): Point | null {
  let cur = targetIdx;
  while (d.prev[cur] !== headIdx) {
    if (d.prev[cur] === -1) return null;
    cur = d.prev[cur];
  }
  return {
    x: (cur % d.w) - (headIdx % d.w),
    y: ((cur / d.w) | 0) - ((headIdx / d.w) | 0),
  };
}

// 从 start 出发 flood fill 可达空格数；blocked 为格索引标记（1=占用）。
export function floodFillArea(grid: Grid, blocked: Uint8Array, start: Point): number {
  const { w, h } = grid;
  const si = idx(start.x, start.y, w);
  if (grid.isWall(start.x, start.y) || blocked[si]) return 0;
  const visited = new Uint8Array(w * h);
  visited[si] = 1;
  const stack: number[] = [si];
  let count = 0;
  while (stack.length) {
    const ci = stack.pop()!;
    count++;
    const cx = ci % w;
    const cy = (ci / w) | 0;
    for (const d of DIRS) {
      const nx = cx + d.x;
      const ny = cy + d.y;
      if (!grid.contains(nx, ny) || grid.isWall(nx, ny)) continue;
      const ni = idx(nx, ny, w);
      if (visited[ni] || blocked[ni]) continue;
      visited[ni] = 1;
      stack.push(ni);
    }
  }
  return count;
}

// 身体（除头）格索引标记。
function bodyMark(grid: Grid, body: Point[]): Uint8Array {
  const mark = new Uint8Array(grid.w * grid.h);
  for (let i = 1; i < body.length; i++) {
    const p = body[i];
    if (p.x >= 0 && p.x < grid.w && p.y >= 0 && p.y < grid.h) mark[idx(p.x, p.y, grid.w)] = 1;
  }
  return mark;
}

// 模拟蛇沿路径走到目标（途中不增长），检查到达后是否还有逃生空间。
function isTargetSafe(grid: Grid, snake: Snake, path: number[], d: DistResult): boolean {
  let sim = snake.positions();
  for (let i = 1; i < path.length; i++) {
    sim = [{ x: path[i] % d.w, y: (path[i] / d.w) | 0 }, ...sim];
    sim.pop();
  }
  const head = sim[0];
  const area = floodFillArea(grid, bodyMark(grid, sim), head);
  return area >= sim.length; // 可达空格数 < 蛇长 → 无逃生空间
}

// 逃生模式：贪心选择使 flood fill 面积最大的相邻格；平局优先当前方向。
function escapeDir(grid: Grid, snake: Snake): Point {
  const head = snake.head;
  const positions = snake.positions();
  const bodySet = new Set(positions.slice(1).map((p) => p.x + ',' + p.y));
  let best: Point | null = null;
  let bestArea = -1;
  for (const d of DIRS) {
    const nx = head.x + d.x;
    const ny = head.y + d.y;
    if (grid.isWall(nx, ny) || bodySet.has(nx + ',' + ny)) continue;
    const sim = [{ x: nx, y: ny }, ...positions];
    sim.pop();
    const area = floodFillArea(grid, bodyMark(grid, sim), { x: nx, y: ny });
    const prefer = d.x === snake.dir.x && d.y === snake.dir.y ? 0.5 : 0;
    if (area + prefer > bestArea) {
      bestArea = area + prefer;
      best = d;
    }
  }
  return best ?? snake.dir;
}

// 每步决策：返回下一步方向。
export function decide(
  grid: Grid,
  snake: Snake,
  blocks: Block[],
  speed: number,
  _rng: () => number = Math.random,
): Point {
  const d = bfsDistances(grid, snake);
  const headIdx = idx(snake.head.x, snake.head.y, grid.w);
  const reachable: { block: Block; idx: number; dcell: number }[] = [];
  for (const b of blocks) {
    if (!grid.contains(b.x, b.y)) continue;
    const bi = idx(b.x, b.y, grid.w);
    if (d.dist[bi] > 0) reachable.push({ block: b, idx: bi, dcell: d.dist[bi] });
  }
  if (reachable.length === 0) return escapeDir(grid, snake);

  // 目标评分：score = 分数 / (d+1) × urgency（临期方块优先抢救）
  const scored = reachable
    .map((c) => {
      const t = c.dcell / speed;
      const slack = c.block.remaining - t;
      const urgency = slack < config.urgencyThresholdSec ? config.urgencyFactor : 1;
      const score = (config.scores[c.block.kind - 1] / (c.dcell + 1)) * urgency;
      return { ...c, score };
    })
    .sort((a, b) => b.score - a.score);

  // 按评分从高到低做安全性检查，首个安全者即目标
  for (const c of scored) {
    const path = pathToTarget(d, c.idx);
    if (isTargetSafe(grid, snake, path, d)) {
      const step = firstStep(d, headIdx, c.idx);
      if (step) return step;
    }
  }
  return escapeDir(grid, snake);
}
