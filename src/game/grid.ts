// 网格与可操作区域（含缩场后的边界）。游戏逻辑以格为单位，与像素无关。
export interface Point {
  x: number;
  y: number;
}

// 可操作区域：始终是轴对齐矩形 [x0,x1) × [y0,y1)；区域外即围墙。
export interface Rect {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
}

export function key(p: Point): string {
  return p.x + ',' + p.y;
}

export class Grid {
  readonly w: number;
  readonly h: number;
  operable: Rect;

  constructor(w: number, h: number) {
    this.w = w;
    this.h = h;
    this.operable = { x0: 0, y0: 0, x1: w, y1: h };
  }

  isWall(x: number, y: number): boolean {
    const r = this.operable;
    return x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1;
  }

  contains(x: number, y: number): boolean {
    return x >= 0 && x < this.w && y >= 0 && y < this.h;
  }

  area(): number {
    const r = this.operable;
    return (r.x1 - r.x0) * (r.y1 - r.y0);
  }
}
