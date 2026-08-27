// 跨会话统计：读写 %APPDATA%\snake-screensaver\stats.json。
import * as fs from 'fs';
import * as path from 'path';
import * as os from 'os';

export interface PersistedStats {
  maxSurvivalSeconds: number;
  successCount: number;
  failCount: number;
  colorCounts: number[];
  totalScore: number;
}

const DEFAULTS: PersistedStats = {
  maxSurvivalSeconds: 0,
  successCount: 0,
  failCount: 0,
  colorCounts: [0, 0, 0, 0, 0, 0, 0],
  totalScore: 0,
};

export function statsFilePath(): string {
  const base = process.env.APPDATA ?? path.join(os.homedir(), 'AppData', 'Roaming');
  return path.join(base, 'snake-screensaver', 'stats.json');
}

export function loadStats(file: string = statsFilePath()): PersistedStats {
  try {
    const raw = fs.readFileSync(file, 'utf8');
    const parsed = JSON.parse(raw) as Partial<PersistedStats>;
    return {
      ...DEFAULTS,
      ...parsed,
      colorCounts: Array.isArray(parsed.colorCounts) ? parsed.colorCounts : [...DEFAULTS.colorCounts],
    };
  } catch {
    return { ...DEFAULTS, colorCounts: [...DEFAULTS.colorCounts] };
  }
}

export function saveStats(s: PersistedStats, file: string = statsFilePath()): void {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(s, null, 2));
}
