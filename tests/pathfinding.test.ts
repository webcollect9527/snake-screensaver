import { describe, it, expect } from 'vitest';
import { Grid } from '../src/game/grid';
import { Snake } from '../src/game/snake';
import { decide } from '../src/game/pathfinding';
import type { Block } from '../src/game/block';

function blk(x: number, y: number, kind = 1, remaining = 60): Block {
  return { x, y, kind, remaining };
}

describe('decide（寻路 AI）', () => {
  it('朝向正前方的可达方块直线移动', () => {
    const grid = new Grid(20, 20);
    const snake = new Snake({ x: 10, y: 10 }, { x: 1, y: 0 }, 3, '#fff');
    const dir = decide(grid, snake, [blk(12, 10)], 10, () => 0.5);
    expect(dir).toEqual({ x: 1, y: 0 });
  });

  it('绕开自身身体，不会让蛇头下一步撞上身体', () => {
    const grid = new Grid(20, 20);
    // 头(10,10)向右，身体向左延伸：(9,10),(8,10)；方块在身体后方，不可达
    const snake = new Snake({ x: 10, y: 10 }, { x: 1, y: 0 }, 3, '#fff');
    const dir = decide(grid, snake, [blk(6, 10)], 10, () => 0.5);
    const nh = { x: 10 + dir.x, y: 10 + dir.y };
    const body = snake.positions();
    expect(body.some((s) => s.x === nh.x && s.y === nh.y)).toBe(false);
    // 且不能撞墙
    expect(grid.isWall(nh.x, nh.y)).toBe(false);
  });

  it('无可达方块时走逃生方向（不撞身体不撞墙）', () => {
    const grid = new Grid(5, 5);
    const snake = new Snake({ x: 2, y: 2 }, { x: 0, y: 1 }, 3, '#fff');
    const dir = decide(grid, snake, [], 10, () => 0.5);
    const valid = [
      [1, 0], [-1, 0], [0, 1], [0, -1],
    ].some(([x, y]) => x === dir.x && y === dir.y);
    expect(valid).toBe(true);
  });

  it('网格边界外不可通行（围墙）', () => {
    const grid = new Grid(3, 3);
    expect(grid.isWall(3, 1)).toBe(true);
    expect(grid.isWall(-1, 0)).toBe(true);
    expect(grid.isWall(1, 1)).toBe(false);
  });
});
