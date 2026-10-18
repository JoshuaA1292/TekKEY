// Message channels over TCP, UDP and a shared-memory ring, behind one interface, so the exchange
// and the router can be moved between transports without changing a line of either.
//
//   tcp://host:port   byte stream of fixed-size records, TCP_NODELAY, non-blocking, busy-polled
//   udp://host:port   records packed into datagrams (never split across them); the server keeps
//                     one channel per peer address
//   shm://path        a file holding two single-producer single-consumer rings of 64-byte slots,
//                     one per direction; both sides spin on cache lines, no syscalls at all
//
// All channels are non-blocking. send() buffers; flush() pushes the batch out; try_recv() returns
// one record if one is complete. recv() spins. Nothing here ever sleeps: the round trip is the
// benchmark, and a sleep would be a point on the law that is not the transport's.
#pragma once
#include <memory>
#include <string>

#include "proto.hpp"

namespace net {

class Channel {
 public:
  virtual ~Channel() = default;
  virtual void send(const void* msg, size_t n) = 0;
  template <class T>
  void send(const T& m) { send(&m, sizeof(T)); }
  virtual void flush() = 0;
  virtual bool try_recv(proto::Any& out) = 0;
  virtual bool alive() const = 0;
  virtual const char* kind() const = 0;
  // Spin until a record arrives or the peer is gone. Returns false if the channel died.
  bool recv(proto::Any& out) {
    while (alive())
      if (try_recv(out)) return true;
    return false;
  }
};

class Acceptor {
 public:
  virtual ~Acceptor() = default;
  // Non-blocking. Also drains transport-level input (UDP) into existing channels.
  virtual std::unique_ptr<Channel> try_accept() = 0;
  virtual const char* kind() const = 0;
};

std::unique_ptr<Channel> connect(const std::string& url);
std::unique_ptr<Acceptor> listen(const std::string& url);

uint64_t now_ns();
void spin_for_ns(uint64_t ns);

}  // namespace net
