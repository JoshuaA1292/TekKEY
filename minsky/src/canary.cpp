// The canary: computation as a conformance test for a matching engine. Each deliberate bug
// (mutation) is switched on in the matching core, the same program is run through the same
// router against the correct core and the mutated one, and the first instruction at which the
// two runs part ways is the instructions-to-divergence. A program can only reveal a bug in a
// rule its orders exercise, so every row also reports which rules the program touched.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "matcher.hpp"
#include "minsky.hpp"
#include "router.hpp"
#include "snake.hpp"
#include "snake_player.hpp"

namespace {

struct Trace {
  std::vector<int> instrs;  // instruction index at every entry
  rt::Stats st;
  rt::Status status;
  uint64_t hash = 0;
};

fb::Config cfg_for(int books) {
  fb::Config c;
  c.books = static_cast<uint32_t>(std::max(books, 8));
  c.accounts = 4;
  c.stp = fb::STP::ExpireMaker;
  return c;
}

// Run a table in-process with a mutation mask, recording the instruction trace.
Trace run_table(const tbl::Table& t, uint64_t mutation, const std::vector<proto::NewOrderMsg>& preload,
                uint64_t max_instr, fb::STP stp = fb::STP::ExpireMaker) {
  ex::InProcChannel ch(cfg_for(t.books));
  ch.matcher().core().set_mutation(mutation);
  ch.matcher().core().set_stp(stp);
  for (const auto& m : preload) rt::submit_sync(ch, m);
  rt::Router r(t, ch);
  Trace tr;
  r.on_instr = [&](int j, uint64_t) {
    tr.instrs.push_back(j);
    return true;
  };
  rt::Options o;
  o.max_instr = max_instr;
  tr.status = r.run(o);
  tr.st = r.stats();
  // Quantities only: a mutation that merely reorders a queue without changing any quantity is
  // invisible to every program and should not count as a divergence.
  uint64_t h = 1469598103934665603ull;
  auto add = [&](int64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= static_cast<uint8_t>(v >> (8 * i));
      h *= 1099511628211ull;
    }
  };
  const fb::Core& core = ch.matcher().core();
  for (uint32_t b = 0; b < core.nbooks(); ++b) {
    core.for_each_level(b, [&](uint8_t side, int64_t px, int64_t total, uint32_t, auto&&) {
      add(side);
      add(px);
      add(total);
    });
    add(-7);
    add(core.last_price(b).value_or(-1));
  }
  tr.hash = h;
  return tr;
}

// First index where two traces differ, or -1 if identical (including length and final state).
long divergence(const Trace& a, const Trace& b) {
  size_t n = std::min(a.instrs.size(), b.instrs.size());
  for (size_t i = 0; i < n; ++i)
    if (a.instrs[i] != b.instrs[i]) return static_cast<long>(i + 1);
  if (a.instrs.size() != b.instrs.size()) return static_cast<long>(n + 1);
  if (a.hash != b.hash) return static_cast<long>(n);  // same trace, different books
  return -1;
}

std::string coverage(const rt::Stats& s) {
  char buf[160];
  std::snprintf(buf, sizeof buf, "partial fills %llu, IOC remainders %llu, stops %llu", (unsigned long long)s.partial_fills,
                (unsigned long long)s.ioc_remainders, (unsigned long long)s.stops_sent);
  return buf;
}

// ---- the probe: four sections, one per rule, each in its own book ----
tbl::Table probe_table() {
  using namespace ob;
  tbl::Table t;
  t.name = "probe";
  auto add = [&](int acct, int book, Side side, Type type, Price px, Price trig, Qty q) {
    t.prog.templates.push_back({acct, book, side, type, px, trig, q, false});
    t.entry_instr.push_back(static_cast<int>(t.prog.templates.size()) - 1);
    return static_cast<int>(t.prog.templates.size()) - 1;
  };
  auto link = [&](int src, std::optional<Price> at, int target) {
    t.prog.links.push_back({On::Fill, src, at, {{Act::Submit, target}}});
  };
  const Price P = 1000;
  // 1. time priority after a partial fill (book 0)
  int A = add(0, 0, Side::Sell, Type::Limit, P, 0, 2);
  int B = add(0, 0, Side::Sell, Type::Limit, P, 0, 1);
  int X1 = add(1, 0, Side::Buy, Type::IOC, P, 0, 1);
  int X2 = add(1, 0, Side::Buy, Type::IOC, P, 0, 1);
  int MA = add(0, 0, Side::Sell, Type::Limit, P + 10, 0, 1);  // "A traded"
  int MB = add(0, 0, Side::Sell, Type::Limit, P + 11, 0, 1);  // "B traded"
  link(A, std::nullopt, MA);
  link(B, std::nullopt, MB);
  // 2. IOC remainder must not rest (book 1)
  int I = add(1, 1, Side::Buy, Type::IOC, P - 1, 0, 1);
  int S = add(0, 1, Side::Sell, Type::Limit, P - 1, 0, 1);
  int MI = add(0, 1, Side::Sell, Type::Limit, P + 12, 0, 1);  // "the IOC was still there"
  link(S, std::nullopt, MI);
  // 3. stop side (book 2): last = P; a sell stop with trigger P-2 must not fire
  int R = add(0, 2, Side::Sell, Type::Limit, P, 0, 1);
  int Q = add(1, 2, Side::Buy, Type::IOC, P, 0, 1);
  int BID = add(1, 2, Side::Buy, Type::Limit, P - 5, 0, 1);
  int ST = add(0, 2, Side::Sell, Type::StopMarket, 0, P - 2, 1);
  int MS = add(0, 2, Side::Sell, Type::Limit, P + 13, 0, 1);  // "the stop fired"
  link(ST, std::nullopt, MS);
  // 4. price priority (book 3): asks at P and P+1, a buy at P+1 must print at P
  int L0 = add(0, 3, Side::Sell, Type::Limit, P, 0, 1);
  int L1 = add(0, 3, Side::Sell, Type::Limit, P + 1, 0, 1);
  int Y = add(1, 3, Side::Buy, Type::IOC, P + 1, 0, 1);
  int MP0 = add(0, 3, Side::Sell, Type::Limit, P + 14, 0, 1);  // "printed at P"
  int MP1 = add(0, 3, Side::Sell, Type::Limit, P + 15, 0, 1);  // "printed at P+1"
  link(Y, P, MP0);
  link(Y, P + 1, MP1);
  for (int x : {A, B, X1, X2, I, S, R, Q, BID, ST, L0, L1, Y}) t.prog.initial.push_back({Act::Submit, x});
  t.books = 4;
  t.ninstr = static_cast<int>(t.prog.templates.size());
  return t;
}

mm::MProg random_prog(std::mt19937_64& g, int& nregs) {
  std::uniform_int_distribution<int> len(1, 12), nr(1, 4), pct(0, 99);
  int n = len(g);
  nregs = nr(g);
  std::uniform_int_distribution<int> tgt(0, n - 1), reg(0, nregs - 1);
  mm::MProg p(n);
  for (auto& in : p) {
    int x = pct(g);
    in.op = x < 10 ? mm::Instr::HALT : x < 50 ? mm::Instr::INC : mm::Instr::DEC;
    in.r = reg(g);
    in.k = tgt(g);
    in.l = tgt(g);
  }
  return p;
}

struct CorpusResult {
  int programs = 0, diverged = 0;
  std::vector<long> at;
  rt::Stats cov;
  std::string summary() const {
    if (!diverged) return "never (" + std::to_string(programs) + " programs)";
    std::vector<long> v = at;
    std::sort(v.begin(), v.end());
    char buf[128];
    std::snprintf(buf, sizeof buf, "%d of %d programs, median %ld instr (min %ld)", diverged, programs, v[v.size() / 2], v.front());
    return buf;
  }
};

CorpusResult corpus(const mm::CompileOpts& co, uint64_t mutation, int N, uint64_t seed, uint64_t max_steps) {
  std::mt19937_64 g(seed);
  CorpusResult out;
  for (int i = 0; i < N; ++i) {
    int nr;
    mm::MProg p = random_prog(g, nr);
    mm::Regs regs(nr);
    for (auto& x : regs) x = std::uniform_int_distribution<int>(0, 8)(g);
    // only programs that do something
    mm::Run ref = mm::run_reference(p, regs, max_steps);
    if (ref.distinct_pcs < 3 || ref.dec_nonzero < 5 || ref.dec_zero < 1) continue;
    mm::Compiled c = mm::compile(p, nr, co);
    tbl::Table t;
    t.prog = c.prog;
    t.entry_instr = c.entry_instr;
    t.books = c.nbooks();
    t.ninstr = static_cast<int>(p.size());
    std::vector<proto::NewOrderMsg> pre;
    for (int r = 0; r < nr; ++r)
      if (regs[r] > 0) {
        proto::NewOrderMsg m;
        m.account = static_cast<uint8_t>(c.geo[r].maker);
        m.book = static_cast<uint32_t>(c.geo[r].book);
        m.side = static_cast<uint8_t>(c.geo[r].side);
        m.price = c.geo[r].reg;
        m.qty = regs[r];
        m.tag = 0xffffffffu;
        pre.push_back(m);
      }
    Trace ok = run_table(t, 0, pre, max_steps);
    Trace bad = run_table(t, mutation, pre, max_steps);
    ++out.programs;
    long d = divergence(ok, bad);
    if (d >= 0) {
      ++out.diverged;
      out.at.push_back(d);
    }
    out.cov.partial_fills += ok.st.partial_fills;
    out.cov.ioc_remainders += ok.st.ioc_remainders;
    out.cov.stops_sent += ok.st.stops_sent;
  }
  return out;
}

struct SnakeResult {
  long frames = -1, instrs = -1;
  rt::Stats cov;
};

SnakeResult snake_run(uint64_t mutation, int frames, uint64_t seed) {
  snake::Options o;
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
  ex::InProcChannel ch(cfg_for(t.books));
  ch.matcher().core().set_mutation(mutation);
  snake::Sim sim(o);
  snake::Player pl(seed);
  rt::Router r(t, ch);
  SnakeResult out;
  int frame = 0;
  r.on_marker = [&](int mk) {
    if (mk != 0 || out.frames >= 0) return;
    mm::Regs regs(p.lay.nregs);
    for (int i = 0; i < p.lay.nregs; ++i)
      regs[i] = ch.matcher().core().resting_qty(static_cast<uint32_t>(c.geo[i].book), static_cast<uint8_t>(c.geo[i].side), c.geo[i].reg);
    if (!sim.diff(regs, p.lay).empty()) {
      out.frames = frame;
      out.instrs = static_cast<long>(r.stats().instructions);
      return;
    }
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
        ch.send_as(2, m);  // a second session, like the visualiser's keyboard
      }
    sim.step(keys);
    ++frame;
  };
  r.on_instr = [&](int, uint64_t) { return frame <= frames && out.frames < 0; };
  rt::Options ro;
  r.run(ro);
  out.cov = r.stats();
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  int N = 300, frames = 300;
  uint64_t seed = 4242;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--programs" && i + 1 < argc) N = std::atoi(argv[++i]);
    else if (a == "--snake-frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (a == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
  }
  mm::CompileOpts s1;
  s1.persistent_backstop = s1.inc_clock = s1.two_accounts = true;
  mm::CompileOpts s2;
  s2.stop_branch = true;
  mm::CompileOpts pg;  // the paper's gadgets with cancel: register loads are multi-lot orders
  pg.gc_backstop = true;
  pg.two_accounts = true;

  std::printf("canary: instructions until a program run on the mutated engine parts from the same run on the correct one\n\n");
  tbl::Table probe = probe_table();
  Trace probe_ok = run_table(probe, 0, {}, 1000);
  std::printf("coverage of each program on the correct engine:\n");
  std::printf("  probe   %zu instructions; %s\n", probe_ok.instrs.size(), coverage(probe_ok.st).c_str());
  SnakeResult sn_ok = snake_run(0, frames, seed);
  std::printf("  snake   %d frames, %llu instructions; %s\n", frames, (unsigned long long)sn_ok.cov.instructions,
              coverage(sn_ok.cov).c_str());
  const uint64_t muts[] = {fb::FifoAfterPartial, fb::PriceInverted, fb::StopWrongSide, fb::IocRests};
  struct Row {
    std::string name, probe, snake, c1, c2, c3;
  };
  std::vector<Row> rows;
  for (uint64_t m : muts) {
    Row row;
    row.name = fb::mutation_name(m);
    Trace pb = run_table(probe, m, {}, 1000);
    long d = divergence(probe_ok, pb);
    row.probe = d < 0 ? "never" : std::to_string(d);
    SnakeResult sn = snake_run(m, frames, seed);
    row.snake = sn.frames < 0 ? "never (" + std::to_string(frames) + " frames)"
                              : "frame " + std::to_string(sn.frames) + ", instr " + std::to_string(sn.instrs);
    CorpusResult r1 = corpus(s1, m, N, seed, 2000);
    CorpusResult r2 = corpus(s2, m, N, seed, 2000);
    CorpusResult r3 = corpus(pg, m, N, seed, 2000);
    row.c1 = r1.summary();
    row.c2 = r2.summary();
    row.c3 = r3.summary();
    if (m == muts[0]) {
      std::printf("  S1 corpus  %s\n", coverage(r1.cov).c_str());
      std::printf("  S2 corpus  %s\n", coverage(r2.cov).c_str());
      std::printf("  paper+cancel corpus  %s\n\n", coverage(r3.cov).c_str());
    }
    rows.push_back(row);
  }
  std::printf("| mutation | probe | Snake | S1 corpus | S2 corpus | paper+cancel corpus |\n|---|---|---|---|---|---|\n");
  for (const auto& r : rows)
    std::printf("| %s | %s | %s | %s | %s | %s |\n", r.name.c_str(), r.probe.c_str(), r.snake.c_str(), r.c1.c_str(),
                r.c2.c_str(), r.c3.c_str());
  return 0;
}
