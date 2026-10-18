// Lock-step equivalence of the wire (exchange process + router process over a transport) with
// stage 1's in-process engine, on random programs compiled four ways:
//   S1   persistent backstop, clock INC, two accounts: fill-price links only
//   S2   stop-branch: the exchange's stops do the branching; the router cancels the loser
//   P    the paper's Table 2 as written, with accept links and one account (STP off)
//   PG   the paper's gadgets with the backstop cancelled on the non-zero branch (Cancel path)
// For each program: same halt status, same instruction count, same registers read back from
// the books, same trade count, same number of resting orders in every book.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "minsky.hpp"
#include "router.hpp"

using namespace mm;

namespace {
MProg random_prog(std::mt19937_64& g, int& nregs) {
  std::uniform_int_distribution<int> len(1, 12), nr(1, 4), pct(0, 99);
  int n = len(g);
  nregs = nr(g);
  std::uniform_int_distribution<int> tgt(0, n - 1), reg(0, nregs - 1);
  MProg p(n);
  for (auto& in : p) {
    int x = pct(g);
    in.op = x < 10 ? Instr::HALT : x < 50 ? Instr::INC : Instr::DEC;
    in.r = reg(g);
    in.k = tgt(g);
    in.l = tgt(g);
  }
  return p;
}
Regs random_regs(std::mt19937_64& g, int nregs) {
  std::uniform_int_distribution<int> v(0, 8);
  Regs r(nregs);
  for (auto& x : r) x = v(g);
  return r;
}
struct Variant {
  const char* name;
  CompileOpts o;
  ob::STP stp;
};
}  // namespace

int main(int argc, char** argv) {
  std::string url = "tcp://127.0.0.1:7000";
  int N = 2000;
  uint64_t seed = 12345, max_steps = 2000;
  bool filtered = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--connect" && i + 1 < argc) url = argv[++i];
    else if (a == "--n" && i + 1 < argc) N = std::atoi(argv[++i]);
    else if (a == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
    else if (a == "--steps" && i + 1 < argc) max_steps = std::stoull(argv[++i]);
    else if (a == "--filtered") filtered = true;
  }
  CompileOpts s1;
  s1.persistent_backstop = s1.inc_clock = s1.two_accounts = true;
  CompileOpts s2;
  s2.stop_branch = true;
  CompileOpts paper;  // accept links, one account
  CompileOpts pg = paper;
  pg.gc_backstop = true;
  Variant variants[] = {{"S1", s1, ob::STP::ExpireMaker},
                        {"S2", s2, ob::STP::ExpireMaker},
                        {"P", paper, ob::STP::None},
                        {"PG", pg, ob::STP::None}};
  std::unique_ptr<net::Channel> ch;
  try {
    ch = net::connect(url);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  std::printf("wire equivalence over %s: %d random programs x 4 variants, up to %llu steps each\n", ch->kind(), N,
              (unsigned long long)max_steps);
  int total_mism = 0;
  uint64_t total_instr = 0, total_waits = 0, total_ns = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (const Variant& v : variants) {
    std::mt19937_64 g(seed);
    int mism = 0, ran = 0, halted = 0;
    uint64_t instr = 0, waits = 0, ns = 0, trades = 0;
    rt::control(*ch, proto::SetSTP, static_cast<int64_t>(v.stp));
    for (int i = 0; i < N; ++i) {
      int nr;
      MProg p = random_prog(g, nr);
      Regs regs = random_regs(g, nr);
      if (filtered) {
        Run ref = run_reference(p, regs, max_steps);
        if (ref.distinct_pcs < 4 || ref.dec_nonzero < 20 || ref.dec_zero < 3) continue;
      }
      Compiled c = compile(p, nr, v.o);
      BookRun ref = run_on_book(c, regs, max_steps, false, v.stp);
      tbl::Table t;
      t.prog = c.prog;
      t.entry_instr = c.entry_instr;
      t.books = c.nbooks();
      t.ninstr = static_cast<int>(p.size());
      rt::control(*ch, proto::ResetBooks);
      for (int r = 0; r < nr; ++r)
        if (regs[r] > 0) {
          const Geo& gg = c.geo[r];
          proto::NewOrderMsg m;
          m.account = static_cast<uint8_t>(gg.maker);
          m.book = static_cast<uint32_t>(gg.book);
          m.side = static_cast<uint8_t>(gg.side);
          m.price = gg.reg;
          m.qty = regs[r];
          m.tag = 0xffffffffu;
          rt::submit_sync(*ch, m);
        }
      rt::Router router(t, *ch);
      rt::Options o;
      o.max_instr = max_steps;
      o.max_ns_wait = 5'000'000'000ull;
      rt::Status st = router.run(o);
      const auto& s = router.stats();
      ++ran;
      instr += s.instructions;
      waits += s.waits;
      ns += s.ns_total;
      halted += st == rt::Status::Halted;
      std::string why;
      auto want = ref.run.status == Run::Halted ? rt::Status::Halted
                  : ref.run.status == Run::StepLimit ? rt::Status::StepLimit
                                                      : rt::Status::Idle;
      if (st != want) why += " status " + std::string(rt::status_name(st)) + " vs " + status_name(ref.run.status);
      if (s.instructions != ref.run.steps)
        why += " steps " + std::to_string(s.instructions) + " vs " + std::to_string(ref.run.steps);
      for (int r = 0; r < nr; ++r) {
        const Geo& gg = c.geo[r];
        int64_t q = rt::book_qty(*ch, static_cast<uint32_t>(gg.book), static_cast<uint8_t>(gg.side), gg.reg);
        if (q != ref.run.regs[r]) why += " r" + std::to_string(r) + "=" + std::to_string(q) + " vs " + std::to_string(ref.run.regs[r]);
      }
      auto stats = rt::control(*ch, proto::GetStats);
      trades += static_cast<uint64_t>(stats.arg2);
      if (static_cast<uint64_t>(stats.arg2) != ref.trades && st == want && want != rt::Status::StepLimit)
        why += " trades " + std::to_string(stats.arg2) + " vs " + std::to_string(ref.trades);
      size_t resting = 0;
      for (int b = 0; b < c.nbooks(); ++b) resting += static_cast<size_t>(rt::control(*ch, proto::BookInfo, b).arg);
      if (resting != ref.resting_orders && st == want && want != rt::Status::StepLimit)
        why += " resting " + std::to_string(resting) + " vs " + std::to_string(ref.resting_orders);
      if (!why.empty()) {
        ++mism;
        if (mism <= 5) std::printf("  %s program %d (%d instr, %d regs):%s\n", v.name, i, (int)p.size(), nr, why.c_str());
      }
    }
    std::printf("  %-3s %5d programs, %4d halted, %9llu instructions, %llu waits (%.2f/instr), %llu trades, "
                "%.1f us/instr over the wire, %d mismatches\n",
                v.name, ran, halted, (unsigned long long)instr, (unsigned long long)waits,
                instr ? double(waits) / instr : 0.0, (unsigned long long)trades, instr ? ns / 1e3 / instr : 0.0, mism);
    total_mism += mism;
    total_instr += instr;
    total_waits += waits;
    total_ns += ns;
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("total: %llu instructions, %llu waits, %d mismatches [%.1fs]\n", (unsigned long long)total_instr,
              (unsigned long long)total_waits, total_mism, secs);
  (void)total_ns;
  return total_mism ? 1 : 0;
}
