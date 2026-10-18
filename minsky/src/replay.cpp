// replay: run a verifier path on the live exchange (CONTRACT.md §5).
#include <unistd.h>

#include <cstdio>
#include <string>

#include "env.hpp"
#include "json.hpp"
#include "program_json.hpp"
#include "replayer.hpp"
#include "verifier.hpp"

namespace {
void usage(FILE* to) {
  std::fputs(
      "replay --program compiled.json --path path.json --exchange tcp://127.0.0.1:7000\n"
      "       [--envelope envelope.json | --fees maker=1.0,taker=3.5] [--instruments instruments.json]\n"
      "       [--attach SUB [--journal-base N]] [--book NAME=B,...] [--keep]\n"
      "\n"
      "Runs a path (a Rejection's counterexample or a Certificate's worst_path, or the bare array)\n"
      "on the live exchange with the same messages the verifier used, and measures the program from\n"
      "the exchange's own fill journal. CONTRACT.md §5:\n"
      "  stdout   {contract_version, loss_cents, fees_cents, trades, matches_verifier, ...}\n"
      "           matches_verifier: loss_cents equals the path's final {\"event\": \"mark\"} value\n"
      "Scratch mode (default): the program is deployed in sub-account 253 on a never-used book\n"
      "(>= 2048), after one print at each mark; afterwards its orders are cancelled and its position\n"
      "closed (unless --keep).\n"
      "--attach SUB: the program is already deployed (by venued) in sub-account SUB on the\n"
      "instruments' live books; only the environment's moves are played (key-holder moves belong to\n"
      "venued and are skipped and counted). --journal-base: SUB's first journal index for it.\n"
      "The exchange must run with --accounts 255: accounts 0 and 254 are the environment.\n",
      to);
}
}  // namespace

int main(int argc, char** argv) {
  std::string program, path, url = "tcp://127.0.0.1:7000", envelope, fees, instruments, books;
  int attach = -1;
  int64_t journal_base = -1;
  bool keep = false;
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
    else if (a == "--path") path = next();
    else if (a == "--exchange") url = next();
    else if (a == "--envelope") envelope = next();
    else if (a == "--fees") fees = next();
    else if (a == "--instruments") instruments = next();
    else if (a == "--attach") attach = std::stoi(next());
    else if (a == "--journal-base") journal_base = std::stoll(next());
    else if (a == "--book") books = next();
    else if (a == "--keep") keep = true;
    else if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    } else {
      usage(stderr);
      return 1;
    }
  }
  if (program.empty() || path.empty()) {
    usage(stderr);
    return 1;
  }
  try {
    const bk::Instruments ins = bk::find_instruments(instruments);
    const bk::Program p = bk::load_program(bk::read_json(program), ins);
    const json::Value pv = bk::read_json(path);
    const json::Value& arr = pv.is_array() ? pv : pv.has("counterexample") ? pv.at("counterexample") : pv.at("worst_path");
    const std::vector<bk::PathMove> moves = bk::path_moves(arr, p);
    const int64_t expected = bk::path_final_loss(arr);

    bk::ReplayOptions opt;
    if (!envelope.empty()) {
      const bk::Envelope e = bk::load_envelope(bk::read_json(envelope));
      opt.fees = e.fees;
      opt.mark_mode = e.trigger_ref == "mark";
    }
    if (!fees.empty()) {
      const size_t c = fees.find(',');
      auto val = [&](const std::string& kv, const char* k) {
        if (kv.rfind(k, 0) != 0) throw bk::Invalid("--fees maker=X,taker=Y");
        return bk::bps100(std::stod(kv.substr(std::string(k).size())));
      };
      opt.fees.maker_bps100 = val(fees.substr(0, c), "maker=");
      opt.fees.taker_bps100 = val(fees.substr(c + 1), "taker=");
    }
    bk::WirePort port(net::connect(url));
    if (attach >= 0) {
      if (attach < 1 || attach > bk::kMaxProgramAccount) throw bk::Invalid("--attach takes a program sub-account 1..252");
      opt.account = static_cast<uint8_t>(attach);
      opt.deploy = false;
      opt.journal_base = journal_base < 0 ? 0 : journal_base;
      for (const auto& name : p.instruments) opt.books.push_back(ins.at(name).book);
    } else {
      opt.oco_base = bk::kReplayOcoBase + ((static_cast<uint32_t>(getpid()) & 0x3fff) << 8);
      uint32_t from = 2048;
      for (size_t i = 0; i < p.instruments.size(); ++i) {
        opt.books.push_back(bk::fresh_book(port, opt.account, from, 4096));
        from = opt.books.back() + 1;
      }
    }
    if (!books.empty()) {  // explicit NAME=B overrides
      size_t st = 0;
      while (st < books.size()) {
        size_t c = books.find(',', st);
        if (c == std::string::npos) c = books.size();
        const std::string kv = books.substr(st, c - st);
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) throw bk::Invalid("--book NAME=B,...");
        for (size_t i = 0; i < p.instruments.size(); ++i)
          if (p.instruments[i] == kv.substr(0, eq)) opt.books[i] = static_cast<uint32_t>(std::stoul(kv.substr(eq + 1)));
        st = c + 1;
      }
    }
    bk::Replayer r(port, p, ins, opt);
    const int skipped = r.run(moves);
    const bk::Ledger led = r.ledger();
    std::vector<int64_t> ticks;
    for (const auto& name : p.instruments) ticks.push_back(ins.at(name).tick_cents);
    const int64_t loss = led.loss(r.marks(), ticks);
    json::Value bk_books = json::Value::object();
    for (size_t i = 0; i < p.instruments.size(); ++i) bk_books.set(p.instruments[i], static_cast<int64_t>(opt.books[i]));
    json::Value out = json::obj({{"contract_version", bk::kContractVersion},
                                 {"loss_cents", loss},
                                 {"fees_cents", led.fees_cents},
                                 {"trades", led.trades},
                                 {"matches_verifier", loss == expected},
                                 {"expected_loss_cents", expected},
                                 {"position", led.pos[0]},
                                 {"sub_account", static_cast<int64_t>(opt.account)},
                                 {"books", bk_books}});
    if (attach >= 0) out.set("skipped_keyholder_moves", skipped);
    if (attach < 0 && !keep) r.cleanup();
    std::fputs(json::pretty(out).c_str(), stdout);
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "replay: %s\n", ex.what());
    return 1;
  }
}
