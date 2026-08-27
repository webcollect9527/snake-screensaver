// 方块：按占比抽样生成、存活计时（只影响后续生成，不回溯在场方块）。
import { config } from '../config';

export interface Block {
  x: number;
  y: number;
  kind: number; // 1..7
  remaining: number; // 剩余存活秒数
}

// 按配置占比（10:9:4:4:3:3:1）抽样方块类型，返回 1..7。
export function pickKind(rng: () => number = Math.random): number {
  const total = config.weights.reduce((a, b) => a + b, 0);
  let r = rng() * total;
  for (let i = 0; i < config.weights.length; i++) {
    r -= config.weights[i];
    if (r < 0) return i + 1;
  }
  return config.weights.length;
}
