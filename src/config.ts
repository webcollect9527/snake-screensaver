// 所有可调参数集中管理，禁止魔法数字散落。
export const config = {
  cellSizePx: 10,            // 单元格边长（px，渲染层再乘 devicePixelRatio）
  baseSpeedCellsPerSec: 10,  // 基础速度：10 格/秒
  initialBlockCap: 3,        // 初始同屏方块数上限（待确认项①，按 4.2 节取值）
  blockLifetimeSec: 60,      // 方块初始生存时间（秒）
  blockCapMin: 1,            // 同屏方块数上限下限
  lifetimeMinSec: 1,         // 生存时间下限（防减到 0 的除零/瞬时超时）
  shrinkFraction: 0.1,       // 场地每次缩小比例（面积的 1/10）
  urgencyThresholdSec: 5,    // 临期方块判定（剩余松弛 < 5s 提升权重）
  urgencyFactor: 3,          // 临期权重提升倍数
  speedUpRate: 0.01,         // 1 号效果：速度 +1%
  speedDownRate: 0.01,       // 2 号效果：速度 -1%
  endFreezeMs: 3000,         // 结束画面定格时长
  blinkPeriodSec: 5,         // 全屏呼吸灯周期（秒，C 渲染层，/c 可配置）
  // 颜色、分数、占比（对照 4.2 节）
  colors: ['#E5484D', '#46A758', '#3E63DD', '#FFB224', '#8E4EC6', '#00A2C7', '#FFFFFF'],
  scores: [1, 2, 1, 2, 5, 1, 5],
  weights: [10, 9, 4, 4, 3, 3, 1], // 占比 10:9:4:4:3:3:1（总 34 份）
  wallColor: '#2B2B2B',      // 围墙色（深灰）
} as const;
