// verify: the bounded key's proof search (CONTRACT.md §4).
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "json.hpp"
#include "program_json.hpp"
#include "verifier.hpp"

namespace {
void usage(FILE* to) {
  std::fputs(
      "verify --program compiled.json --envelope envelope.json --mark P [--timeout-ms N]\n"
      "       [--instruments instruments.json] [--first]\n"
      "\n"
      "Runs every future of a CompiledProgram (CONTRACT.md §2.2) on an exact copy of the exchange's\n"
      "matching engine, against the envelope's limits and its model of everyone else\n"
      "(BOUNDED_KEYS.md §5.1, §8). CONTRACT.md §4:\n"
      "  stdout   a Certificate (BOUNDED_KEYS.md §9.1, without program_id/account/budget/key)\n"
      "           or a Rejection (§9.2, without program_id)\n"
      "  exit     0 certified, 3 rejected, 4 timeout ({\"status\": \"timeout\"}), 1 internal error\n"
      "  --mark   the last print when submitted (one value, or NAME=P,NAME=P per instrument)\n"
      "  --first  report the first violating path the search meets, not the shortest\n"
      "Deterministic: the move order is fixed; only explored.ms differs between runs.\n"
      "instruments.json is found as policyc does ($BK_INSTRUMENTS, ./, ./fixtures/, ../fixtures/).\n",
      to);
}
}  // namespace

int main(int argc, char** argv) {
  std::string program, envelope, instruments, mark;
  bk::VerifyOptions opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage(stderr);
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "--program") program = next();
    else if (a == "--envelope") envelope = next();
    else if (a == "--mark") mark = next();
    else if (a == "--timeout-ms") opt.timeout_ms = std::stoll(next());
    else if (a == "--instruments") instruments = next();
    else if (a == "--first") opt.shortest = false;
    else if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    } else {
      usage(stderr);
      return 1;
    }
  }
  if (program.empty() || envelope.empty() || mark.empty()) {
    usage(stderr);
    return 1;
  }
  try {
    const bk::Instruments ins = bk::find_instruments(instruments);
    const bk::Program p = bk::load_program(bk::read_json(program), ins);
    const bk::Envelope e = bk::load_envelope(bk::read_json(envelope));
    if (mark.find('=') == std::string::npos) {
      opt.marks.push_back(std::stoll(mark));
    } else {
      opt.marks.assign(p.instruments.size(), 0);
      std::vector<bool> got(p.instruments.size(), false);
      size_t st = 0;
      while (st <= mark.size()) {
        size_t c = mark.find(',', st);
        if (c == std::string::npos) c = mark.size();
        const std::string kv = mark.substr(st, c - st);
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) throw bk::Invalid("--mark NAME=P,NAME=P");
        const std::string name = kv.substr(0, eq);
        bool found = false;
        for (size_t i = 0; i < p.instruments.size(); ++i)
          if (p.instruments[i] == name) {
            opt.marks[i] = std::stoll(kv.substr(eq + 1));
            got[i] = found = true;
          }
        if (!found) throw bk::Invalid("--mark names an instrument the program does not trade: " + name);
        st = c + 1;
      }
      for (size_t i = 0; i < got.size(); ++i)
        if (!got[i]) throw bk::Invalid("--mark is missing " + p.instruments[i]);
    }
    const bk::VerifyResult r = bk::verify(p, e, ins, opt);
    std::fputs(json::pretty(r.out).c_str(), stdout);
    return r.exit_code;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "verify: %s\n", ex.what());
    return 1;
  }
}
