// A scripted Snake player used by the tests and the canary: a breadth-first path to the fruit,
// with (unless `pure`) occasional random keys, idle frames and double presses.
#pragma once
#include <algorithm>
#include <random>
#include <vector>

#include "snake.hpp"

namespace snake {
// A player: mostly a breadth-first path to the fruit, sometimes random, sometimes idle or
// pressing two keys in one frame, so long snakes and odd inputs are both exercised.
struct Player {
  std::mt19937_64 g;
  bool pure;
  explicit Player(uint64_t seed, bool pure_ = false) : g(seed), pure(pure_) {}
  int bfs_dir(const snake::Sim& s) {
    const int W = s.opt.width, H = s.opt.height;
    std::vector<int> dist(W * H, -1), from(W * H, -1);
    std::vector<int> q;
    q.push_back(s.hy * W + s.hx);
    dist[q[0]] = 0;
    int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    for (size_t qi = 0; qi < q.size(); ++qi) {
      int c = q[qi], x = c % W, y = c / W;
      if (x == s.fx && y == s.fy) break;
      for (int d = 0; d < 4; ++d) {
        if (c == q[0] && d == (s.last ^ 1)) continue;
        int nx = x + dx[d], ny = y + dy[d];
        if (s.opt.wrap) nx &= 15, ny &= 15;
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
        int nc = ny * W + nx;
        // the tail cell will be free unless the snake is growing
        if (s.pixel[nc] && !(nc == s.ty * W + s.tx && s.grow == 0)) continue;
        if (dist[nc] >= 0) continue;
        dist[nc] = dist[c] + 1;
        from[nc] = d;
        q.push_back(nc);
      }
    }
    int goal = s.fy * W + s.fx;
    if (dist[goal] < 0) {
      // no path: any safe move
      for (int d = 0; d < 4; ++d) {
        if (d == (s.last ^ 1)) continue;
        int nx = s.hx + dx[d], ny = s.hy + dy[d];
        if (s.opt.wrap) nx &= 15, ny &= 15;
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
        if (!s.pixel[ny * W + nx]) return d;
      }
      return -1;
    }
    // walk back to the first step
    int c = goal;
    while (dist[c] > 1) {
      int d = from[c];
      int x = c % W, y = c / W;
      x -= dx[d];
      y -= dy[d];
      if (s.opt.wrap) x &= 15, y &= 15;
      c = y * W + x;
    }
    return from[c];
  }
  void keys(const snake::Sim& s, int out[4]) {
    for (int k = 0; k < 4; ++k) out[k] = 0;
    std::uniform_int_distribution<int> pct(0, 99);
    int x = pure ? 99 : pct(g);
    if (x < 10) return;  // no input
    if (x < 18) {
      out[std::uniform_int_distribution<int>(0, 3)(g)] += 1 + (pct(g) < 20);
      if (pct(g) < 30) out[std::uniform_int_distribution<int>(0, 3)(g)] += 1;  // two keys in one frame
      return;
    }
    int d = bfs_dir(s);
    if (d >= 0) out[d] = 1;
  }
};


}  // namespace snake
