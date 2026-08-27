// 7 种效果结算。速度用对数计数避免浮点连乘误差。
import { config } from '../config';

export type EffectKind = 1 | 2 | 3 | 4 | 5 | 6;

export class Effects {
  speedUpCount = 0; // 1 号：速度 +1%
  speedDownCount = 0; // 2 号：速度 -1%
  lifetime: number = config.blockLifetimeSec; // 3/4 号：全局方块生存时间（秒）
  blockCap: number = config.initialBlockCap; // 5/6 号：同屏方块数上限

  speed(): number {
    const lnBase = Math.log(config.baseSpeedCellsPerSec);
    const lnSpeed =
      lnBase +
      this.speedUpCount * Math.log(1 + config.speedUpRate) +
      this.speedDownCount * Math.log(1 - config.speedDownRate);
    return Math.exp(lnSpeed);
  }

  // kind: 1..7；7 号随机触发 1~6 之一。
  apply(kind: number, rng: () => number): void {
    const eff: EffectKind = kind === 7 ? ((1 + Math.floor(rng() * 6)) as EffectKind) : (kind as EffectKind);
    switch (eff) {
      case 1:
        this.speedUpCount++;
        break;
      case 2:
        this.speedDownCount++;
        break;
      case 3:
        this.lifetime += 1;
        break;
      case 4:
        this.lifetime = Math.max(config.lifetimeMinSec, this.lifetime - 1);
        break;
      case 5:
        this.blockCap += 1;
        break;
      case 6:
        this.blockCap = Math.max(config.blockCapMin, this.blockCap - 1);
        break;
    }
  }
}
