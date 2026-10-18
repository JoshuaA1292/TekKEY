// Compile a program to a router table (and a visualiser layout).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "programs.hpp"
#include "snake.hpp"
#include "table.hpp"

static void usage() {
  std::puts(
      "compile snake   --out FILE.tbl [--layout FILE] [--size WxH] [--wrap] [--growth N] [--pad N] [--seed N]\n"
      "compile rule110 --out FILE.tbl [--layout FILE] [--cells N] [--gens N (0 = loop forever)]\n"
      "compile table3|add|absdiff|mul|divmod --out FILE.tbl\n"
      "  all programs use the S1 gadgets: persistent backstop, clock INC, two accounts (0 sells, 1 buys)");
}

static tbl::Table to_table(const mm::Compiled& c, const std::string& name, int ninstr, std::vector<int> markers) {
  tbl::Table t;
  t.name = name;
  t.prog = c.prog;
  t.entry_instr = c.entry_instr;
  t.books = c.nbooks();
  t.ninstr = ninstr;
  t.markers = std::move(markers);
  return t;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string what = argv[1], out, layout;
  snake::Options so;
  int cells = 96, gens = 0;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return std::string(argv[++i]);
    };
    if (a == "--out") out = next();
    else if (a == "--layout") layout = next();
    else if (a == "--size") {
      std::string s = next();
      if (std::sscanf(s.c_str(), "%dx%d", &so.width, &so.height) != 2) {
        usage();
        return 2;
      }
    } else if (a == "--wrap") so.wrap = true;
    else if (a == "--growth") so.growth = std::atoi(next().c_str());
    else if (a == "--pad") so.pad = std::atoi(next().c_str());
    else if (a == "--seed") so.seed = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--cells") cells = std::atoi(next().c_str());
    else if (a == "--gens") gens = std::atoi(next().c_str());
    else {
      usage();
      return 2;
    }
  }
  if (out.empty()) {
    usage();
    return 2;
  }
  mm::CompileOpts co;
  co.persistent_backstop = co.inc_clock = co.two_accounts = true;
  try {
    tbl::Table t;
    tbl::Layout lay;
    if (what == "snake") {
      snake::Program p = snake::compile(so);
      mm::Compiled c = mm::compile(p.prog, p.lay.nregs, co);
      t = to_table(c, "snake", static_cast<int>(p.prog.size()), {p.frame_instr, p.die_instr, p.init_instr});
      lay = snake::layout_file(p, c.clock_book, static_cast<int>(co.P), 2);
      std::printf("snake %dx%d%s growth %d pad %d: %zu Minsky instructions, %zu templates, %zu links, %d books\n",
                  so.width, so.height, so.wrap ? " wrap" : "", so.growth, so.pad, p.prog.size(),
                  c.prog.templates.size(), c.prog.links.size(), c.nbooks());
    } else if (what == "rule110") {
      int marker = 0;
      mm::MProg p = programs::rule110(cells, gens, marker);
      mm::Compiled c = mm::compile(p, 2 * cells, co);
      t = to_table(c, "rule110", static_cast<int>(p.size()), {marker});
      lay.v["P"] = {co.P};
      std::vector<long long> cs;
      for (int i = 0; i < cells; ++i) cs.push_back(i);
      lay.v["cells"] = cs;
      lay.v["width"] = {cells};
      lay.v["clock"] = {c.clock_book};
      lay.v["nbooks"] = {c.nbooks()};
      std::printf("rule110 on %d cells, %s: %zu Minsky instructions, %zu templates, %d books\n", cells,
                  gens ? (std::to_string(gens) + " generations").c_str() : "looping forever", p.size(),
                  c.prog.templates.size(), c.nbooks());
    } else {
      mm::MProg p;
      int nregs = 0;
      if (what == "table3") p = mm::paper_table3(), nregs = 2;
      else if (what == "add") p = mm::prog_add(), nregs = 3;
      else if (what == "absdiff") p = mm::prog_absdiff(), nregs = 3;
      else if (what == "mul") p = mm::prog_mul(), nregs = 4;
      else if (what == "divmod") p = mm::prog_divmod(), nregs = 5;
      else {
        usage();
        return 2;
      }
      mm::Compiled c = mm::compile(p, nregs, co);
      t = to_table(c, what, static_cast<int>(p.size()), {});
      lay.v["P"] = {co.P};
      std::vector<long long> regs;
      for (int r = 0; r < nregs; ++r) regs.push_back(c.geo[r].book);
      lay.v["registers"] = regs;
      lay.v["nbooks"] = {c.nbooks()};
      std::printf("%s: %zu Minsky instructions, %zu templates, %d books; registers are sell quantity at %lld in books 0..%d\n",
                  what.c_str(), p.size(), c.prog.templates.size(), c.nbooks(), (long long)co.P, nregs - 1);
    }
    tbl::save(t, out);
    if (!layout.empty()) tbl::save(lay, layout);
    std::printf("wrote %s%s%s\n", out.c_str(), layout.empty() ? "" : " and ", layout.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "compile: %s\n", e.what());
    return 1;
  }
  return 0;
}
