// policyc: MPL -> CompiledProgram (CONTRACT.md §3).
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>

#include "json.hpp"
#include "policy.hpp"
#include "program_json.hpp"

namespace {
void usage(FILE* to) {
  std::fputs(
      "policyc --backend minsky --instruments instruments.json --env env.json < source.mpl > out.json\n"
      "\n"
      "Compiles an MPL policy (BOUNDED_KEYS.md §6) into a CompiledProgram (CONTRACT.md §2.2): every\n"
      "node a native order on the exchange (§7), a deploy plan (parents before children, one\n"
      "generation per round trip) and a report. CONTRACT.md §3:\n"
      "  stdin    MPL source\n"
      "  stdout   a CompiledProgram, or a StaticError {status: static_error, line, col, message}\n"
      "  --env    the envelope's env object (or a whole envelope); used for the\n"
      "           allow < max_gap_ticks warning\n"
      "  exit     0 compiled, 2 static error, 1 internal error (stderr has details)\n"
      "Deterministic: the same input gives byte-identical output. source_hash is the sha256 of stdin.\n"
      "--unsafe-allow-zero lets `allow 0` through (tests only: shows the verifier catches it too).\n",
      to);
}
}  // namespace

int main(int argc, char** argv) {
  bk::CompileOptions opt;
  std::string instruments, env;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage(stderr);
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "--backend") opt.backend = next();
    else if (a == "--instruments") instruments = next();
    else if (a == "--env") env = next();
    else if (a == "--unsafe-allow-zero") opt.unsafe_allow_zero = true;
    else if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    } else {
      usage(stderr);
      return 1;
    }
  }
  try {
    const bk::Instruments ins = bk::find_instruments(instruments);
    if (!env.empty()) {
      const json::Value e = bk::read_json(env);
      const json::Value& ev = e.has("env") ? e.at("env") : e;
      if (const json::Value* g = ev.get("max_gap_ticks")) opt.max_gap_ticks = g->as_int();
    }
    const std::string src((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    try {
      const bk::Program p = bk::compile(src, ins, opt);
      std::fputs(json::pretty(bk::to_json(p), 200).c_str(), stdout);
      return 0;
    } catch (const bk::StaticError& e) {
      std::fputs(json::pretty(e.to_json()).c_str(), stdout);
      return 2;
    }
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "policyc: %s\n", ex.what());
    return 1;
  }
}
