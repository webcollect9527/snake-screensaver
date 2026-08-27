import { describe, it, expect } from 'vitest';
import * as fs from 'fs';
import { Game } from '../src/game/engine';

// mulberry32：确定性随机，保证结果可复现
function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

describe('AI 整局模拟', () => {
  it('多局对局平均生存时间合理，且会吃方块', () => {
    const totalSurvival: number[] = [];
    const totalAte: number[] = [];
    const GAMES = 10;
    for (let g = 0; g < GAMES; g++) {
      const game = new Game(48, 27, { rng: mulberry32(1000 + g) });
      for (let t = 0; t < 60 * 1000; t += 16) {
        game.update(16);
        if (game.state === 'over') break;
      }
      totalSurvival.push(game.survivalSec);
      totalAte.push(game.colorCounts.reduce((a, b) => a + b, 0));
    }
    const avgSurvival = totalSurvival.reduce((a, b) => a + b, 0) / GAMES;
    const sumAte = totalAte.reduce((a, b) => a + b, 0);
    fs.writeFileSync('sim-out.txt', `平均生存 ${avgSurvival.toFixed(1)}s，共吃 ${sumAte} 个方块\n`);
    expect(sumAte).toBeGreaterThan(0);
    expect(avgSurvival).toBeGreaterThan(10);
  });
});
