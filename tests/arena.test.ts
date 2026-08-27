import { describe, it, expect } from 'vitest';
import { Grid } from '../src/game/grid';
import { Snake } from '../src/game/snake';
import { tryShrink } from '../src/game/arena';
import type { Point } from '../src/game/grid';

describe('tryShrink（场地缩小）', () => {
  it('空旷场地缩小约 1/10，蛇身不被覆盖', () => {
    const grid = new Grid(20, 20);
    const snake = new Snake({ x: 10, y: 10 }, { x: 1, y: 0 }, 3, '#fff');
    const res = tryShrink(grid, snake, [], () => 0.5);
    expect(res.shrunk).toBe(true);
    expect(grid.area()).toBeLessThan(400);
    expect(snake.positions().every((p) => !grid.isWall(p.x, p.y))).toBe(true);
  });

  it('推边会覆盖蛇身时改选其它边，最终蛇身始终在区域内', () => {
    const grid = new Grid(20, 20);
    // 蛇贴着左边缘
    const snake = new Snake({ x: 0, y: 10 }, { x: 0, y: 1 }, 3, '#fff');
    const res = tryShrink(grid, snake, [], () => 0.5);
    expect(res.shrunk).toBe(true);
    expect(snake.positions().every((p) => !grid.isWall(p.x, p.y))).toBe(true);
  });

  it('四条边都被蛇身阻挡时延迟（不缩小、不判负）', () => {
    const grid = new Grid(8, 8);
    const snake = new Snake({ x: 0, y: 0 }, { x: 1, y: 0 }, 1, '#fff');
    // 蛇身铺满整个周长
    const ring: Point[] = [];
    for (let x = 0; x < 8; x++) ring.push({ x, y: 0 });
    for (let y = 1; y < 8; y++) ring.push({ x: 7, y });
    for (let x = 6; x >= 0; x--) ring.push({ x, y: 7 });
    for (let y = 6; y >= 1; y--) ring.push({ x: 0, y });
    snake.segments = ring.map((p) => ({ x: p.x, y: p.y, color: '#fff' }));
    const res = tryShrink(grid, snake, [], () => 0.5);
    expect(res.shrunk).toBe(false);
    expect(res.gameOver).toBe(false);
    expect(grid.area()).toBe(64);
  });

  it('缩小后剩余空地不足 1/10 时判场地耗尽', () => {
    const grid = new Grid(5, 5);
    const snake = new Snake({ x: 0, y: 0 }, { x: 1, y: 0 }, 1, '#fff');
    // 蛇占满 y=0..3 全部 20 格（去掉一角），只剩 y=4 一行空
    const cells: Point[] = [];
    for (let y = 0; y < 4; y++) {
      for (let x = 0; x < 5; x++) {
        if (!(x === 4 && y === 3)) cells.push({ x, y });
      }
    }
    snake.segments = cells.map((p) => ({ x: p.x, y: p.y, color: '#fff' }));
    const res = tryShrink(grid, snake, [], () => 0.5);
    expect(res.shrunk).toBe(false);
    expect(res.gameOver).toBe(true);
  });
});
