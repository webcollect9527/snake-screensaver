// 蛇状态：离散网格步进、增长（新增尾部单元着色）、碰撞检测。
import type { Point } from './grid';

export interface Segment {
  x: number;
  y: number;
  color: string;
}

export class Snake {
  segments: Segment[]; // 头在前
  dir: Point;

  constructor(start: Point, dir: Point, len = 3, color = '#ffffff') {
    this.dir = { ...dir };
    this.segments = [];
    for (let i = 0; i < len; i++) {
      this.segments.push({ x: start.x - dir.x * i, y: start.y - dir.y * i, color });
    }
  }

  get head(): Point {
    return this.segments[0];
  }

  nextHead(dir: Point = this.dir): Point {
    return { x: this.head.x + dir.x, y: this.head.y + dir.y };
  }

  positions(): Point[] {
    return this.segments.map((s) => ({ x: s.x, y: s.y }));
  }

  // 头下一步是否会撞到身体。grow=true 时尾部不挪走，撞尾也算碰撞。
  collidesWithBody(grow: boolean): boolean {
    const nh = this.nextHead();
    const skipTail = grow ? 0 : 1;
    for (let i = 0; i < this.segments.length - skipTail; i++) {
      const s = this.segments[i];
      if (s.x === nh.x && s.y === nh.y) return true;
    }
    return false;
  }

  // grow=true 时新增一个单元（长度+1，尾部不挪走）；新增单元颜色 = 被吃方块颜色。
  step(dir: Point, grow: boolean, newUnitColor?: string): Point {
    this.dir = { ...dir };
    const nh = this.nextHead();
    const headColor = this.segments[0].color;
    this.segments.unshift({
      x: nh.x,
      y: nh.y,
      color: grow ? (newUnitColor ?? headColor) : headColor,
    });
    if (!grow) this.segments.pop();
    return nh;
  }
}
