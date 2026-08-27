import { describe, it, expect } from 'vitest';
import { Game } from '../src/game/engine';
import { Snake } from '../src/game/snake';

describe('Game（引擎状态机）', () => {
  it('吃到正前方方块：得分、计数、长度+1', () => {
    const game = new Game(20, 20, { rng: () => 0.5, onEnd: () => {} });
    game.snake = new Snake({ x: 10, y: 10 }, { x: 1, y: 0 }, 3, '#fff');
    game.blocks = [{ x: 11, y: 10, kind: 1, remaining: 60 }];
    game.effects.blockCap = 1;
    game.update(100); // 基础速度 10 格/秒 → 一步
    expect(game.score).toBe(1);
    expect(game.colorCounts[0]).toBe(1);
    expect(game.snake.segments.length).toBe(4);
    expect(game.state).toBe('running');
  });

  it('完全被困且唯一出口是墙时，撞墙死亡', () => {
    const game = new Game(3, 3, { rng: () => 0.5 });
    game.snake = new Snake({ x: 2, y: 1 }, { x: 1, y: 0 }, 1, '#fff');
    game.snake.segments = [
      { x: 2, y: 1, color: '#fff' }, { x: 1, y: 1, color: '#fff' },
      { x: 0, y: 1, color: '#fff' }, { x: 2, y: 2, color: '#fff' },
      { x: 2, y: 0, color: '#fff' },
    ];
    game.blocks = [];
    game.update(100);
    expect(game.state).toBe('over');
    expect(game.reason).toBe('wall');
  });

  it('结束回调携带本局统计', () => {
    const holder: { reason?: string } = {};
    const game = new Game(3, 3, { rng: () => 0.5, onEnd: (s) => { holder.reason = s.reason; } });
    game.snake = new Snake({ x: 2, y: 1 }, { x: 1, y: 0 }, 1, '#fff');
    game.snake.segments = [
      { x: 2, y: 1, color: '#fff' }, { x: 1, y: 1, color: '#fff' },
      { x: 0, y: 1, color: '#fff' }, { x: 2, y: 2, color: '#fff' },
      { x: 2, y: 0, color: '#fff' },
    ];
    game.blocks = [];
    game.update(100);
    expect(holder.reason).toBe('wall');
  });
});
