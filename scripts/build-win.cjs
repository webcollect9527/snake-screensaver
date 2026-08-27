// 原生屏保构建：用 TinyCC 编译 C 逻辑自检 + 生成 release/SnakeScreensaver.scr
// 用法：node scripts/build-win.cjs [--test-only]
// 说明：选用 tcc 是因为它自包含（编译+汇编+链接一体），不依赖可能被杀软按文件名
//       拦截的 as.exe/ld.exe；tcc 产物也极小（~30KB）。
const { execSync } = require('child_process');
const { existsSync, mkdirSync } = require('fs');
const { join } = require('path');
const os = require('os');

const ROOT = join(__dirname, '..');

function findTcc() {
  const candidates = [
    process.env.TCC,
    join('C:', 'Users', os.userInfo().username, 'tcc', 'tcc.exe'),
    join(process.env.LOCALAPPDATA || '', 'tcc', 'tcc.exe'),
  ].filter(Boolean);
  for (const c of candidates) if (existsSync(c)) return c;
  try { execSync('tcc -v', { stdio: 'ignore' }); return 'tcc'; } catch {}
  throw new Error('未找到 tcc。请将 tcc.exe 放到 ~/tcc/ 或设置 TCC 环境变量');
}

function sh(cmd) {
  console.log('> ' + cmd);
  execSync(cmd, { stdio: 'inherit', cwd: ROOT });
}

const tcc = findTcc();
mkdirSync(join(ROOT, 'build'), { recursive: true });
mkdirSync(join(ROOT, 'release'), { recursive: true });
const testOnly = process.argv.includes('--test-only');

// 1. 逻辑自检（控制台）
sh(`"${tcc}" src/win/game_test.c src/win/game.c -o build\\game_test.exe -I src/win`);
sh('build\\game_test.exe');

// 2. 屏保本体（GUI 子系统，直接输出 .scr）
if (!testOnly) {
  sh(`"${tcc}" src/win/screensaver.c src/win/game.c -o release\\SnakeScreensaver.scr -I src/win -mwindows`);
  console.log('OK: release/SnakeScreensaver.scr');
}
