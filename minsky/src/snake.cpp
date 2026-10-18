#include "snake.hpp"

#include <stdexcept>
#include <string>

namespace snake {

using bits::BitAsm;
using bits::Bits;

Program compile(const Options& o) {
  // The body starts three cells wide, and a full board would spin the fruit search forever.
  if (o.width < 4 || o.width > 16 || o.height < 2 || o.height > 16 || o.width * o.height < 16)
    throw std::invalid_argument("board must be at least 4 wide, 2 high and 16 cells");
  if (o.growth < 0 || o.pad < 0) throw std::invalid_argument("growth and pad must be non-negative");
  if (o.wrap && (o.width != 16 || o.height != 16)) throw std::invalid_argument("wrap needs a 16x16 board");
  if (o.seed == 0) throw std::invalid_argument("seed must be nonzero");
  Program P;
  P.opt = o;
  Layout& L = P.lay;
  L.w = o.width;
  L.h = o.height;
  int n = 0;
  auto alloc = [&] { return n++; };
  auto alloc_n = [&](int k) {
    Bits b(k);
    for (auto& x : b) x = alloc();
    return b;
  };
  L.pixel = alloc_n(L.w * L.h);
  L.dcell = alloc_n(2 * L.w * L.h);
  L.head_x = alloc_n(4);
  L.head_y = alloc_n(4);
  L.tail_x = alloc_n(4);
  L.tail_y = alloc_n(4);
  L.fruit_x = alloc_n(4);
  L.fruit_y = alloc_n(4);
  L.dir = alloc_n(2);
  L.last = alloc_n(2);
  L.lfsr = alloc_n(16);
  L.parity = alloc();
  L.grow = alloc();
  L.score = alloc();
  L.pad = alloc();
  for (int& k : L.keys) k = alloc();
  L.frame = alloc();
  L.death = alloc();
  L.nregs = n;

  mm::Asm a;
  BitAsm B(a);
  Bits head = L.head_x, tail = L.tail_x, fruit = L.fruit_x;
  head.insert(head.end(), L.head_y.begin(), L.head_y.end());
  tail.insert(tail.end(), L.tail_y.begin(), L.tail_y.end());
  fruit.insert(fruit.end(), L.fruit_y.begin(), L.fruit_y.end());
  auto in_range = [&](unsigned v) { return int(v & 15) < L.w && int(v >> 4) < L.h; };
  auto cell = [&](unsigned v) { return L.at(int(v & 15), int(v >> 4)); };

  const int INIT = a.label(), FRAME = a.label(), DIE = a.label(), TAILSTEP = a.label(), MOVE = a.label(),
            MOVE2 = a.label(), COLL = a.label(), FRUITCHK = a.label(), EAT = a.label();
  const int ENDFRAME = o.pad > 0 ? a.label() : FRAME;

  // A leaf that must jump somewhere but has nothing to do: one instruction on a scratch bit.
  auto jump = [&](int target) { a.dec(L.parity, target, target); };

  // New fruit: step the LFSR, take its low 8 bits, retry while the cell is occupied or off the
  // board. Ends at `cont`.
  auto emit_new_fruit = [&](int cont) {
    const int NF = B.here();
    const int PODD = a.label(), PEVEN = a.label(), SH = a.label();
    B.parity({L.lfsr[0], L.lfsr[2], L.lfsr[3], L.lfsr[5]}, PODD, PEVEN);
    a.bind(PODD);
    a.inc(L.parity, SH);
    a.bind(PEVEN);
    a.bind(SH);
    for (int i = 0; i < 15; ++i) {
      int nx = a.label();
      B.move(L.lfsr[i + 1], L.lfsr[i], nx);
      a.bind(nx);
    }
    const int P1 = a.label(), CP = a.label();
    a.dec(L.parity, P1, CP);
    a.bind(P1);
    a.inc(L.lfsr[15], CP);
    a.bind(CP);
    const int DEC = a.label();
    B.copy_n(Bits(L.lfsr.begin(), L.lfsr.begin() + 4), L.fruit_x, DEC);
    a.bind(DEC);
    const int DEC2 = a.label();
    B.copy_n(Bits(L.lfsr.begin() + 4, L.lfsr.begin() + 8), L.fruit_y, DEC2);
    a.bind(DEC2);
    B.decode(fruit, [&](unsigned v) {
      if (!in_range(v)) jump(NF);
      else B.read(L.pixel[cell(v)], NF, cont);
    });
  };

  // ---- START: seed the LFSR once, then INIT ----
  B.write_n(L.lfsr, o.seed, INIT);

  // ---- INIT: a new game ----
  a.bind(INIT);
  P.init_instr = static_cast<int>(a.size());
  {
    Bits all = L.pixel;
    all.insert(all.end(), L.dcell.begin(), L.dcell.end());
    int G = a.label();
    B.clear_n(all, G);
    a.bind(G);
    int GL = B.here(), SL = a.label();
    a.dec(L.grow, GL, SL);
    a.bind(SL);
    int SL2 = B.here(), S2 = a.label();
    a.dec(L.score, SL2, S2);
    a.bind(S2);
    const int cx = L.w / 2, cy = L.h / 2;
    int l1 = a.label(), l2 = a.label(), l3 = a.label(), l4 = a.label(), l5 = a.label(), l6 = a.label(),
        l7 = a.label(), l8 = a.label(), l9 = a.label(), l10 = a.label(), l11 = a.label(), l12 = a.label();
    B.write_n(L.head_x, cx, l1);
    a.bind(l1);
    B.write_n(L.head_y, cy, l2);
    a.bind(l2);
    B.write_n(L.tail_x, cx - 2, l3);
    a.bind(l3);
    B.write_n(L.tail_y, cy, l4);
    a.bind(l4);
    B.write1(L.pixel[L.at(cx - 2, cy)], l5);
    a.bind(l5);
    B.write1(L.pixel[L.at(cx - 1, cy)], l6);
    a.bind(l6);
    B.write1(L.pixel[L.at(cx, cy)], l7);
    a.bind(l7);
    B.write1(L.dcell[2 * L.at(cx - 2, cy)], l8);
    a.bind(l8);
    B.write1(L.dcell[2 * L.at(cx - 2, cy) + 1], l9);
    a.bind(l9);
    B.write1(L.dcell[2 * L.at(cx - 1, cy)], l10);
    a.bind(l10);
    B.write1(L.dcell[2 * L.at(cx - 1, cy) + 1], l11);
    a.bind(l11);
    B.write_n(L.dir, 3, l12);
    a.bind(l12);
    int NF = a.label();
    B.write_n(L.last, 3, NF);
    a.bind(NF);
    emit_new_fruit(FRAME);
  }

  // ---- FRAME ----
  a.bind(FRAME);
  P.frame_instr = static_cast<int>(a.size());
  {
    int F1 = a.label(), F2 = a.label();
    a.inc(L.frame, F1);
    a.bind(F1);
    a.dec(L.frame, F2, F2);  // one trade in the frame book per frame
    a.bind(F2);
  }
  // input: drain each key book; a press turns the snake unless it would reverse
  for (int k = 0; k < 4; ++k) {
    const int KL = B.here(), KA = a.label(), KN = k < 3 ? a.label() : MOVE, KSET = a.label();
    a.dec(L.keys[k], KA, KN);
    a.bind(KA);
    B.eq_const(L.last, static_cast<unsigned>(k ^ 1), KL, KSET);
    a.bind(KSET);
    B.write_n(L.dir, static_cast<unsigned>(k), KL);
    if (k < 3) a.bind(KN);
  }
  // move: remember the direction, write it into the cell the head leaves, advance the head
  a.bind(MOVE);
  {
    int DC = a.label();
    B.copy_n(L.dir, L.last, DC);
    a.bind(DC);
    B.decode(head, [&](unsigned v) {
      if (!in_range(v)) {
        jump(DIE);
        return;
      }
      const int c = cell(v);
      int m = a.label();
      B.copy(L.dir[0], L.dcell[2 * c], m);
      a.bind(m);
      B.copy(L.dir[1], L.dcell[2 * c + 1], MOVE2);
    });
  }
  a.bind(MOVE2);
  {
    const int UP = a.label(), DOWN = a.label(), LEFT = a.label(), RIGHT = a.label();
    B.branch2(L.dir[1], L.dir[0], UP, DOWN, LEFT, RIGHT);
    a.bind(UP);
    B.dec_n(L.head_y, COLL, o.wrap ? COLL : DIE);
    a.bind(DOWN);
    if (L.h < 16) {
      int CH = a.label();
      B.inc_n(L.head_y, CH, DIE);
      a.bind(CH);
      B.eq_const(L.head_y, static_cast<unsigned>(L.h), DIE, COLL);
    } else {
      B.inc_n(L.head_y, COLL, o.wrap ? COLL : DIE);
    }
    a.bind(LEFT);
    B.dec_n(L.head_x, COLL, o.wrap ? COLL : DIE);
    a.bind(RIGHT);
    if (L.w < 16) {
      int CH = a.label();
      B.inc_n(L.head_x, CH, DIE);
      a.bind(CH);
      B.eq_const(L.head_x, static_cast<unsigned>(L.w), DIE, COLL);
    } else {
      B.inc_n(L.head_x, COLL, o.wrap ? COLL : DIE);
    }
  }
  // collision test and draw: the pixel under the new head
  a.bind(COLL);
  B.decode(head, [&](unsigned v) {
    if (!in_range(v)) {
      jump(DIE);
      return;
    }
    const int c = cell(v);
    int HIT = a.label(), FREE = a.label();
    a.dec(L.pixel[c], HIT, FREE);
    a.bind(HIT);
    a.inc(L.pixel[c], DIE);
    a.bind(FREE);
    a.inc(L.pixel[c], FRUITCHK);
  });
  a.bind(FRUITCHK);
  {
    int FX = a.label();
    B.eq_n(L.head_x, L.fruit_x, FX, TAILSTEP);
    a.bind(FX);
    B.eq_n(L.head_y, L.fruit_y, EAT, TAILSTEP);
  }
  a.bind(EAT);
  {
    int g = a.label();
    a.inc(L.score, g);
    a.bind(g);
    for (int i = 0; i < o.growth; ++i) {
      int nx = a.label();
      a.inc(L.grow, nx);
      a.bind(nx);
    }
    emit_new_fruit(TAILSTEP);
  }
  // tail: if growing, keep it; otherwise clear its pixel and follow the stored direction
  a.bind(TAILSTEP);
  {
    const int T1 = a.label(), TUP = a.label(), TDOWN = a.label(), TLEFT = a.label(), TRIGHT = a.label();
    a.dec(L.grow, ENDFRAME, T1);
    a.bind(T1);
    B.decode(tail, [&](unsigned v) {
      if (!in_range(v)) {
        jump(ENDFRAME);
        return;
      }
      const int c = cell(v);
      int t2 = a.label(), hi = a.label(), lo = a.label();
      B.clear(L.pixel[c], t2);
      a.bind(t2);
      a.dec(L.dcell[2 * c + 1], hi, lo);
      a.bind(hi);
      a.dec(L.dcell[2 * c], TRIGHT, TLEFT);
      a.bind(lo);
      a.dec(L.dcell[2 * c], TDOWN, TUP);
    });
    a.bind(TUP);
    B.dec_n(L.tail_y, ENDFRAME, ENDFRAME);
    a.bind(TDOWN);
    B.inc_n(L.tail_y, ENDFRAME, ENDFRAME);
    a.bind(TLEFT);
    B.dec_n(L.tail_x, ENDFRAME, ENDFRAME);
    a.bind(TRIGHT);
    B.inc_n(L.tail_x, ENDFRAME, ENDFRAME);
  }
  if (o.pad > 0) {
    a.bind(ENDFRAME);
    for (int i = 0; i < o.pad; ++i) {
      int nx = a.label();
      a.inc(L.pad, nx);
      a.bind(nx);
    }
    int PL = B.here();
    a.dec(L.pad, PL, FRAME);
  }
  // death: one trade in the death book, then a new game
  a.bind(DIE);
  P.die_instr = static_cast<int>(a.size());
  {
    int d1 = a.label();
    a.inc(L.death, d1);
    a.bind(d1);
    a.dec(L.death, INIT, INIT);
  }
  P.prog = a.finish();
  return P;
}

tbl::Layout layout_file(const Program& p, int clock_book, int P, int key_account) {
  tbl::Layout l;
  const Layout& L = p.lay;
  auto put = [&](const char* k, const Bits& b) { l.v[k] = std::vector<long long>(b.begin(), b.end()); };
  l.v["P"] = {P};
  l.v["backstop"] = {P + 1};
  l.v["width"] = {L.w};
  l.v["height"] = {L.h};
  put("pixels", L.pixel);
  l.v["keys"] = {L.keys[0], L.keys[1], L.keys[2], L.keys[3]};
  l.v["frame"] = {L.frame};
  l.v["death"] = {L.death};
  l.v["score"] = {L.score};
  l.v["grow"] = {L.grow};
  put("fruit_x", L.fruit_x);
  put("fruit_y", L.fruit_y);
  put("head_x", L.head_x);
  put("head_y", L.head_y);
  put("tail_x", L.tail_x);
  put("tail_y", L.tail_y);
  put("dir", L.dir);
  put("last", L.last);
  put("lfsr", L.lfsr);
  l.v["clock"] = {clock_book};
  l.v["key_account"] = {key_account};
  l.v["nbooks"] = {L.nregs + 1};
  l.v["wrap"] = {p.opt.wrap};
  l.v["growth"] = {p.opt.growth};
  l.v["pad"] = {p.opt.pad};
  return l;
}

// ---- the reference game ----
Sim::Sim(const Options& o) : opt(o), pixel(o.width * o.height, 0), dcell(2 * o.width * o.height, 0), lfsr(o.seed) {
  reset_game();
}

void Sim::lfsr_step() {
  const uint16_t bit = (lfsr ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
  lfsr = static_cast<uint16_t>((lfsr >> 1) | (bit << 15));
}

void Sim::new_fruit() {
  for (;;) {
    lfsr_step();
    fx = lfsr & 15;
    fy = (lfsr >> 4) & 15;
    if (fx < opt.width && fy < opt.height && !pixel[fy * opt.width + fx]) return;
  }
}

void Sim::reset_game() {
  std::fill(pixel.begin(), pixel.end(), 0);
  std::fill(dcell.begin(), dcell.end(), 0);
  grow = score = 0;
  const int cx = opt.width / 2, cy = opt.height / 2;
  hx = cx;
  hy = cy;
  tx = cx - 2;
  ty = cy;
  auto at = [&](int x, int y) { return y * opt.width + x; };
  pixel[at(cx - 2, cy)] = pixel[at(cx - 1, cy)] = pixel[at(cx, cy)] = 1;
  dcell[2 * at(cx - 2, cy)] = dcell[2 * at(cx - 2, cy) + 1] = 1;
  dcell[2 * at(cx - 1, cy)] = dcell[2 * at(cx - 1, cy) + 1] = 1;
  dir = last = 3;
  new_fruit();
}

void Sim::step(const int keys[4]) {
  ++frames;
  for (int k = 0; k < 4; ++k)
    if (keys[k] > 0 && last != (k ^ 1)) dir = k;
  last = dir;
  auto at = [&](int x, int y) { return y * opt.width + x; };
  dcell[2 * at(hx, hy)] = dir & 1;
  dcell[2 * at(hx, hy) + 1] = (dir >> 1) & 1;
  auto die = [&] {
    ++deaths;
    reset_game();
  };
  switch (dir) {
    case 0:
      if (hy == 0) {
        if (!opt.wrap) return die();
        hy = 15;
      } else {
        --hy;
      }
      break;
    case 1:
      ++hy;
      if (hy >= opt.height) {
        if (!opt.wrap) return die();
        hy = 0;
      }
      break;
    case 2:
      if (hx == 0) {
        if (!opt.wrap) return die();
        hx = 15;
      } else {
        --hx;
      }
      break;
    default:
      ++hx;
      if (hx >= opt.width) {
        if (!opt.wrap) return die();
        hx = 0;
      }
      break;
  }
  if (pixel[at(hx, hy)]) return die();
  pixel[at(hx, hy)] = 1;
  if (hx == fx && hy == fy) {
    ++score;
    grow += opt.growth;
    new_fruit();
  }
  if (grow > 0) {
    --grow;
    return;
  }
  const int c = at(tx, ty);
  pixel[c] = 0;
  const int d = dcell[2 * c] | (dcell[2 * c + 1] << 1);
  dcell[2 * c] = dcell[2 * c + 1] = 0;
  switch (d) {
    case 0: ty = (ty - 1) & 15; break;
    case 1: ty = (ty + 1) & 15; break;
    case 2: tx = (tx - 1) & 15; break;
    default: tx = (tx + 1) & 15; break;
  }
}

std::string Sim::diff(const mm::Regs& r, const Layout& L) const {
  auto bitsval = [&](const Bits& b) {
    unsigned v = 0;
    for (size_t i = 0; i < b.size(); ++i) v |= static_cast<unsigned>(r[b[i]] & 1) << i;
    return v;
  };
  auto chk = [&](const std::string& what, long long got, long long want) {
    return got == want ? std::string() : what + " " + std::to_string(got) + " vs " + std::to_string(want);
  };
  for (int i = 0; i < opt.width * opt.height; ++i) {
    if (r[L.pixel[i]] != pixel[i]) return "pixel " + std::to_string(i) + " " + std::to_string(r[L.pixel[i]]) + " vs " + std::to_string(pixel[i]);
    for (int b = 0; b < 2; ++b)
      if (r[L.dcell[2 * i + b]] != dcell[2 * i + b]) return "dcell " + std::to_string(i) + "." + std::to_string(b);
  }
  std::string d;
  if (!(d = chk("head_x", bitsval(L.head_x), hx)).empty()) return d;
  if (!(d = chk("head_y", bitsval(L.head_y), hy)).empty()) return d;
  if (!(d = chk("tail_x", bitsval(L.tail_x), tx)).empty()) return d;
  if (!(d = chk("tail_y", bitsval(L.tail_y), ty)).empty()) return d;
  if (!(d = chk("fruit_x", bitsval(L.fruit_x), fx)).empty()) return d;
  if (!(d = chk("fruit_y", bitsval(L.fruit_y), fy)).empty()) return d;
  if (!(d = chk("dir", bitsval(L.dir), dir)).empty()) return d;
  if (!(d = chk("last", bitsval(L.last), last)).empty()) return d;
  if (!(d = chk("lfsr", bitsval(L.lfsr), lfsr)).empty()) return d;
  if (!(d = chk("grow", r[L.grow], grow)).empty()) return d;
  if (!(d = chk("score", r[L.score], score)).empty()) return d;
  if (!(d = chk("parity", r[L.parity], 0)).empty()) return d;
  if (!(d = chk("pad", r[L.pad], 0)).empty()) return d;
  if (!(d = chk("frame", r[L.frame], 0)).empty()) return d;
  if (!(d = chk("death", r[L.death], 0)).empty()) return d;
  for (int k = 0; k < 4; ++k)
    if (!(d = chk("key" + std::to_string(k), r[L.keys[k]], 0)).empty()) return d;
  return "";
}

}  // namespace snake
