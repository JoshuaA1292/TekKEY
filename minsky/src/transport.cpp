#include "transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <map>
#include <stdexcept>
#include <vector>

namespace net {

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
void spin_for_ns(uint64_t ns) {
  if (!ns) return;
  const uint64_t until = now_ns() + ns;
  while (now_ns() < until) {
  }
}

namespace {

struct Url {
  std::string scheme, host, path;
  int port = 0;
};
Url parse(const std::string& url) {
  auto p = url.find("://");
  if (p == std::string::npos) throw std::invalid_argument("url needs scheme://: " + url);
  Url u;
  u.scheme = url.substr(0, p);
  std::string rest = url.substr(p + 3);
  if (u.scheme == "shm") {
    u.path = rest;
    return u;
  }
  auto c = rest.rfind(':');
  if (c == std::string::npos) throw std::invalid_argument("url needs host:port: " + url);
  u.host = rest.substr(0, c);
  u.port = std::stoi(rest.substr(c + 1));
  if (u.host.empty()) u.host = "0.0.0.0";
  return u;
}

void set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) throw std::runtime_error("fcntl");
}
void set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}
sockaddr_in addr_of(const Url& u) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(u.port));
  if (inet_pton(AF_INET, u.host.c_str(), &a.sin_addr) != 1) throw std::invalid_argument("bad IPv4 host " + u.host);
  return a;
}

// Fixed-size record parser over a byte buffer.
struct Framer {
  std::vector<uint8_t> buf;
  size_t head = 0;
  bool bad = false;  // an unknown type byte: the stream cannot be resynchronised
  bool take(proto::Any& out) {
    if (bad) return false;
    const size_t avail = buf.size() - head;
    if (avail == 0) return false;
    const size_t n = proto::size_of(buf[head]);
    if (n == 0) {
      bad = true;
      buf.clear();
      head = 0;
      return false;
    }
    if (avail < n) return false;
    std::memcpy(out.bytes, buf.data() + head, n);
    out.size = n;
    head += n;
    if (head == buf.size()) {
      buf.clear();
      head = 0;
    } else if (head > (1 << 16)) {
      buf.erase(buf.begin(), buf.begin() + static_cast<long>(head));
      head = 0;
    }
    return true;
  }
};

// ---- TCP ------------------------------------------------------------------------
class TcpChannel : public Channel {
 public:
  explicit TcpChannel(int fd) : fd_(fd) {
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    set_nosigpipe(fd_);
    set_nonblock(fd_);
    in_.buf.reserve(1 << 16);
    out_.reserve(1 << 16);
  }
  ~TcpChannel() override { ::close(fd_); }
  // A peer that stops reading is dropped once this much output is backed up.
  static constexpr size_t kMaxBacklog = 64u << 20;
  void send(const void* msg, size_t n) override {
    const auto* p = static_cast<const uint8_t*>(msg);
    out_.insert(out_.end(), p, p + n);
    if (out_.size() - off_ >= (1 << 15)) flush();
  }
  // Writes what the socket will take now and keeps the rest; never spins on a full socket.
  void flush() override {
    while (off_ < out_.size() && alive_) {
      ssize_t w = ::send(fd_, out_.data() + off_, out_.size() - off_, 0);
      if (w > 0) {
        off_ += static_cast<size_t>(w);
      } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;  // the peer is not draining; try again on the next flush
      } else if (w < 0 && errno == EINTR) {
        continue;
      } else {
        alive_ = false;
      }
    }
    if (off_ == out_.size()) {
      out_.clear();
      off_ = 0;
    } else if (off_ > (1 << 20)) {
      out_.erase(out_.begin(), out_.begin() + static_cast<long>(off_));
      off_ = 0;
    }
    if (out_.size() - off_ > kMaxBacklog) alive_ = false;
  }
  bool try_recv(proto::Any& out) override {
    if (in_.take(out)) return true;
    if (in_.bad) alive_ = false;
    if (!alive_) return false;
    if (off_ < out_.size()) flush();  // keep pushing the backlog out
    uint8_t tmp[1 << 16];
    ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
    if (r > 0) {
      in_.buf.insert(in_.buf.end(), tmp, tmp + r);
      bool got = in_.take(out);
      if (in_.bad) alive_ = false;
      return got;
    }
    if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) alive_ = false;
    return false;
  }
  bool alive() const override { return alive_; }
  const char* kind() const override { return "tcp"; }

 private:
  int fd_;
  bool alive_ = true;
  Framer in_;
  std::vector<uint8_t> out_;
  size_t off_ = 0;  // bytes of out_ already written
};

class TcpAcceptor : public Acceptor {
 public:
  explicit TcpAcceptor(const Url& u) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket");
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = addr_of(u);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
      throw std::runtime_error("bind tcp port " + std::to_string(u.port) + ": " + std::strerror(errno));
    if (::listen(fd_, 16) < 0) throw std::runtime_error("listen");
    set_nonblock(fd_);
  }
  ~TcpAcceptor() override { ::close(fd_); }
  std::unique_ptr<Channel> try_accept() override {
    int c = ::accept(fd_, nullptr, nullptr);
    if (c < 0) return nullptr;
    return std::make_unique<TcpChannel>(c);
  }
  const char* kind() const override { return "tcp"; }

 private:
  int fd_;
};

std::unique_ptr<Channel> tcp_connect(const Url& u) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket");
  sockaddr_in a = addr_of(u);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
    ::close(fd);
    throw std::runtime_error("connect tcp " + u.host + ":" + std::to_string(u.port) + ": " + std::strerror(errno));
  }
  return std::make_unique<TcpChannel>(fd);
}

// ---- UDP ------------------------------------------------------------------------
constexpr size_t kDatagram = 1400;
bool whole_records(const uint8_t* p, size_t n);

// Client side: one socket connected to the server.
class UdpClient : public Channel {
 public:
  explicit UdpClient(const Url& u) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket");
    sockaddr_in a = addr_of(u);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) throw std::runtime_error("connect udp");
    set_nosigpipe(fd_);
    set_nonblock(fd_);
    int sz = 4 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
  }
  ~UdpClient() override { ::close(fd_); }
  void send(const void* msg, size_t n) override {
    if (out_.size() + n > kDatagram) flush();
    const auto* p = static_cast<const uint8_t*>(msg);
    out_.insert(out_.end(), p, p + n);
  }
  void flush() override {
    if (out_.empty()) return;
    for (;;) {
      ssize_t w = ::send(fd_, out_.data(), out_.size(), 0);
      if (w >= 0) break;
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) continue;
      alive_ = false;
      break;
    }
    out_.clear();
  }
  bool try_recv(proto::Any& out) override {
    if (in_.take(out)) return true;
    uint8_t tmp[kDatagram + 64];
    ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
    if (r > 0) {
      if (whole_records(tmp, static_cast<size_t>(r))) in_.buf.insert(in_.buf.end(), tmp, tmp + r);
      return in_.take(out);
    }
    if (r < 0 && errno == ECONNREFUSED) alive_ = false;
    return false;
  }
  bool alive() const override { return alive_; }
  const char* kind() const override { return "udp"; }

 private:
  int fd_;
  bool alive_ = true;
  Framer in_;
  std::vector<uint8_t> out_;
};

// A datagram is accepted only if it is a whole number of known records; anything else is
// dropped whole, so one bad datagram never desynchronises the stream.
bool whole_records(const uint8_t* p, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t k = proto::size_of(p[i]);
    if (k == 0 || i + k > n) return false;
    i += k;
  }
  return true;
}

class UdpAcceptor;
class UdpPeer : public Channel {
 public:
  UdpPeer(int fd, sockaddr_in peer) : fd_(fd), peer_(peer) {}
  void send(const void* msg, size_t n) override {
    if (out_.size() + n > kDatagram) flush();
    const auto* p = static_cast<const uint8_t*>(msg);
    out_.insert(out_.end(), p, p + n);
  }
  void flush() override {
    if (out_.empty()) return;
    for (;;) {
      ssize_t w = ::sendto(fd_, out_.data(), out_.size(), 0, reinterpret_cast<sockaddr*>(&peer_), sizeof peer_);
      if (w >= 0) break;
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) continue;
      break;
    }
    out_.clear();
  }
  bool try_recv(proto::Any& out) override { return in_.take(out); }
  bool alive() const override { return true; }  // UDP peers never say goodbye
  const char* kind() const override { return "udp"; }
  void push(const uint8_t* p, size_t n) {
    if (whole_records(p, n)) in_.buf.insert(in_.buf.end(), p, p + n);
  }

 private:
  int fd_;
  sockaddr_in peer_;
  Framer in_;
  std::vector<uint8_t> out_;
};

class UdpAcceptor : public Acceptor {
 public:
  explicit UdpAcceptor(const Url& u) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket");
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = addr_of(u);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
      throw std::runtime_error("bind udp port " + std::to_string(u.port) + ": " + std::strerror(errno));
    set_nonblock(fd_);
    int sz = 4 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
  }
  ~UdpAcceptor() override { ::close(fd_); }
  std::unique_ptr<Channel> try_accept() override {
    // Drain what arrived (bounded, so a flood cannot starve matching); hand back the first new
    // peer (if any).
    for (int k = 0; k < 1024; ++k) {
      uint8_t tmp[kDatagram + 64];
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      ssize_t r = ::recvfrom(fd_, tmp, sizeof tmp, 0, reinterpret_cast<sockaddr*>(&from), &fl);
      if (r <= 0) break;
      const uint64_t key = (static_cast<uint64_t>(from.sin_addr.s_addr) << 16) | from.sin_port;
      auto it = peers_.find(key);
      if (it == peers_.end()) {
        auto ch = std::make_shared<UdpPeer>(fd_, from);
        it = peers_.emplace(key, ch).first;
        fresh_.push_back(ch);
      }
      it->second->push(tmp, static_cast<size_t>(r));
    }
    if (fresh_.empty()) return nullptr;
    auto ch = fresh_.front();
    fresh_.pop_front();
    return std::make_unique<Forward>(ch);
  }
  const char* kind() const override { return "udp"; }

 private:
  // The acceptor keeps the peer alive (it must push into it); the session holds a forwarder.
  struct Forward : Channel {
    std::shared_ptr<UdpPeer> p;
    explicit Forward(std::shared_ptr<UdpPeer> x) : p(std::move(x)) {}
    void send(const void* m, size_t n) override { p->send(m, n); }
    void flush() override { p->flush(); }
    bool try_recv(proto::Any& o) override { return p->try_recv(o); }
    bool alive() const override { return true; }
    const char* kind() const override { return "udp"; }
  };
  int fd_;
  std::map<uint64_t, std::shared_ptr<UdpPeer>> peers_;
  std::deque<std::shared_ptr<UdpPeer>> fresh_;
};

// ---- shared memory ----------------------------------------------------------------
constexpr uint32_t kSlots = 4096;
constexpr uint32_t kSlot = 64;

struct alignas(64) ShmRing {
  alignas(64) std::atomic<uint64_t> head;
  alignas(64) std::atomic<uint64_t> tail;
  alignas(64) uint8_t slots[kSlots][kSlot];

  bool push(const void* msg, size_t n) {
    if (n == 0 || n >= kSlot) return false;
    const uint64_t t = tail.load(std::memory_order_relaxed);
    if (t - head.load(std::memory_order_acquire) == kSlots) return false;
    uint8_t* s = slots[t % kSlots];
    s[0] = static_cast<uint8_t>(n);
    std::memcpy(s + 1, msg, n);
    tail.store(t + 1, std::memory_order_release);
    return true;
  }
  bool pop(proto::Any& out) {
    const uint64_t h = head.load(std::memory_order_relaxed);
    if (h == tail.load(std::memory_order_acquire)) return false;
    const uint8_t* s = slots[h % kSlots];
    const size_t n = s[0];
    // the other side's length byte is not trusted: it must be exactly the record's size
    const bool ok = n > 0 && n <= proto::kMaxMsg && n == proto::size_of(s[1]);
    if (ok) {
      out.size = n;
      std::memcpy(out.bytes, s + 1, n);
    }
    head.store(h + 1, std::memory_order_release);
    return ok;
  }
  void reset() {
    head.store(0);
    tail.store(0);
  }
};
struct ShmFile {
  char magic[8];
  alignas(64) std::atomic<uint64_t> client_epoch;   // bumped by a connecting client
  alignas(64) std::atomic<uint64_t> server_epoch;   // set by the server once it has reset the rings
  alignas(64) std::atomic<uint64_t> server_alive;
  ShmRing c2s;
  ShmRing s2c;
};
static_assert(kSlot - 1 >= proto::kMaxMsg);

ShmFile* map_shm(const std::string& path, bool create) {
  int fd = ::open(path.c_str(), create ? (O_RDWR | O_CREAT) : O_RDWR, 0666);
  if (fd < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));
  if (create && ftruncate(fd, sizeof(ShmFile)) < 0) throw std::runtime_error("ftruncate");
  struct stat st{};
  fstat(fd, &st);
  if (st.st_size < static_cast<off_t>(sizeof(ShmFile))) throw std::runtime_error("shm file too small: " + path);
  void* p = ::mmap(nullptr, sizeof(ShmFile), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) throw std::runtime_error("mmap");
  return static_cast<ShmFile*>(p);
}

class ShmChannel : public Channel {
 public:
  // server = true: we write s2c and read c2s
  ShmChannel(ShmFile* f, bool server, uint64_t epoch) : f_(f), server_(server), epoch_(epoch) {}
  void send(const void* msg, size_t n) override {
    ShmRing& r = server_ ? f_->s2c : f_->c2s;
    if (dead_) return;
    uint64_t t0 = 0;
    while (!r.push(msg, n)) {
      if (!alive()) return;
      // a peer that stops draining for a second is gone (a crashed client leaves the epoch as is)
      const uint64_t now = now_ns();
      if (!t0) t0 = now;
      else if (now - t0 > 1'000'000'000ull) {
        dead_ = true;
        return;
      }
    }
  }
  void flush() override {}
  bool try_recv(proto::Any& out) override {
    if (!alive()) return false;
    ShmRing& r = server_ ? f_->c2s : f_->s2c;
    while (r.pop(out)) return true;  // pop skips invalid slots
    return false;
  }
  bool alive() const override {
    if (dead_) return false;
    return server_ ? f_->client_epoch.load(std::memory_order_acquire) == epoch_
                   : f_->server_alive.load(std::memory_order_acquire) == 1;
  }
  const char* kind() const override { return "shm"; }

 private:
  ShmFile* f_;
  bool server_;
  uint64_t epoch_;
  bool dead_ = false;
};

class ShmAcceptor : public Acceptor {
 public:
  explicit ShmAcceptor(const Url& u) : path_(u.path) {
    f_ = map_shm(path_, true);
    std::memset(static_cast<void*>(f_), 0, sizeof(ShmFile));
    std::memcpy(f_->magic, "MNSKSHM", 8);
    f_->c2s.reset();
    f_->s2c.reset();
    f_->client_epoch.store(0);
    f_->server_epoch.store(0);
    f_->server_alive.store(1, std::memory_order_release);
  }
  ~ShmAcceptor() override {
    f_->server_alive.store(0);
    ::munmap(f_, sizeof(ShmFile));
  }
  // The server owns the rings: on a new epoch it resets them (the previous session's channel
  // is already dead, its epoch having moved, and the new client does not send until it sees
  // server_epoch catch up), then acknowledges.
  std::unique_ptr<Channel> try_accept() override {
    const uint64_t e = f_->client_epoch.load(std::memory_order_acquire);
    if (e == seen_) return nullptr;
    seen_ = e;
    f_->c2s.reset();
    f_->s2c.reset();
    f_->server_epoch.store(e, std::memory_order_release);
    return std::make_unique<ShmChannel>(f_, true, e);
  }
  const char* kind() const override { return "shm"; }

 private:
  std::string path_;
  ShmFile* f_;
  uint64_t seen_ = 0;
};

std::unique_ptr<Channel> shm_connect(const Url& u) {
  ShmFile* f = map_shm(u.path, false);
  if (std::memcmp(f->magic, "MNSKSHM", 8) != 0 || f->server_alive.load() != 1)
    throw std::runtime_error("no exchange on " + u.path);
  // Bump the epoch and wait for the server to reset the rings and acknowledge. A stale file
  // from a crashed server (server_alive still 1) times out here.
  const uint64_t e = f->client_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint64_t t0 = now_ns();
  while (f->server_epoch.load(std::memory_order_acquire) != e) {
    if (now_ns() - t0 > 2'000'000'000ull) throw std::runtime_error("no exchange answering on " + u.path);
  }
  return std::make_unique<ShmChannel>(f, false, e);
}

}  // namespace

std::unique_ptr<Channel> connect(const std::string& url) {
  std::signal(SIGPIPE, SIG_IGN);
  Url u = parse(url);
  if (u.scheme == "tcp") return tcp_connect(u);
  if (u.scheme == "udp") return std::make_unique<UdpClient>(u);
  if (u.scheme == "shm") return shm_connect(u);
  throw std::invalid_argument("unknown transport " + u.scheme);
}

std::unique_ptr<Acceptor> listen(const std::string& url) {
  std::signal(SIGPIPE, SIG_IGN);
  Url u = parse(url);
  if (u.scheme == "tcp") return std::make_unique<TcpAcceptor>(u);
  if (u.scheme == "udp") return std::make_unique<UdpAcceptor>(u);
  if (u.scheme == "shm") return std::make_unique<ShmAcceptor>(u);
  throw std::invalid_argument("unknown transport " + u.scheme);
}

}  // namespace net
