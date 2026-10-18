// Run a router table against a live exchange.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "router.hpp"

static void usage() {
  std::puts(
      "router --table FILE [--connect URL] [--max-instr N] [--max-fps F] [--reset] [--delay-us N]\n"
      "       [--load BOOK:SIDE:PRICE:QTY:ACCOUNT ...] [--read BOOK:SIDE:PRICE ...] [--quiet]\n"
      "  URL: tcp://127.0.0.1:7000 (default) | udp://127.0.0.1:7001 | shm:///tmp/minsky.shm\n"
      "  --reset clears the exchange first; --load rests orders before the run (registers);\n"
      "  --read prints resting quantity at (book, side, price) after the run.");
}

int main(int argc, char** argv) {
  std::string table, url = "tcp://127.0.0.1:7000";
  rt::Options opt;
  bool reset = false, quiet = false;
  int64_t delay_us = -1;
  std::vector<std::string> loads, reads;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return std::string(argv[++i]);
    };
    if (a == "--table") table = next();
    else if (a == "--connect") url = next();
    else if (a == "--max-instr") opt.max_instr = std::stoull(next());
    else if (a == "--max-fps") opt.max_fps = std::stod(next());
    else if (a == "--delay-us") delay_us = std::stoll(next());
    else if (a == "--reset") reset = true;
    else if (a == "--quiet") quiet = true;
    else if (a == "--load") loads.push_back(next());
    else if (a == "--read") reads.push_back(next());
    else {
      usage();
      return 2;
    }
  }
  if (table.empty()) {
    usage();
    return 2;
  }
  try {
    tbl::Table t = tbl::load(table);
    auto ch = net::connect(url);
    if (reset) rt::control(*ch, proto::ResetBooks);
    if (delay_us >= 0) rt::control(*ch, proto::SetDelayUs, delay_us);
    for (const auto& s : loads) {
      long long b, side, px, qty, acct;
      if (std::sscanf(s.c_str(), "%lld:%lld:%lld:%lld:%lld", &b, &side, &px, &qty, &acct) != 5) {
        std::fprintf(stderr, "bad --load %s\n", s.c_str());
        return 2;
      }
      proto::NewOrderMsg m;
      m.book = static_cast<uint32_t>(b);
      m.side = static_cast<uint8_t>(side);
      m.price = px;
      m.qty = qty;
      m.account = static_cast<uint8_t>(acct);
      m.tag = 0xffffffffu;
      rt::submit_sync(*ch, m);
    }
    if (!quiet)
      std::printf("router: %s: %zu templates, %zu links, %d instructions, %d books, %zu markers; over %s\n",
                  t.name.c_str(), t.prog.templates.size(), t.prog.links.size(), t.ninstr, t.books, t.markers.size(),
                  ch->kind());
    rt::Router r(t, *ch);
    rt::Status st = r.run(opt);
    const auto& s = r.stats();
    const double secs = s.ns_total / 1e9;
    if (!quiet || st != rt::Status::Halted) {
      std::printf("router: %s (halt code %d)\n", rt::status_name(st), s.halt_code);
      std::printf("  %llu instructions, %llu orders, %llu cancels, %llu fills, %llu waits, %llu frames in %.3f s\n",
                  (unsigned long long)s.instructions, (unsigned long long)s.orders_sent,
                  (unsigned long long)s.cancels_sent, (unsigned long long)s.fills, (unsigned long long)s.waits,
                  (unsigned long long)s.frames, secs);
      if (secs > 0)
        std::printf("  %.0f instructions/s, %.2f waits/instruction, %.1f us per wait, %.1f fps, waiting %.1f%% of the time\n",
                    s.instructions / secs, s.instructions ? double(s.waits) / s.instructions : 0,
                    s.waits ? s.ns_waiting / 1e3 / s.waits : 0, s.frames / secs, 100.0 * s.ns_waiting / s.ns_total);
    }
    for (const auto& x : reads) {
      long long b, side, px;
      if (std::sscanf(x.c_str(), "%lld:%lld:%lld", &b, &side, &px) != 3) continue;
      std::printf("  book %lld %s @%lld: %lld\n", b, side ? "ask" : "bid", px,
                  (long long)rt::book_qty(*ch, static_cast<uint32_t>(b), static_cast<uint8_t>(side), px));
    }
    return st == rt::Status::Halted || st == rt::Status::StepLimit ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "router: %s\n", e.what());
    return 1;
  }
}
