// Snake, compiled to one-bit registers (order books), and a direct simulator of the same game
// with the same rules, bit for bit: the simulator is the reference the compiled program is
// checked against, in the interpreter, on the in-process engine, and over the wire.
#pragma once
#include <cstdint>
#include <vector>

#include "bitasm.hpp"
#include "table.hpp"

namespace snake {

struct Options {
  int width = 16, height = 16;  // the board is width x height; both <= 16 (4-bit coordinates)
  bool wrap = false;            // true: the board is a torus; false: walls kill
  int growth = 3;               // segments gained per fruit
  int pad = 0;                  // extra instructions burned per frame (a counter loop), for the law demo
  uint16_t seed = 0xACE1;       // LFSR seed (nonzero)
};

// Where everything lives. Book indices are registers of the Minsky program.
struct Layout {
  int w = 16, h = 16;
  std::vector<int> pixel;   // h*w, row-major
  std::vector<int> dcell;   // 2 per cell: direction the snake moved when it left the cell
  bits::Bits head_x, head_y, tail_x, tail_y, fruit_x, fruit_y;  // 4 bits each
  bits::Bits dir, last;     // 2 bits: 0 up, 1 down, 2 left, 3 right
  bits::Bits lfsr;          // 16 bits
  int parity = 0;           // scratch bit
  int grow = 0, score = 0;  // unbounded counters
  int pad = 0;              // counter for the pad loop
  int keys[4] = {0, 0, 0, 0};
  int frame = 0, death = 0;
  int nregs = 0;
  int at(int x, int y) const { return y * w + x; }
};

struct Program {
  mm::MProg prog;
  Layout lay;
  int frame_instr = -1, die_instr = -1, init_instr = -1;
  Options opt;
};

Program compile(const Options& o);
tbl::Layout layout_file(const Program& p, int clock_book, int P, int key_account);

// The reference game. step() consumes key presses (counts per key, in order up/down/left/right;
// every press is applied: if the direction is not the reverse of the last move, it becomes the
// new direction) and plays one frame. Mirrors the compiled program exactly, including the
// LFSR and the fruit placement retry.
struct Sim {
  explicit Sim(const Options& o);
  void reset_game();
  void step(const int keys[4]);
  // state
  Options opt;
  std::vector<int> pixel;  // h*w
  std::vector<int> dcell;  // 2 per cell
  int hx = 0, hy = 0, tx = 0, ty = 0, fx = 0, fy = 0;
  int dir = 3, last = 3;
  uint16_t lfsr = 0;
  int64_t grow = 0, score = 0;
  uint64_t frames = 0, deaths = 0;
  void new_fruit();
  void lfsr_step();
  // Compare with a register file laid out per `lay`; returns a description of the first
  // difference or "" if identical.
  std::string diff(const mm::Regs& regs, const Layout& lay) const;
};

}  // namespace snake
