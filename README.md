<div align="center">

# 🐍 Snake Screensaver

AI 自动操控的贪吃蛇 Windows 屏幕保护程序（`.scr`）· 原生 Win32/C 实现

![C](https://img.shields.io/badge/C-C99-555555?logo=c&logoColor=white)
![Win32](https://img.shields.io/badge/平台-Win32%20GDI-0078D6)
![大小](https://img.shields.io/badge/产物-~30%20KB-2ea44f)
![构建](https://img.shields.io/badge/构建-TinyCC-9c3)

</div>

蛇在全屏网格中自行寻路、吃方块，受 7 种方块效果（加速/减速、生存时间增减、同屏方块数增减）影响；方块超时会使场地缩小，蛇死亡或场地耗尽后自动重开，右上角 HUD 持续显示统计。全程无人交互，由内置 AI 决策。

## ✨ 特性

- **AI 寻路**：多目标 BFS 最短路径 + 按「分数/距离 × 临期权重」评分选目标；flood fill 安全检查防自杀，走投无路时贪心逃生（`Int32Array`/原生数组热路径，全屏 192×108 网格下每次决策 <2ms）
- **7 种方块**：红/绿/蓝/黄/紫/青/白，各带分数与效果，按 `10:9:4:4:3:3:1` 占比生成
- **场地缩小**：方块超时 → 随机选边向内推进面积 1/10，避开蛇身与方块，被阻挡延迟执行
- **四种结束**：铺满成功 / 撞墙 / 撞自己 / 场地耗尽；结束定格 3 秒自动重开
- **HUD**：50% 半透明面板，7 色计数、得分、**当前速度**、本局/最高生存时间、成功/失败次数
- **数字时钟**：左上角（距上/左各 2 格）5×7 点阵电子表样式，HH:MM:SS 含秒，冒号两侧与每对数字间留空格，颜色实时跟随蛇头，大小可在 `/c` 设置（1~10 档）
- **呼吸灯**：所有常亮元素（方块/围墙/网格/HUD）按 10 秒周期缓慢呼吸（亮→暗 5s、暗→亮 5s），常亮元素滞后方块 2s；临期（<10s）方块快速闪烁预警；蛇自身与数字时钟呼吸豁免
- **无黑屏闪**：整帧离屏双缓冲（一次 BitBlt 上屏）+ 禁止背景擦黑 + 定时器对齐显示器刷新率，切换/重开无黑屏闪
- **可配置设置**：`/c` 设置窗口可调速度/方块/缩场/方块占比等全部参数，持久化到 `config.json`
- **跨会话持久化**：`%APPDATA%\snake-screensaver\stats.json`（统计）与 `config.json`（设置）
- **原生屏保协议**：`/s` 全屏置顶隐藏鼠标 + 位移>10px/按键/点击退出；`/c` 设置窗口；`/p` 预览
- **极小**：单文件 `SnakeScreensaver.scr` 约 **30 KB**，无需任何运行时依赖

> 为什么弃用 Electron？Electron 便携版与 Windows 屏保协议不兼容（NSIS 桩会把 `/c`、`/s` 当安装器开关，报 "error launching installer"），且无法做到真全屏、体积最小也有 ~80MB。原生 Win32/C 是 Windows 屏保的标准形态：一个文件、原生协议、几十 KB。

## 🧱 项目结构

```
src/win/                 # 原生实现（产品）
├── screensaver.c        # Win32 壳：/s /c /p、退出监听、GDI 渲染、HUD、stats.json
├── game.c / game.h      # 纯游戏逻辑（BFS 寻路、效果、缩场、状态机，可无 GUI 自测）
└── game_test.c          # 逻辑自检：AI 整局模拟 + 行为断言（控制台）
src/game/ + tests/       # TypeScript 逻辑参考实现 + vitest 测试（算法可执行规格）
scripts/
└── build-win.cjs        # 用 TinyCC 编译自检 + 生成 .scr
```

## 🚀 快速开始

环境要求：Windows 10/11，[TinyCC](https://download.savannah.gnu.org/releases/tinycc/)（`tcc.exe` 放到 `~/tcc/` 或设置 `TCC` 环境变量）。

```bash
npm run build:win   # 先跑 C 逻辑自检，再生成 release/SnakeScreensaver.scr
```

产物：`release/SnakeScreensaver.scr`（约 30 KB）。

**开发自测**（普通窗口，不退出监听）：双击 `.scr` 或右键 →「测试」即窗口化运行；也可显式指定：

```bash
SnakeScreensaver.scr --windowed
```

## 🔌 安装为 Windows 屏保

1. 将 `release/SnakeScreensaver.scr` 复制到 `C:\Windows\System32\`（管理员权限）
2. 桌面右键 →「个性化」→「锁屏界面」→「屏幕保护程序设置」，下拉选择 **Snake Screensaver**
3. 或右键 `.scr` →「测试」

> 首次运行若被安全软件拦截，请在杀软中把该文件加入信任/白名单。

## 🧪 测试

```bash
npm test          # TypeScript 逻辑参考实现的 vitest 单元测试 + AI 整局模拟
npm run build:win # 含 C 版逻辑自检（game_test.exe），验证原生实现与参考实现行为一致
```

## ⚙️ 可调参数

默认值集中在 `src/win/game.c` 的 `params_default()`（与 `src/config.ts` 一致）。**运行时可配置**：右键 `.scr` →「设置」，或运行 `/c` 打开设置窗口，保存到 `%APPDATA%\snake-screensaver\config.json`。

| 参数 | 默认 | 说明 |
|---|---|---|
| `baseSpeed` | 20 | 基础速度（格/秒，初始默认翻倍） |
| `initialBlockCap` | 3 | 初始同屏方块数上限 |
| `blockLifetime` | 60 | 方块初始生存时间（秒） |
| `shrinkFraction` | 0.1 | 场地每次缩小比例 |
| `urgencyThreshold` | 5 | 临期方块判定（松弛 <N 秒） |
| `urgencyFactor` | 3 | 临期权重提升 |
| `endFreezeMs` | 3000 | 结束定格时长（毫秒） |
| `cellSizePx` | 10 | 单元格边长（px） |
| `clockScale` | 1 | 数字时钟缩放（1~10，档 10 = 10 倍） |
| `weights[1..7]` | 10:9:4:4:3:3:1 | 7 种方块生成占比 |

## ❓ FAQ

| 问题 | 说明 |
|---|---|
| 动一下鼠标屏保就退出？ | 屏保正常行为：位移 >10px、按键、点击均退出 |
| 配置窗口打不开？ | `/c` 打开配置窗口；若直接双击 .scr 是运行不是配置 |
| 统计/设置存在哪？ | `%APPDATA%\snake-screensaver\stats.json`（统计）、`config.json`（设置） |
| 场地越来越小？ | 方块超时未吃会触发缩场（1/10），剩余空地不足判定「场地耗尽」 |

## 📄 License

未指定
