// 纯逻辑自检（无 GUI 可运行）：AI 整局模拟 + 关键行为断言。
// 编译：gcc src/win/game_test.c src/win/game.c -o game_test.exe
#include <stdio.h>
#include <stdlib.h>
#include "game.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

// AI 整局模拟（与 TS tests/sim.test.ts 同种子同算法）
static void sim(void) {
  const int GAMES = 10;
  double totalSurvival = 0;
  int sumAte = 0;
  for (int g = 0; g < GAMES; g++) {
    Game game;
    game_init(&game, 48, 27, 1000u + (unsigned)g);
    for (double t = 0; t < 60 * 1000.0; t += 16.0) {
      game_update(&game, 16.0);
      if (game.state == 1) break;
    }
    for (int i = 0; i < 7; i++) sumAte += game.colorCounts[i];
    totalSurvival += game.survival;
    game_free(&game);
  }
  double avg = totalSurvival / GAMES;
  printf("sim: avg survival %.1fs, ate %d\n", avg, sumAte);
  CHECK(sumAte > 0, "AI 应吃到方块");
  CHECK(avg > 10.0, "AI 平均生存应 >10s");
}

// 吃方块：得分、计数、长度 +1
static void test_eat(void) {
  Game g;
  game_init(&g, 20, 20, 1);
  g.snake.len = 3;
  g.snake.seg[0] = (Point){10, 10};
  g.snake.seg[1] = (Point){9, 10};
  g.snake.seg[2] = (Point){8, 10};
  g.snake.dir = (Point){1, 0};
  g.blocks[0] = (Block){11, 10, 1, 60.0};
  g.blockCount = 1;
  game_update(&g, 100.0); // 基础速度 10 格/秒 → 一步
  CHECK(g.score == 1, "吃到 1 号方块得分 1");
  CHECK(g.colorCounts[0] == 1, "1 号色计数 +1");
  CHECK(g.snake.len == 4, "蛇长 +1");
  CHECK(g.state == 0, "仍在运行");
  game_free(&g);
}

// 完全被困且唯一出口是墙时撞墙死亡
static void test_wall_death(void) {
  Game g;
  game_init(&g, 3, 3, 1);
  g.snake.len = 5;
  g.snake.seg[0] = (Point){2, 1};
  g.snake.seg[1] = (Point){1, 1};
  g.snake.seg[2] = (Point){0, 1};
  g.snake.seg[3] = (Point){2, 2};
  g.snake.seg[4] = (Point){2, 0};
  g.snake.dir = (Point){1, 0};
  g.blockCount = 0;
  game_update(&g, 100.0);
  CHECK(g.state == 1, "被困撞墙应死亡");
  CHECK(g.reason == 2, "结束原因应为撞墙");
  game_free(&g);
}

int main(void) {
  sim();
  test_eat();
  test_wall_death();
  if (failures) {
    printf("%d FAILURES\n", failures);
    return 1;
  }
  printf("ALL OK\n");
  return 0;
}
