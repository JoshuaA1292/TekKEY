// Snake, three ways against the reference simulator, frame by frame:
//   1. the Minsky program in an interpreter (fast; many configurations, thousands of frames)
//   2. the compiled order program on the in-process stage 1 engine, state read from the books
//   3. (--wire URL) the router against a live exchange, keys injected as external orders on a
//      second session, state read back with control messages
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <algorithm>
#include <string>

#include "engine.hpp"
#include "minsky.hpp"
#include "router.hpp"
#include "snake.hpp"
#include "snake_player.hpp"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

void interpreter_test(const snake::Options& o, int frames, uint64_t seed, const char* name, bool pure = false) {
  snake::Program p = snake::compile(o);
  snake::Sim sim(o);
  snake::Player pl(seed, pure);
  mm::Regs regs(p.lay.nregs, 0);
  int frame = 0, mism = 0;
  uint64_t instr_at_frame = 0, instr_total = 0, min_f = UINT64_MAX, max_f = 0;
  auto hook = [&](int pc, uint64_t steps) {
    if (pc != p.frame_instr) return true;
    if (frame > 0) {
      std::string d = sim.diff(regs, p.lay);
      if (!d.empty() && mism++ < 3) std::printf("  %s frame %d: %s\n", name, frame, d.c_str());
      uint64_t f = steps - instr_at_frame;
      min_f = std::min(min_f, f);
      max_f = std::max(max_f, f);
    } else {
      std::string d = sim.diff(regs, p.lay);
      if (!d.empty() && mism++ < 3) std::printf("  %s initial state: %s\n", name, d.c_str());
    }
    instr_at_frame = steps;
    if (frame == frames) return false;
    int keys[4];
    pl.keys(sim, keys);
    for (int k = 0; k < 4; ++k) regs[p.lay.keys[k]] += keys[k];
    sim.step(keys);
    ++frame;
    return true;
  };
  bits::HookRun r = bits::run_with_hook(p.prog, regs, 200'000'000, hook);
  instr_total = r.steps;
  check(r.status == bits::HookRun::Stopped, std::string(name) + " ran to the frame limit");
  check(mism == 0, std::string(name) + " matched the simulator on every frame");
  std::printf("  %-22s %6d frames, %4llu deaths, best score %lld, %llu instructions (%.0f/frame, min %llu, max %llu), "
              "%zu-instruction program, %d mismatches\n",
              name, frames, (unsigned long long)sim.deaths, (long long)sim.score, (unsigned long long)instr_total,
              double(instr_total) / frames, (unsigned long long)min_f, (unsigned long long)max_f, p.prog.size(), mism);
}

// The compiled order program on the in-process engine.
void engine_test(const snake::Options& o, int frames, uint64_t seed) {
  snake::Program p = snake::compile(o);
  mm::CompileOpts co;
  co.persistent_backstop = co.inc_clock = co.two_accounts = true;
  mm::Compiled c = mm::compile(p.prog, p.lay.nregs, co);
  ob::Engine eng(c.nbooks(), 3, ob::STP::ExpireMaker, &c.prog);
  eng.log_trades = false;
  snake::Sim sim(o);
  snake::Player pl(seed);
  int frame = 0, mism = 0;
  auto read = [&] {
    mm::Regs r(p.lay.nregs);
    for (int i = 0; i < p.lay.nregs; ++i) r[i] = eng.resting_qty(c.geo[i].book, c.geo[i].side, c.geo[i].reg);
    return r;
  };
  uint64_t instrs = 0;
  eng.on_action = [&](const ob::Action& a) {
    int j = a.kind == ob::Act::Submit ? c.entry_instr[a.arg] : -1;
    if (j < 0) return;
    ++instrs;
    if (j != p.frame_instr) return;
    std::string d = sim.diff(read(), p.lay);
    if (!d.empty() && mism++ < 3) std::printf("  engine frame %d: %s\n", frame, d.c_str());
    if (frame == frames) {
      eng.stop_requested = true;
      return;
    }
    int keys[4];
    pl.keys(sim, keys);
    for (int k = 0; k < 4; ++k)
      for (int n = 0; n < keys[k]; ++n)
        eng.submit({2, c.geo[p.lay.keys[k]].book, ob::Side::Sell, ob::Type::Limit, c.geo[p.lay.keys[k]].reg, 0, 1, false});
    sim.step(keys);
    ++frame;
  };
  eng.start();
  ob::Status st = eng.run();
  check(st == ob::Status::Stopped, "engine run stopped at the frame limit");
  check(mism == 0, "engine matched the simulator on every frame");
  ob::Qty peak = 0;
  for (int b = 0; b < c.nbooks(); ++b) peak = std::max(peak, eng.peak_lots(b));
  std::printf("  engine: %d frames, %llu deaths, best score %lld, %llu trades, %llu instructions, peak %lld lots in a "
              "book, %zu resting orders, %d mismatches\n",
              frames, (unsigned long long)sim.deaths, (long long)sim.score, (unsigned long long)eng.trade_count(),
              (unsigned long long)instrs, (long long)peak, eng.resting_orders(), mism);
}

// Over the wire.
void wire_test(const std::string& url, const std::string& keys_url, const snake::Options& o, int frames, uint64_t seed) {
  snake::Program p = snake::compile(o);
  mm::CompileOpts co;
  co.persistent_backstop = co.inc_clock = co.two_accounts = true;
  mm::Compiled c = mm::compile(p.prog, p.lay.nregs, co);
  tbl::Table t;
  t.prog = c.prog;
  t.entry_instr = c.entry_instr;
  t.books = c.nbooks();
  t.ninstr = static_cast<int>(p.prog.size());
  t.markers = {p.frame_instr, p.die_instr};
  auto ch = net::connect(url);
  auto keys_ch = net::connect(keys_url);  // a second session, like the visualiser's keyboard
  rt::control(*ch, proto::ResetBooks);
  rt::control(*ch, proto::SetSTP, 1);
  snake::Sim sim(o);
  snake::Player pl(seed);
  int frame = 0, mism = 0;
  auto read = [&] {
    mm::Regs r(p.lay.nregs);
    for (int i = 0; i < p.lay.nregs; ++i)
      r[i] = rt::book_qty(*keys_ch, static_cast<uint32_t>(c.geo[i].book), static_cast<uint8_t>(c.geo[i].side), c.geo[i].reg);
    return r;
  };
  rt::Router router(t, *ch);
  router.on_marker = [&](int mk) {
    if (mk != 0) return;
    // the router has flushed nothing new for this frame yet: the books are at the frame boundary
    std::string d = sim.diff(read(), p.lay);
    if (!d.empty() && mism++ < 3) std::printf("  wire frame %d: %s\n", frame, d.c_str());
    if (frame < frames) {
      int keys[4];
      pl.keys(sim, keys);
      for (int k = 0; k < 4; ++k)
        for (int n = 0; n < keys[k]; ++n) {
          proto::NewOrderMsg m;
          m.account = 2;
          m.book = static_cast<uint32_t>(c.geo[p.lay.keys[k]].book);
          m.side = proto::Sell;
          m.price = c.geo[p.lay.keys[k]].reg;
          m.qty = 1;
          m.tag = 0xfffffffeu;
          rt::submit_sync(*keys_ch, m);  // in the book before the router sends anything
        }
      sim.step(keys);
    }
    ++frame;
  };
  router.on_instr = [&](int, uint64_t) { return frame <= frames; };
  rt::Options ro;
  ro.max_ns_wait = 5'000'000'000ull;
  rt::Status st = router.run(ro);
  const auto& s = router.stats();
  check(st == rt::Status::StepLimit, std::string("wire run stopped at the frame limit: ") + rt::status_name(st));
  check(mism == 0, "wire matched the simulator on every frame");
  std::printf("  wire (%s): %d frames, %llu deaths, best score %lld, %llu instructions, %llu waits, %.1f us/instr, "
              "%.0f fps unpaced, %d mismatches\n",
              ch->kind(), frames, (unsigned long long)sim.deaths, (long long)sim.score,
              (unsigned long long)s.instructions, (unsigned long long)s.waits, s.ns_total / 1e3 / s.instructions,
              1e9 * s.frames / s.ns_total, mism);
}
}  // namespace

int main(int argc, char** argv) {
  std::string wire, keys;
  int scale = 1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--wire" && i + 1 < argc) wire = argv[++i];
    else if (a == "--keys" && i + 1 < argc) keys = argv[++i];  // the keyboard's own session (shm serves one client)
    else if (a == "--quick") scale = 10;
  }
  snake::Options o;
  interpreter_test(o, 20000 / scale, 1, "16x16 walls");
  snake::Options w = o;
  w.wrap = true;
  interpreter_test(w, 20000 / scale, 2, "16x16 wrap");
  snake::Options g1 = o;
  g1.growth = 1;
  g1.seed = 0x1234;
  interpreter_test(g1, 10000 / scale, 3, "16x16 growth 1");
  snake::Options small = o;
  small.width = 10;
  small.height = 8;
  interpreter_test(small, 10000 / scale, 4, "10x8 walls");
  snake::Options padded = o;
  padded.pad = 50;
  interpreter_test(padded, 2000 / scale, 5, "16x16 pad 50");
  snake::Options big = o;
  big.growth = 8;
  interpreter_test(big, 20000 / scale, 6, "16x16 growth 8");
  interpreter_test(o, 20000 / scale, 9, "16x16 pure pathfinder", true);
  engine_test(o, 600 / scale, 7);
  if (!wire.empty()) wire_test(wire, keys.empty() ? wire : keys, o, 300 / scale, 8);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
