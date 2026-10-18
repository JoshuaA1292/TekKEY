// Rule 110 (looping) against a direct simulation; table and layout files round-trip.
#include <cstdio>
#include <fstream>
#include <string>

#include "bitasm.hpp"
#include "programs.hpp"
#include "snake.hpp"
#include "table.hpp"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}
}  // namespace

int main() {
  // Rule 110 loop: 48 cells, run 40 generations in the interpreter, compare every row.
  for (int n : {8, 48, 96}) {
    int marker = -1;
    mm::MProg p = programs::rule110(n, 0, marker);
    check(marker == 0, "generation marker is the entry instruction");
    mm::Regs regs(2 * n, 0);
    regs[n - 1] = 1;
    std::vector<int> expect(n, 0);
    expect[n - 1] = 1;
    int gens = 0, wrong = 0;
    auto hook = [&](int pc, uint64_t) {
      if (pc != marker) return true;
      std::vector<int> row(n);
      for (int i = 0; i < n; ++i) row[i] = static_cast<int>(regs[i]);
      wrong += row != expect;
      for (int i = n; i < 2 * n; ++i) wrong += regs[i] != 0;
      expect = programs::rule110_step(expect);
      return ++gens <= 40;
    };
    bits::HookRun r = bits::run_with_hook(p, regs, UINT64_MAX, hook);
    check(r.status == bits::HookRun::Stopped && gens == 41, "rule110 loop ran 40 generations on " + std::to_string(n) + " cells");
    check(wrong == 0, "rule110 rows all match on " + std::to_string(n) + " cells");
    std::printf("  rule110 %d cells: %zu-instruction program, %llu instructions for 40 generations (%.0f/gen), %d wrong rows\n", n,
                p.size(), (unsigned long long)r.steps, double(r.steps) / 40, wrong);
  }
  // finite version halts
  {
    int marker;
    mm::MProg p = programs::rule110(16, 5, marker);
    mm::Regs regs(32, 0);
    regs[15] = 1;
    mm::Run r = mm::run_reference(p, regs, 1'000'000);
    check(r.status == mm::Run::Halted, "finite rule110 halts");
  }
  // table round trip
  {
    snake::Options o;
    snake::Program sp = snake::compile(o);
    mm::CompileOpts co;
    co.persistent_backstop = co.inc_clock = co.two_accounts = true;
    mm::Compiled c = mm::compile(sp.prog, sp.lay.nregs, co);
    tbl::Table t;
    t.name = "snake";
    t.prog = c.prog;
    t.entry_instr = c.entry_instr;
    t.books = c.nbooks();
    t.ninstr = static_cast<int>(sp.prog.size());
    t.markers = {sp.frame_instr, sp.die_instr};
    const std::string path = "/tmp/minsky-test.tbl";
    tbl::save(t, path);
    tbl::Table u = tbl::load(path);
    bool same = u.name == t.name && u.books == t.books && u.ninstr == t.ninstr && u.markers == t.markers &&
                u.entry_instr == t.entry_instr && u.prog.templates.size() == t.prog.templates.size() &&
                u.prog.links.size() == t.prog.links.size() && u.prog.initial.size() == t.prog.initial.size();
    for (size_t i = 0; same && i < t.prog.templates.size(); ++i) {
      const auto &a = t.prog.templates[i], &b = u.prog.templates[i];
      same = a.account == b.account && a.book == b.book && a.side == b.side && a.type == b.type && a.price == b.price &&
             a.trigger == b.trigger && a.qty == b.qty && a.reduce_only == b.reduce_only;
    }
    for (size_t i = 0; same && i < t.prog.links.size(); ++i) {
      const auto &a = t.prog.links[i], &b = u.prog.links[i];
      same = a.on == b.on && a.src == b.src && a.at == b.at && a.actions.size() == b.actions.size();
      for (size_t k = 0; same && k < a.actions.size(); ++k)
        same = a.actions[k].kind == b.actions[k].kind && a.actions[k].arg == b.actions[k].arg;
    }
    check(same, "table save/load round trip is exact");
    tbl::Layout l = snake::layout_file(sp, c.clock_book, 1000, 2);
    tbl::save(l, "/tmp/minsky-test.layout");
    tbl::Layout l2 = tbl::load_layout("/tmp/minsky-test.layout");
    check(l2.v == l.v, "layout save/load round trip is exact");
    check(l2.vec("pixels").size() == 256 && l2.get("P") == 1000 && l2.vec("keys").size() == 4, "layout has the snake keys");
    std::printf("  table: %zu templates, %zu links, %zu bytes on disk\n", t.prog.templates.size(), t.prog.links.size(),
                (size_t)std::ifstream(path, std::ios::binary | std::ios::ate).tellg());
  }
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
