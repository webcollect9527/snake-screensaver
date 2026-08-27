// 游戏主循环与状态机：tick 调度、方块生存/超时、缩场、吃方块、结束与自动重开。
// 纯逻辑层，不依赖 Electron/Canvas，保证可单测。
import { Grid, key, Point } from './grid';
import { Snake } from './snake';
import { Block, pickKind } from './block';
import { Effects } from './effects';
import { decide } from './pathfinding';
import { tryShrink } from './arena';
import { config } from '../config';

export type EndReason = 'success' | 'wall' | 'self' | 'area';

export interface GameStats {
  score: number;
  colorCounts: number[];
  survivalSec: number;
  reason: EndReason;
}

export interface GameOptions {
  rng?: () => number;
  onEnd?: (stats: GameStats) => void;
}

const SPAWN_DIRS: Point[] = [
  { x: 1, y: 0 },
  { x: -1, y: 0 },
  { x: 0, y: 1 },
  { x: 0, y: -1 },
];

export class Game {
  grid: Grid;
  snake!: Snake; // reset() 中初始化
  effects = new Effects();
  blocks: Block[] = [];
  score = 0;
  colorCounts: number[] = [0, 0, 0, 0, 0, 0, 0];
  state: 'running' | 'over' = 'running';
  reason: EndReason | null = null;
  survivalSec = 0;

  private overTimer = 0;
  private pendingShrink = false;
  private accumMs = 0;
  private rng: () => number;
  private onEnd: (stats: GameStats) => void;

  constructor(w: number, h: number, opts: GameOptions = {}) {
    this.rng = opts.rng ?? Math.random;
    this.onEnd = opts.onEnd ?? (() => {});
    this.grid = new Grid(w, h);
    this.reset();
  }

  reset(): void {
    this.grid.operable = { x0: 0, y0: 0, x1: this.grid.w, y1: this.grid.h };
    this.effects = new Effects();
    this.blocks = [];
    this.score = 0;
    this.colorCounts = [0, 0, 0, 0, 0, 0, 0];
    this.survivalSec = 0;
    this.state = 'running';
    this.reason = null;
    this.overTimer = 0;
    this.pendingShrink = false;
    this.accumMs = 0;
    const r = this.grid.operable;
    const cx = Math.floor((r.x0 + r.x1) / 2);
    const cy = Math.floor((r.y0 + r.y1) / 2);
    const dir = SPAWN_DIRS[Math.floor(this.rng() * SPAWN_DIRS.length)];
    this.snake = new Snake({ x: cx, y: cy }, dir, 3, config.colors[0]);
    this.spawnToCap();
  }

  update(dtMs: number): void {
    if (this.state === 'over') {
      this.overTimer -= dtMs;
      if (this.overTimer <= 0) this.reset();
      return;
    }
    this.survivalSec += dtMs / 1000;

    // 方块存活计时与超时
    let expired = false;
    for (const b of this.blocks) {
      b.remaining -= dtMs / 1000;
      if (b.remaining <= 0) expired = true;
    }
    if (expired) {
      this.blocks = this.blocks.filter((b) => b.remaining > 0);
      this.pendingShrink = true;
    }

    // 场地缩小（超时触发，可能延迟到某条边可行）
    if (this.pendingShrink) {
      const res = tryShrink(this.grid, this.snake, this.blocks, this.rng);
      if (res.shrunk) this.pendingShrink = false;
      else if (res.gameOver) {
        this.end('area');
        return;
      }
    }

    // 按当前速度走步（逻辑固定步长）
    this.accumMs += dtMs;
    const stepMs = 1000 / this.effects.speed();
    while (this.accumMs >= stepMs && this.state === 'running') {
      this.accumMs -= stepMs;
      this.moveStep();
    }
  }

  private end(reason: EndReason): void {
    this.state = 'over';
    this.reason = reason;
    this.overTimer = config.endFreezeMs;
    this.onEnd({
      score: this.score,
      colorCounts: this.colorCounts.slice(),
      survivalSec: this.survivalSec,
      reason,
    });
  }

  private moveStep(): void {
    const dir = decide(this.grid, this.snake, this.blocks, this.effects.speed(), this.rng);
    const nh = this.snake.nextHead(dir);
    if (this.grid.isWall(nh.x, nh.y)) {
      this.end('wall');
      return;
    }
    const eaten = this.blocks.find((b) => b.x === nh.x && b.y === nh.y) ?? null;
    if (this.snake.collidesWithBody(!!eaten)) {
      this.end('self');
      return;
    }
    this.snake.step(dir, !!eaten, eaten ? config.colors[eaten.kind - 1] : undefined); // 新增单元 = 被吃方块颜色
    if (eaten) {
      this.blocks = this.blocks.filter((b) => b !== eaten);
      this.effects.apply(eaten.kind, this.rng);
      this.score += config.scores[eaten.kind - 1];
      this.colorCounts[eaten.kind - 1]++;
      this.reconcileCap();
      this.spawnToCap();
    }
    // 成功：蛇身铺满整个可操作区域
    if (this.snake.segments.length === this.grid.area()) this.end('success');
  }

  private reconcileCap(): void {
    while (this.blocks.length > this.effects.blockCap) {
      const i = Math.floor(this.rng() * this.blocks.length);
      this.blocks.splice(i, 1);
    }
  }

  private spawnToCap(): void {
    const cap = this.effects.blockCap;
    let guard = 0;
    while (this.blocks.length < cap && guard++ < 10000) {
      const free = this.freeCells();
      if (free.length === 0) return;
      const cell = free[Math.floor(this.rng() * free.length)];
      this.blocks.push({
        x: cell.x,
        y: cell.y,
        kind: pickKind(this.rng),
        remaining: this.effects.lifetime, // 新方块按当前全局生存时间生成
      });
    }
  }

  private freeCells(): Point[] {
    const cells: Point[] = [];
    const r = this.grid.operable;
    const taken = new Set<string>();
    for (const s of this.snake.positions()) taken.add(key(s));
    for (const b of this.blocks) taken.add(key(b));
    for (let y = r.y0; y < r.y1; y++) {
      for (let x = r.x0; x < r.x1; x++) {
        const k = x + ',' + y;
        if (!taken.has(k)) cells.push({ x, y });
      }
    }
    return cells;
  }
}
