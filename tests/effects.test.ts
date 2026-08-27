import { describe, it, expect } from 'vitest';
import { Effects } from '../src/game/effects';
import { config } from '../src/config';

describe('Effects（7 种效果结算）', () => {
  it('1 号加速、2 号减速：速度倍率按对数计数变化', () => {
    const e = new Effects();
    const base = e.speed();
    e.apply(1, () => 0);
    expect(e.speed()).toBeGreaterThan(base);
    const faster = e.speed();
    e.apply(2, () => 0);
    expect(e.speed()).toBeLessThan(faster);
  });

  it('3 号生存时间 +1 秒，4 号 -1 秒且有 1 秒下限', () => {
    const e = new Effects();
    expect(e.lifetime).toBe(config.blockLifetimeSec);
    e.apply(3, () => 0);
    expect(e.lifetime).toBe(config.blockLifetimeSec + 1);
    for (let i = 0; i < 100; i++) e.apply(4, () => 0);
    expect(e.lifetime).toBe(config.lifetimeMinSec);
  });

  it('5 号同屏上限 +1，6 号 -1 且有 1 下限', () => {
    const e = new Effects();
    expect(e.blockCap).toBe(config.initialBlockCap);
    e.apply(5, () => 0);
    expect(e.blockCap).toBe(config.initialBlockCap + 1);
    for (let i = 0; i < 100; i++) e.apply(6, () => 0);
    expect(e.blockCap).toBe(config.blockCapMin);
  });

  it('7 号随机触发 1~6 之一（固定 rng=0 时为 1 号）', () => {
    const e = new Effects();
    e.apply(7, () => 0);
    expect(e.speedUpCount).toBe(1);
  });
});
