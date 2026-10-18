// Server robustness against a live exchange: the three defects the review reproduced.
#include <cstdio>
#include <string>
#include <thread>

#include "router.hpp"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}
bool recv_type_within(net::Channel& ch, uint8_t type, proto::Any& out, uint64_t ns) {
  const uint64_t t0 = net::now_ns();
  while (net::now_ns() - t0 < ns) {
    if (ch.try_recv(out) && out.type() == type) return true;
    if (!ch.alive()) return false;
  }
  return false;
}
}  // namespace

int main(int argc, char** argv) {
  std::string url = argc > 1 ? argv[1] : "tcp://127.0.0.1:7000";
  try {
    // 1. a malformed byte closes that session only
    {
      auto bad = net::connect(url);
      uint8_t junk = 0xff;
      bad->send(&junk, 1);
      bad->flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      auto ok = net::connect(url);
      proto::Any a;
      check(recv_type_within(*ok, proto::Hello, a, 2'000'000'000ull), "exchange still answers after a malformed byte");
      check(rt::control(*ok, proto::Ping).op == proto::Ping, "ping after malformed byte");
    }
    // 2. a maker fill reaches a session that is not talking (two sessions: not on shm, which
    //    serves one client; the keyboard uses TCP there)
    if (url.rfind("shm://", 0) != 0) {
      auto maker = net::connect(url);
      auto taker = net::connect(url);
      rt::control(*taker, proto::ResetBooks);
      proto::NewOrderMsg m;
      m.account = 0;
      m.book = 5;
      m.side = proto::Sell;
      m.price = 1000;
      m.qty = 1;
      m.tag = 77;
      auto ack = rt::submit_sync(*maker, m);
      check(ack.status == proto::Rested, "maker rests");
      proto::NewOrderMsg t = m;
      t.account = 1;
      t.side = proto::Buy;
      t.otype = proto::IOC;
      t.tag = 78;
      check(rt::submit_sync(*taker, t).status == proto::Filled, "taker filled");
      proto::Any f;
      check(recv_type_within(*maker, proto::Fill, f, 2'000'000'000ull) && f.as<proto::FillMsg>().tag == 77 &&
                f.as<proto::FillMsg>().is_maker == 1,
            "maker fill delivered to the passive session");
      // and an STP expiry is reported to the passive owner
      m.tag = 79;
      rt::submit_sync(*maker, m);
      t.account = 0;
      t.tag = 80;
      rt::submit_sync(*taker, t);  // same account: expire-maker
      check(recv_type_within(*maker, proto::Ack, f, 2'000'000'000ull) && f.as<proto::AckMsg>().status == proto::Expired &&
                f.as<proto::AckMsg>().tag == 79,
            "STP expiry reported to the passive owner");
      // a taker cut short by STP is Done with its real remainder, not Filled
      rt::control(*taker, proto::SetSTP, 2);  // cancel-taker
      m.tag = 81;
      rt::submit_sync(*maker, m);
      t.tag = 82;
      auto r = rt::submit_sync(*taker, t);
      check(r.status == proto::Done && r.remaining == 1, "cancel-taker STP: Done with remaining 1");
      rt::control(*taker, proto::SetSTP, 1);
      rt::control(*taker, proto::ResetBooks);
    }
    // 3. a client that stops reading does not wedge the exchange
    {
      auto flood = net::connect(url);
      proto::NewOrderMsg m;
      m.account = 0;
      m.book = 6;
      m.side = proto::Sell;
      m.price = 1000;
      m.qty = 1;
      for (int i = 0; i < 400000; ++i) {
        m.tag = static_cast<uint32_t>(i);
        flood->send(m);
        if (i % 1000 == 0) flood->flush();
      }
      flood->flush();
      // never read a reply; meanwhile a new client must still be served
      auto ok = net::connect(url);
      proto::Any a;
      check(recv_type_within(*ok, proto::Hello, a, 5'000'000'000ull), "new client served while another is not reading");
      const uint64_t t0 = net::now_ns();
      auto r = rt::control(*ok, proto::Ping);
      check(r.op == proto::Ping && net::now_ns() - t0 < 2'000'000'000ull, "ping answered promptly during the flood");
      rt::control(*ok, proto::ResetBooks);
    }
    // 4. shm-style reconnects: open and close the same transport repeatedly
    {
      for (int i = 0; i < 5; ++i) {
        auto c = net::connect(url);
        check(rt::control(*c, proto::Ping).op == proto::Ping, "reconnect " + std::to_string(i));
      }
    }
  } catch (const std::exception& e) {
    std::printf("FAIL: exception %s\n", e.what());
    ++failures;
  }
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
