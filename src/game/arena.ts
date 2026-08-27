// 场地缩小（4.4 节）：随机选边向内推进 1/10，避开蛇身与方块，可行性不足则延迟。
import { Grid, Point, Rect } from './grid';
import { Snake } from './snake';
import type { Block } from './block';
import { config } from '../config';

export interface ShrinkResult {
  shrunk: boolean;
  gameOver: boolean;
}

function contains(r: Rect, p: Point): boolean {
  return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1;
}

// 返回是否缩小成功；gameOver=true 表示剩余空地不足 1/10，判场地耗尽。
export function tryShrink(
  grid: Grid,
  snake: Snake,
  blocks: Block[],
  rng: () => number = Math.random,
): ShrinkResult {
  const r = grid.operable;
  const area = (r.x1 - r.x0) * (r.y1 - r.y0);
  const w = r.x1 - r.x0;
  const h = r.y1 - r.y0;

  const edges = [
    { idx: 0, depth: Math.ceil((area * config.shrinkFraction) / h) }, // 左
    { idx: 1, depth: Math.ceil((area * config.shrinkFraction) / h) }, // 右
    { idx: 2, depth: Math.ceil((area * config.shrinkFraction) / w) }, // 上
    { idx: 3, depth: Math.ceil((area * config.shrinkFraction) / w) }, // 下
  ];
  const order = [...edges].sort(() => rng() - 0.5);

  for (const e of order) {
    let nx0 = r.x0;
    let ny0 = r.y0;
    let nx1 = r.x1;
    let ny1 = r.y1;
    switch (e.idx) {
      case 0: nx0 = r.x0 + e.depth; break;
      case 1: nx1 = r.x1 - e.depth; break;
      case 2: ny0 = r.y0 + e.depth; break;
      case 3: ny1 = r.y1 - e.depth; break;
    }
    if (nx1 <= nx0 || ny1 <= ny0) continue; // 退化，无法再缩
    const nr: Rect = { x0: nx0, y0: ny0, x1: nx1, y1: ny1 };
    // 新边界若覆盖蛇身或任何存活方块 → 改选其它边
    if (!snake.positions().every((p) => contains(nr, p))) continue;
    if (!blocks.every((b) => contains(nr, b))) continue;
    const newArea = (nr.x1 - nr.x0) * (nr.y1 - nr.y0);
    const snakeLen = snake.segments.length;
    // 连锁判断：剩余空地 < 当前可操作面积 1/10 → 无法再缩 → 场地耗尽
    if (newArea - snakeLen < newArea / 10) return { shrunk: false, gameOver: true };
    grid.operable = nr;
    return { shrunk: true, gameOver: false };
  }
  // 四条边均被阻挡：延迟到某条边可行时再执行（不豁免）
  return { shrunk: false, gameOver: false };
}
