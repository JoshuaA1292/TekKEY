// The latency benchmark: the clock speed of the exchange computer.
//
// A stateless router executes one program instruction per order round trip, so the venue's
// round-trip latency is the computer's clock period and instructions per second = 1 / round trip.
// This program measures that round trip on every transport the exchange speaks (tcp, udp, shm),
// for three shapes:
//   ping    a Control{Ping} round trip: the wire alone, no matching
//   decjz   one instruction-shaped round trip: a 1-lot IOC BUY into a book holding one large
//           resting SELL; the exchange answers Fill then Ack. What a DECJZ costs the router.
//   inc     three orders sent back to back (two resting SELLs, then an IOC that lifts the
//           second), read until the third Ack. What an INC costs.
// plus two in-process points with no wire at all (fb::Core and stage 1's ob::Engine), and a sweep
// of the exchange's latency knob (Control{SetDelayUs}) so the reader can see that the knob adds
// exactly what it says.
//
// Every sample is net::now_ns() around send + flush + recv-until-Ack. Nothing sleeps inside a
// timed region; every channel is busy-polled. Nothing is pinned.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine.hpp"
#include "fastbook.hpp"
#include "proto.hpp"
#include "router.hpp"
#include "transport.hpp"

namespace {

constexpr uint64_t kSampleTimeoutNs = 5'000'000'000ull;  // a reply slower than this is a dead venue
constexpr int64_t kBookPrice = 1001;                      // decjz book: one big SELL at this price
constexpr int64_t kIncPrice = 1000;

struct Stat {
  double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
  size_t n = 0;
  double instr_per_s() const { return p50 > 0 ? 1e9 / p50 : 0; }
};

// Nearest-rank percentiles over ns samples (sorts in place).
Stat summarize(std::vector<double>& s) {
  Stat st;
  st.n = s.size();
  if (s.empty()) return st;
  std::sort(s.begin(), s.end());
  auto q = [&](double p) {
    size_t i = static_cast<size_t>(std::ceil(p * static_cast<double>(s.size())));
    if (i == 0) i = 1;
    return s[std::min(i - 1, s.size() - 1)];
  };
  st.p50 = q(0.50);
  st.p90 = q(0.90);
  st.p99 = q(0.99);
  st.p999 = q(0.999);
  st.max = s.back();
  return st;
}

struct Row {
  std::string path, shape;
  Stat st;
};

// Spin for one record. False if the channel died or the deadline passed (checked every 1024
// spins so the clock read never dominates the poll).
bool recv_bounded(net::Channel& ch, proto::Any& a, uint64_t deadline_ns) {
  for (uint32_t spins = 0;; ++spins) {
    if (ch.try_recv(a)) return true;
    if (!ch.alive()) return false;
    if ((spins & 1023) == 0 && net::now_ns() > deadline_ns) return false;
  }
}

[[noreturn]] void die(const std::string& what) { throw std::runtime_error(what); }

// ---- shapes ----------------------------------------------------------------------------------

// Control{Ping} until its ControlReply. Also the reachability probe: the first one is bounded.
std::vector<double> run_ping(net::Channel& ch, uint64_t iters, uint64_t warmup) {
  std::vector<double> out;
  out.reserve(iters);
  proto::ControlMsg c;
  c.op = proto::Ping;
  proto::Any a;
  for (uint64_t i = 0; i < warmup + iters; ++i) {
    c.tag = static_cast<uint32_t>(i);
    const uint64_t t0 = net::now_ns();
    ch.send(c);
    ch.flush();
    for (;;) {
      if (!recv_bounded(ch, a, t0 + kSampleTimeoutNs)) die("no reply to Ping (timeout or connection lost)");
      if (a.type() == proto::ControlReply && a.as<proto::ControlReplyMsg>().tag == c.tag) break;
      // Hello on connect, stray Expired acks, etc.: not ours
    }
    const uint64_t t1 = net::now_ns();
    if (i >= warmup) out.push_back(static_cast<double>(t1 - t0));
  }
  return out;
}

// One big SELL rests at kBookPrice in book 0 from account 0; each sample is a 1-lot IOC BUY from
// account 1 that lifts one lot: Fill then Ack. Time until the Ack.
std::vector<double> run_decjz(net::Channel& ch, uint64_t iters, uint64_t warmup) {
  rt::control(ch, proto::ResetBooks);
  proto::NewOrderMsg rest;
  rest.account = 0;
  rest.side = proto::Sell;
  rest.otype = proto::Limit;
  rest.book = 0;
  rest.price = kBookPrice;
  rest.qty = static_cast<int64_t>(iters + warmup + 10);
  rest.tag = 0xffffffffu;
  const proto::AckMsg ra = rt::submit_sync(ch, rest);
  if (ra.status != proto::Rested) die("the backstop SELL did not rest (status " + std::to_string(ra.status) + ")");

  proto::NewOrderMsg ioc;
  ioc.account = 1;
  ioc.side = proto::Buy;
  ioc.otype = proto::IOC;
  ioc.book = 0;
  ioc.price = kBookPrice;
  ioc.qty = 1;

  std::vector<double> out;
  out.reserve(iters);
  proto::Any a;
  uint64_t bad = 0;
  for (uint64_t i = 0; i < warmup + iters; ++i) {
    ioc.tag = static_cast<uint32_t>(i);
    bool filled = false;
    uint8_t status = proto::Rejected;
    const uint64_t t0 = net::now_ns();
    ch.send(ioc);
    ch.flush();
    for (;;) {
      if (!recv_bounded(ch, a, t0 + kSampleTimeoutNs)) die("no Ack for the IOC (timeout or connection lost)");
      if (a.type() == proto::Fill) {
        if (a.as<proto::FillMsg>().tag == ioc.tag) filled = true;
      } else if (a.type() == proto::Ack) {
        const auto& k = a.as<proto::AckMsg>();
        if (proto::unsolicited(k.status)) continue;
        if (k.tag != ioc.tag) die("Ack tag mismatch: something else was in flight");
        status = k.status;
        break;
      }
    }
    const uint64_t t1 = net::now_ns();
    if (i >= warmup) out.push_back(static_cast<double>(t1 - t0));
    if (!filled || status != proto::Filled) ++bad;
  }
  if (bad) std::printf("  (warning: %llu decjz samples did not see Fill then Ack{Filled})\n", (unsigned long long)bad);
  return out;
}

// Three orders without waiting: SELL 1 @ kIncPrice in book 2, SELL 1 @ kIncPrice in book 3,
// IOC BUY 1 @ kIncPrice in book 3 from account 1. Read until the third Ack. Books are reset every
// 1,000 iterations (outside the timed region) so book 2 does not grow without bound.
std::vector<double> run_inc(net::Channel& ch, uint64_t iters, uint64_t warmup) {
  rt::control(ch, proto::ResetBooks);
  proto::NewOrderMsg s2, s3, buy;
  s2.account = 0;
  s2.side = proto::Sell;
  s2.otype = proto::Limit;
  s2.book = 2;
  s2.price = kIncPrice;
  s2.qty = 1;
  s3 = s2;
  s3.book = 3;
  buy.account = 1;
  buy.side = proto::Buy;
  buy.otype = proto::IOC;
  buy.book = 3;
  buy.price = kIncPrice;
  buy.qty = 1;

  std::vector<double> out;
  out.reserve(iters);
  proto::Any a;
  uint64_t bad = 0;
  for (uint64_t i = 0; i < warmup + iters; ++i) {
    if (i && i % 1000 == 0) rt::control(ch, proto::ResetBooks);
    const uint32_t base = static_cast<uint32_t>(i * 4);
    s2.tag = base;
    s3.tag = base + 1;
    buy.tag = base + 2;
    int acks = 0;
    bool filled = false;
    uint8_t last_status = proto::Rejected;
    const uint64_t t0 = net::now_ns();
    ch.send(s2);
    ch.send(s3);
    ch.send(buy);
    ch.flush();
    for (;;) {
      if (!recv_bounded(ch, a, t0 + kSampleTimeoutNs)) die("no Ack for the INC triple (timeout or connection lost)");
      if (a.type() == proto::Fill) {
        if (a.as<proto::FillMsg>().tag == buy.tag) filled = true;
      } else if (a.type() == proto::Ack) {
        const auto& k = a.as<proto::AckMsg>();
        if (proto::unsolicited(k.status)) continue;
        if (k.tag < base || k.tag > base + 2) die("Ack tag mismatch in the INC shape");
        ++acks;
        if (k.tag == buy.tag) {
          last_status = k.status;
          break;
        }
      }
    }
    const uint64_t t1 = net::now_ns();
    if (i >= warmup) out.push_back(static_cast<double>(t1 - t0));
    if (acks != 3 || !filled || last_status != proto::Filled) ++bad;
  }
  if (bad) std::printf("  (warning: %llu inc samples did not see 3 Acks with the IOC filled)\n", (unsigned long long)bad);
  return out;
}

// ---- in-process points -----------------------------------------------------------------------

struct NoSink : fb::Sink {
  void fill(const fb::Order&, bool, fb::Price, fb::Qty) override {}
  void cancelled(const fb::Order&) override {}
  void trade(uint32_t, fb::Price, fb::Qty, uint8_t, uint8_t) override {}
  void level(uint32_t, uint8_t, fb::Price, fb::Qty, uint32_t) override {}
  void reset(uint32_t) override {}
};

// The clock on Apple silicon ticks at 24 MHz (41.7 ns), coarser than one in-process submit, so
// each sample times `kBatch` submits and reports ns per order.
constexpr uint64_t kBatch = 32;

std::vector<double> run_core_decjz(uint64_t iters, uint64_t warmup) {
  NoSink sink;
  fb::Config cfg;
  cfg.books = 16;
  cfg.accounts = 4;
  cfg.stp = fb::STP::ExpireMaker;
  fb::Core core(cfg, &sink);
  proto::NewOrderMsg rest;
  rest.account = 0;
  rest.side = proto::Sell;
  rest.otype = proto::Limit;
  rest.book = 0;
  rest.price = kBookPrice;
  rest.qty = static_cast<int64_t>((iters + warmup) * kBatch + 10);
  if (core.submit(rest, 0).status != proto::Rested) die("in-process backstop did not rest");
  proto::NewOrderMsg ioc;
  ioc.account = 1;
  ioc.side = proto::Buy;
  ioc.otype = proto::IOC;
  ioc.book = 0;
  ioc.price = kBookPrice;
  ioc.qty = 1;
  std::vector<double> out;
  out.reserve(iters);
  uint64_t bad = 0;
  for (uint64_t i = 0; i < warmup + iters; ++i) {
    const uint64_t t0 = net::now_ns();
    for (uint64_t k = 0; k < kBatch; ++k) {
      ioc.tag = static_cast<uint32_t>(i * kBatch + k);
      if (core.submit(ioc, 0).status != proto::Filled) ++bad;
    }
    const uint64_t t1 = net::now_ns();
    if (i >= warmup) out.push_back(static_cast<double>(t1 - t0) / static_cast<double>(kBatch));
  }
  if (bad) std::printf("  (warning: %llu fb::Core submits were not filled)\n", (unsigned long long)bad);
  return out;
}

std::vector<double> run_engine_decjz(uint64_t iters, uint64_t warmup) {
  ob::Engine eng(16, 4, ob::STP::ExpireMaker);
  eng.log_trades = false;
  ob::Template rest;
  rest.account = 0;
  rest.book = 0;
  rest.side = ob::Side::Sell;
  rest.type = ob::Type::Limit;
  rest.price = kBookPrice;
  rest.qty = static_cast<int64_t>((iters + warmup) * kBatch + 10);
  eng.submit(rest);
  eng.run();
  if (eng.resting_qty(0, ob::Side::Sell, kBookPrice) != rest.qty) die("ob::Engine backstop did not rest");
  ob::Template ioc;
  ioc.account = 1;
  ioc.book = 0;
  ioc.side = ob::Side::Buy;
  ioc.type = ob::Type::IOC;
  ioc.price = kBookPrice;
  ioc.qty = 1;
  std::vector<double> out;
  out.reserve(iters);
  for (uint64_t i = 0; i < warmup + iters; ++i) {
    const uint64_t t0 = net::now_ns();
    for (uint64_t k = 0; k < kBatch; ++k) {
      eng.submit(ioc);
      eng.run();
    }
    const uint64_t t1 = net::now_ns();
    if (i >= warmup) out.push_back(static_cast<double>(t1 - t0) / static_cast<double>(kBatch));
  }
  const int64_t expect = rest.qty - static_cast<int64_t>((iters + warmup) * kBatch);
  if (eng.resting_qty(0, ob::Side::Sell, kBookPrice) != expect)
    std::printf("  (warning: ob::Engine backstop quantity is not what %llu fills should leave)\n",
                (unsigned long long)((iters + warmup) * kBatch));
  return out;
}

// ---- output ----------------------------------------------------------------------------------

void print_row(const Row& r) {
  std::printf("| %-21s | %-6s | %9.3f | %9.3f | %9.3f | %9.3f | %10.3f | %11.0f |\n", r.path.c_str(), r.shape.c_str(),
              r.st.p50 / 1e3, r.st.p90 / 1e3, r.st.p99 / 1e3, r.st.p999 / 1e3, r.st.max / 1e3, r.st.instr_per_s());
}

struct KnobPoint {
  int64_t delay_us;
  Stat st;
};

std::string json_row(const Row& r) {
  char b[512];
  std::snprintf(b, sizeof b,
                "{\"path\":\"%s\",\"shape\":\"%s\",\"n\":%zu,\"p50_us\":%.4f,\"p90_us\":%.4f,\"p99_us\":%.4f,"
                "\"p999_us\":%.4f,\"max_us\":%.4f,\"instr_per_s\":%.1f}",
                r.path.c_str(), r.shape.c_str(), r.st.n, r.st.p50 / 1e3, r.st.p90 / 1e3, r.st.p99 / 1e3,
                r.st.p999 / 1e3, r.st.max / 1e3, r.st.instr_per_s());
  return b;
}

void usage() {
  std::puts(
      "bench [--iters N] [--warmup N] [--tcp tcp://127.0.0.1:7200] [--udp udp://127.0.0.1:7201]\n"
      "      [--shm shm:///tmp/minsky-bench.shm] [--delays 0,50,100,300,1000] [--knob-transport tcp|udp|shm]\n"
      "      [--knob-iters N] [--instr-per-frame N] [--json FILE] [--no-wire]\n"
      "  defaults: --iters 50000 --warmup 5000 --knob-iters 5000 --knob-transport tcp --instr-per-frame 1000");
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t iters = 50'000, warmup = 5'000, knob_iters = 5'000;
  std::string urls[3] = {"tcp://127.0.0.1:7200", "udp://127.0.0.1:7201", "shm:///tmp/minsky-bench.shm"};
  const char* names[3] = {"tcp", "udp", "shm"};
  std::string knob = "tcp", json_path;
  std::vector<int64_t> delays = {0, 50, 100, 300, 1000};
  double instr_per_frame = 1000;
  bool wire = true;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return std::string(argv[++i]);
    };
    if (a == "--iters") iters = std::stoull(next());
    else if (a == "--warmup") warmup = std::stoull(next());
    else if (a == "--knob-iters") knob_iters = std::stoull(next());
    else if (a == "--tcp") urls[0] = next();
    else if (a == "--udp") urls[1] = next();
    else if (a == "--shm") urls[2] = next();
    else if (a == "--knob-transport") knob = next();
    else if (a == "--instr-per-frame") instr_per_frame = std::stod(next());
    else if (a == "--json") json_path = next();
    else if (a == "--no-wire") wire = false;
    else if (a == "--delays") {
      delays.clear();
      std::string s = next();
      size_t p = 0;
      while (p <= s.size()) {
        size_t c = s.find(',', p);
        if (c == std::string::npos) c = s.size();
        if (c > p) delays.push_back(std::stoll(s.substr(p, c - p)));
        p = c + 1;
      }
    } else {
      usage();
      return 2;
    }
  }
  if (knob != "tcp" && knob != "udp" && knob != "shm") {
    usage();
    return 2;
  }

  std::printf("minsky bench: %llu samples per point after %llu warm-up, each = now_ns() around send+flush+recv\n\n",
              (unsigned long long)iters, (unsigned long long)warmup);

  std::vector<Row> rows;
  std::vector<std::string> unreachable;
  std::unique_ptr<net::Channel> chans[3];
  double decjz_ips[3] = {0, 0, 0};

  auto connect_to = [&](int t) -> net::Channel* {
    if (chans[t]) return chans[t].get();
    try {
      chans[t] = net::connect(urls[t]);
      // Bounded probe: a UDP connect() succeeds with nobody listening, so only a reply proves a venue.
      run_ping(*chans[t], 1, 0);
      return chans[t].get();
    } catch (const std::exception&) {
      chans[t].reset();
      return nullptr;
    }
  };

  if (wire) {
    for (int t = 0; t < 3; ++t) {
      net::Channel* ch = connect_to(t);
      if (!ch) {
        std::printf("%s: %s unreachable, skipping\n", names[t], urls[t].c_str());
        unreachable.push_back(names[t]);
        continue;
      }
      try {
        std::printf("%s: measuring ping...", names[t]);
        std::fflush(stdout);
        auto s = run_ping(*ch, iters, warmup);
        rows.push_back({names[t], "ping", summarize(s)});
        std::printf(" decjz...");
        std::fflush(stdout);
        s = run_decjz(*ch, iters, warmup);
        rows.push_back({names[t], "decjz", summarize(s)});
        decjz_ips[t] = rows.back().st.instr_per_s();
        std::printf(" inc...");
        std::fflush(stdout);
        s = run_inc(*ch, iters, warmup);
        rows.push_back({names[t], "inc", summarize(s)});
        rt::control(*ch, proto::ResetBooks);
        std::printf(" done\n");
      } catch (const std::exception& e) {
        std::printf("\n%s: %s; skipping the rest of this transport\n", names[t], e.what());
        chans[t].reset();
      }
    }
  }

  std::printf("in-process: fb::Core...");
  std::fflush(stdout);
  {
    auto s = run_core_decjz(iters, warmup);
    rows.push_back({"in-process fb::Core", "decjz", summarize(s)});
  }
  std::printf(" ob::Engine...");
  std::fflush(stdout);
  {
    auto s = run_engine_decjz(iters, warmup);
    rows.push_back({"in-process ob::Engine", "decjz", summarize(s)});
  }
  std::printf(" done\n\n");

  std::puts("## Round trip per transport and shape (µs)\n");
  std::puts("| path                  | shape  |    p50 µs |       p90 |       p99 |     p99.9 |        max |     instr/s |");
  std::puts("|-----------------------|--------|----------:|----------:|----------:|----------:|-----------:|------------:|");
  for (const auto& r : rows) print_row(r);
  std::puts("\n(in-process rows: ns per order, each sample a batch of 32 submits; instr/s = 1e9 / p50)");

  // ---- latency knob sweep ----
  std::vector<KnobPoint> knob_pts;
  const int kt = knob == "tcp" ? 0 : knob == "udp" ? 1 : 2;
  net::Channel* kch = wire ? connect_to(kt) : nullptr;
  if (!kch) {
    std::printf("\nknob sweep on %s skipped: %s\n", knob.c_str(), wire ? "unreachable" : "--no-wire");
  } else {
    std::printf("\n## Latency knob on %s: Control{SetDelayUs} then decjz p50 (%llu samples after %llu warm-up)\n\n",
                knob.c_str(), (unsigned long long)knob_iters, (unsigned long long)std::min<uint64_t>(warmup, 500));
    try {
      for (int64_t d : delays) {
        rt::control(*kch, proto::SetDelayUs, d);
        auto s = run_decjz(*kch, knob_iters, std::min<uint64_t>(warmup, 500));
        knob_pts.push_back({d, summarize(s)});
      }
      rt::control(*kch, proto::SetDelayUs, 0);
    } catch (const std::exception& e) {
      std::printf("knob sweep aborted: %s\n", e.what());
      try {
        if (kch->alive()) rt::control(*kch, proto::SetDelayUs, 0);
      } catch (...) {
      }
    }
    if (!knob_pts.empty()) {
      const double base = knob_pts.front().st.p50;
      std::puts("| delay µs | decjz p50 µs | p50 - base µs | p99 µs | instr/s |");
      std::puts("|---------:|-------------:|--------------:|-------:|--------:|");
      for (const auto& k : knob_pts)
        std::printf("| %8lld | %12.3f | %13.3f | %6.1f | %7.0f |\n", (long long)k.delay_us, k.st.p50 / 1e3,
                    (k.st.p50 - base) / 1e3, k.st.p99 / 1e3, k.st.instr_per_s());
    }
  }

  // ---- the law, applied to Snake ----
  auto fps_str = [&](int t) {
    char b[64];
    if (decjz_ips[t] <= 0) return std::string("n/a");
    std::snprintf(b, sizeof b, "%.1f", decjz_ips[t] / instr_per_frame);
    return std::string(b);
  };
  std::printf("\nSnake at ~%.0f instructions per frame: %s fps on tcp, %s on udp, %s on shm\n", instr_per_frame,
              fps_str(0).c_str(), fps_str(1).c_str(), fps_str(2).c_str());

  // ---- JSON ----
  if (!json_path.empty()) {
    FILE* f = std::fopen(json_path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "bench: cannot write %s\n", json_path.c_str());
      return 1;
    }
    std::fprintf(f, "{\n  \"iters\": %llu,\n  \"warmup\": %llu,\n  \"unreachable\": [", (unsigned long long)iters,
                 (unsigned long long)warmup);
    for (size_t i = 0; i < unreachable.size(); ++i) std::fprintf(f, "%s\"%s\"", i ? "," : "", unreachable[i].c_str());
    std::fprintf(f, "],\n  \"rows\": [\n");
    for (size_t i = 0; i < rows.size(); ++i) std::fprintf(f, "    %s%s\n", json_row(rows[i]).c_str(), i + 1 < rows.size() ? "," : "");
    std::fprintf(f, "  ],\n  \"knob\": {\"transport\": \"%s\", \"iters\": %llu, \"points\": [\n", knob.c_str(),
                 (unsigned long long)knob_iters);
    for (size_t i = 0; i < knob_pts.size(); ++i) {
      const auto& k = knob_pts[i];
      std::fprintf(f, "    {\"delay_us\": %lld, \"p50_us\": %.4f, \"p99_us\": %.4f, \"added_us\": %.4f, \"instr_per_s\": %.1f}%s\n",
                   (long long)k.delay_us, k.st.p50 / 1e3, k.st.p99 / 1e3, (k.st.p50 - knob_pts.front().st.p50) / 1e3,
                   k.st.instr_per_s(), i + 1 < knob_pts.size() ? "," : "");
    }
    std::fprintf(f, "  ]},\n  \"snake\": {\"instr_per_frame\": %.0f, \"fps\": {", instr_per_frame);
    for (int t = 0; t < 3; ++t)
      std::fprintf(f, "%s\"%s\": %s", t ? ", " : "", names[t], decjz_ips[t] > 0 ? fps_str(t).c_str() : "null");
    std::fprintf(f, "}}\n}\n");
    std::fclose(f);
    std::printf("wrote %s\n", json_path.c_str());
  }
  return 0;
}
