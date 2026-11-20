// The process: bind an ephemeral listener, register the port, then serve every HTTP/1.1
// keep-alive connection on a *blocking* worker pool until /done.
//
// gVisor physics: a blocking recv()/send() pair costs two systrap exits per request, while an
// epoll reactor pays a third (epoll_wait) before every read.  So the hot path parks one small
// 256 KiB-stack thread per active connection, reads with plain recv(), routes, and writes with a
// single send().  Threads are created on demand (16 warm at boot, +1 whenever every worker is busy)
// and retire themselves after 3 s idle, so a 50-100 connection clone burst costs no fixed pool
// starvation and leaves ~16 warm threads afterwards.  Cached responses (res.full) are handed to
// send() straight from their immutable buffer: no framing, no copy, no request-id work unless the
// cached bytes carry a request-id placeholder.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "forge.hpp"

namespace kestrel {

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) {
  g_stop = 1;
  std::_Exit(0);  // async-signal-safe; the harness only needs a prompt, clean process exit
}

static inline long long now_ms() {
  return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static const char* status_text(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 409:
      return "Conflict";
    case 422:
      return "Unprocessable Entity";
    case 431:
      return "Request Header Fields Too Large";
    default:
      return "Internal Server Error";
  }
}

// ---------------------------------------------------------------- connection state

static constexpr size_t kMaxHeaderBytes = 65536;
static constexpr size_t kMaxBodyBytes = 256ull << 20;
static constexpr size_t kMaxInputBytes = (256ull << 20) + kMaxHeaderBytes;
static constexpr long long kReapMs = 6000;   // > undici's 4 s idle keep-alive: it closes first
static constexpr int kWorkersStart = 16;
static constexpr int kMaxWorkers = 210;      // stay well under the 256-pid sandbox budget
static constexpr int kIdleExitSeconds = 3;

struct Conn {
  int fd = -1;
  std::string in;       // receive buffer (persistent across requests)
  size_t in_pos = 0;    // first unconsumed byte
  std::string out;      // scratch framing buffer for the non-cached paths
  std::string hot;      // per-connection mutable copy for request-id stamping
  const void* hot_src = nullptr;
  Request req;          // reused parser target
  Response res;         // reused router target
  bool sent_continue = false;

  void begin(int socket_fd) {
    fd = socket_fd;
    in.clear();
    in_pos = 0;
    out.clear();
    hot.clear();
    hot_src = nullptr;
    sent_continue = false;
    req.body.clear();
    res.reset();
  }
};

// ---------------------------------------------------------------- request parsing

enum class Parse { Ok, NeedMore, Bad };

static Parse decode_chunked(const char* p, size_t n, std::string& body, size_t& used) {
  body.clear();
  size_t pos = 0;
  for (;;) {
    if (pos >= n) return Parse::NeedMore;
    const char* nl = (const char*)std::memchr(p + pos, '\n', n - pos);
    if (!nl) return Parse::NeedMore;
    size_t line_end = (size_t)(nl - p);
    size_t line_len = line_end - pos;
    if (line_len > 0 && p[pos + line_len - 1] == '\r') line_len--;
    if (line_len > 1024) return Parse::Bad;
    size_t digits = 0;
    unsigned long long size = 0;
    for (size_t i = 0; i < line_len; i++) {
      char c = p[pos + i];
      if (c == ';' || c == ' ' || c == '\t') break;
      int v = hexval(c);
      if (v < 0) return Parse::Bad;
      if (size > ((kMaxBodyBytes) >> 4)) return Parse::Bad;
      size = (size << 4) | (unsigned long long)v;
      digits++;
    }
    if (digits == 0) return Parse::Bad;
    size_t data_at = line_end + 1;
    if (size == 0) {
      // Trailer section: consume header lines until an empty one.
      size_t q = data_at;
      for (;;) {
        if (q >= n) return Parse::NeedMore;
        const char* nl2 = (const char*)std::memchr(p + q, '\n', n - q);
        if (!nl2) return Parse::NeedMore;
        size_t le2 = (size_t)(nl2 - p);
        size_t ll = le2 - q;
        if (ll > 0 && p[q + ll - 1] == '\r') ll--;
        q = le2 + 1;
        if (ll == 0) {
          used = q;
          return Parse::Ok;
        }
      }
    }
    if (size > kMaxBodyBytes || body.size() + size > kMaxBodyBytes) return Parse::Bad;
    if (n - data_at < size + 2) return Parse::NeedMore;
    body.append(p + data_at, (size_t)size);
    size_t after = data_at + (size_t)size;
    if (p[after] == '\r' && p[after + 1] == '\n')
      after += 2;
    else if (p[after] == '\n')
      after += 1;
    else
      return Parse::Bad;
    pos = after;
  }
}

// Parse one complete request starting at c.in_pos. On Ok, `end` is the absolute first byte after
// the request. Nothing is consumed on NeedMore, so a pipelined trailing request is never dropped.
static Parse parse_request(Conn& c, size_t& end, bool& keep_alive, bool& need_continue) {
  const std::string& in = c.in;
  const size_t start = c.in_pos;
  const size_t avail = in.size() - start;
  if (avail == 0) return Parse::NeedMore;

  size_t hdr = in.find("\r\n\r\n", start);
  if (hdr == std::string::npos) return avail > kMaxHeaderBytes ? Parse::Bad : Parse::NeedMore;
  if (hdr - start > kMaxHeaderBytes) return Parse::Bad;

  size_t eol = in.find("\r\n", start);
  if (eol == std::string::npos || eol > hdr) return Parse::NeedMore;

  size_t sp1 = in.find(' ', start);
  if (sp1 == std::string::npos || sp1 > eol) return Parse::Bad;
  size_t sp2 = in.find(' ', sp1 + 1);
  if (sp2 == std::string::npos || sp2 > eol) return Parse::Bad;

  Request& req = c.req;
  req.method.assign(in, start, sp1 - start);
  req.target.assign(in, sp1 + 1, sp2 - sp1 - 1);
  std::string version = in.substr(sp2 + 1, eol - sp2 - 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return Parse::Bad;

  req.headers.clear();
  size_t total = 0;
  size_t pos = eol + 2;
  while (pos < hdr) {
    size_t nl = in.find("\r\n", pos);
    if (nl == std::string::npos || nl > hdr) return Parse::Bad;
    if (nl == pos) break;
    total += nl - pos;
    if (total > kMaxHeaderBytes) return Parse::Bad;
    size_t colon = in.find(':', pos);
    if (colon != std::string::npos && colon < nl) {
      size_t nstart = pos, nend = colon;
      while (nend > nstart && (in[nend - 1] == ' ' || in[nend - 1] == '\t')) nend--;
      std::string name(in, nstart, nend - nstart);
      for (char& ch : name)
        if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
      size_t vstart = colon + 1, vend = nl;
      while (vstart < vend && (in[vstart] == ' ' || in[vstart] == '\t')) vstart++;
      while (vend > vstart && (in[vend - 1] == ' ' || in[vend - 1] == '\t')) vend--;
      req.headers.emplace_back(std::move(name), in.substr(vstart, vend - vstart));
    }
    pos = nl + 2;
  }

  req.host = req.header("host");
  size_t question = req.target.find('?');
  if (question == std::string::npos) {
    req.path = url_decode(req.target, false);
    req.query.clear();
  } else {
    req.path = url_decode(req.target.substr(0, question), false);
    req.query = req.target.substr(question + 1);
  }

  std::string connection = to_lower(req.header("connection"));
  if (version == "HTTP/1.0")
    keep_alive = connection.find("keep-alive") != std::string::npos;
  else
    keep_alive = connection.find("close") == std::string::npos;

  need_continue = to_lower(req.header("expect")).find("100-continue") != std::string::npos;

  req.body.clear();
  size_t body_at = hdr + 4;
  std::string transfer = to_lower(req.header("transfer-encoding"));
  if (transfer.find("chunked") != std::string::npos) {
    size_t used = 0;
    Parse r = decode_chunked(in.data() + body_at, in.size() - body_at, req.body, used);
    if (r != Parse::Ok) return r;
    end = body_at + used;
    return Parse::Ok;
  }
  std::string length_header = req.header("content-length");
  if (!length_header.empty()) {
    char* parse_end = nullptr;
    unsigned long long size = std::strtoull(length_header.c_str(), &parse_end, 10);
    if (parse_end == length_header.c_str() || size > kMaxBodyBytes) return Parse::Bad;
    if (in.size() - body_at < size) return Parse::NeedMore;
    if (size) req.body.assign(in.data() + body_at, (size_t)size);
    end = body_at + (size_t)size;
    return Parse::Ok;
  }
  end = body_at;
  return Parse::Ok;
}

// ---------------------------------------------------------------- response framing

static inline void stamp_request_id(Store& store, char* dst) {
  uint64_t n = ++store.request_seq;
  static const char* hex = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    dst[i] = hex[n & 15];
    n >>= 4;
  }
}

static void append_request_id(std::string& out, Store& store) {
  out += "X-GitHub-Request-Id: ";
  char id[16];
  stamp_request_id(store, id);
  out.append(id, sizeof(id));
  out += "\r\n";
}

static void queue_normal(std::string& out, Response& res, bool keep_alive, Store& store) {
  out.reserve(out.size() + res.body.size() + 512);
  out += "HTTP/1.1 ";
  append_uint(out, (unsigned long long)res.status);
  out.push_back(' ');
  out += status_text(res.status);
  out += "\r\n";
  bool has_body = res.status != 204;
  if (has_body && !res.content_type.empty()) {
    out += "Content-Type: ";
    out += res.content_type;
    out += "\r\n";
  }
  if (has_body) {
    out += "Content-Length: ";
    append_uint(out, res.body.size());
    out += "\r\n";
  }
  if (res.with_request_id) append_request_id(out, store);
  for (const auto& kv : res.headers) {
    out += kv.first;
    out += ": ";
    out += kv.second;
    out += "\r\n";
  }
  if (!keep_alive) out += "Connection: close\r\n";
  out += "\r\n";
  if (has_body) out += res.body;
}

// Queue a complete pre-framed HTTP response. When the connection needs no request-id stamp, the
// caller sends the shared immutable buffer directly from this function's hot path.
static void queue_full(std::string& out, const Response& res, bool keep_alive, Store& store) {
  const std::string& f = *res.full;
  char rid[16];
  if (res.id_offset) stamp_request_id(store, rid);

  if (keep_alive) {
    size_t base = out.size();
    out.append(f);
    if (res.id_offset && out.size() >= base + res.id_offset + 16)
      std::memcpy(&out[base + res.id_offset], rid, 16);
    return;
  }

  // The client asked to close: insert Connection: close before the header terminator.
  size_t he = f.find("\r\n\r\n");
  if (he == std::string::npos) {
    out.append(f);
    return;
  }
  size_t base = out.size();
  out.append(f.data(), he);
  static const char kClose[] = "\r\nConnection: close";
  out.append(kClose, sizeof(kClose) - 1);
  out.append(f.data() + he, f.size() - he);
  if (res.id_offset)
    std::memcpy(&out[base + sizeof(kClose) - 1 + res.id_offset], rid, 16);
}

static bool send_all(int fd, const char* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

// Send one response. The fast path is a single send() from an immutable cached buffer; the
// request-id placeholder is stamped in a per-connection copy so the shared bytes stay read-only.
static bool send_response(Conn& c, Response& res, bool keep_alive, Store& store) {
  if (res.full && keep_alive) {
    const std::string& f = *res.full;
    if (!res.id_offset) return send_all(c.fd, f.data(), f.size());
    if (c.hot_src != res.full.get() || c.hot.size() != f.size()) {
      c.hot.assign(f.data(), f.size());
      c.hot_src = res.full.get();
    }
    if (c.hot.size() >= res.id_offset + 16) stamp_request_id(store, &c.hot[res.id_offset]);
    return send_all(c.fd, c.hot.data(), c.hot.size());
  }
  c.out.clear();
  if (res.full)
    queue_full(c.out, res, keep_alive, store);
  else
    queue_normal(c.out, res, keep_alive, store);
  return send_all(c.fd, c.out.data(), c.out.size());
}

// Consume every complete request already buffered. Returns false when the connection must close.
static bool pump(Conn& c, Store& store) {
  static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
  for (;;) {
    while (c.in_pos < c.in.size() && (c.in[c.in_pos] == '\r' || c.in[c.in_pos] == '\n')) c.in_pos++;
    if (c.in_pos >= c.in.size()) break;

    bool keep_alive = true;
    bool need_continue = false;
    size_t end = c.in_pos;
    Parse state = parse_request(c, end, keep_alive, need_continue);
    if (state == Parse::NeedMore) {
      if (need_continue && !c.sent_continue) {
        if (!send_all(c.fd, kContinue, sizeof(kContinue) - 1)) return false;
        c.sent_continue = true;
      }
      break;
    }
    if (state == Parse::Bad) return false;
    c.sent_continue = false;

    c.res.reset();
    try {
      route(store, c.req, c.res);
    } catch (...) {
      c.res.reset();
      fail_json(store, c.res, 500, "Internal Server Error");
    }
    if (!send_response(c, c.res, keep_alive, store)) return false;
    c.in_pos = end;
    if (!keep_alive) return false;
  }

  if (c.in_pos == c.in.size()) {
    c.in.clear();
    c.in_pos = 0;
  } else if (c.in_pos > (1u << 20)) {
    c.in.erase(0, c.in_pos);
    c.in_pos = 0;
  }
  return true;
}

// One blocking connection: parse what is buffered, then park in recv() for the next request.
static void serve_connection(int fd, Store& store, Conn& c) {
  c.begin(fd);
  char buf[16384];
  for (;;) {
    if (!pump(c, store)) return;
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      if (c.in.size() - c.in_pos + (size_t)n > kMaxInputBytes) return;
      c.in.append(buf, (size_t)n);
      continue;
    }
    if (n == 0) return;  // peer closed
    if (errno == EINTR) continue;
    return;  // timeout (6 s idle), or a fatal socket error
  }
}

// ---------------------------------------------------------------- dynamic worker pool

static std::mutex g_queue_mu;
static std::condition_variable g_queue_cv;
static std::deque<int> g_queue;
static std::atomic<int> g_idle{0};
static std::atomic<int> g_total{0};
static std::atomic<bool> g_shutdown{false};

static void* worker_entry(void* arg) {
  Store* store = (Store*)arg;
  Conn conn;
  for (;;) {
    int fd;
    {
      std::unique_lock<std::mutex> lock(g_queue_mu);
      if (g_queue.empty()) {
        if (g_shutdown.load(std::memory_order_relaxed)) return nullptr;
        g_idle.fetch_add(1, std::memory_order_relaxed);
        bool woken = g_queue_cv.wait_for(lock, std::chrono::seconds(kIdleExitSeconds), [] {
          return !g_queue.empty() || g_shutdown.load(std::memory_order_relaxed);
        });
        g_idle.fetch_sub(1, std::memory_order_relaxed);
        if (g_shutdown.load(std::memory_order_relaxed)) return nullptr;
        if (g_queue.empty()) {
          if (woken) continue;  // straggler notify with no work: park again
          // Idle timeout: retire every thread above the 16-thread warm floor.
          if (g_total.load(std::memory_order_relaxed) > kWorkersStart) {
            g_total.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
          }
          continue;
        }
      }
      fd = g_queue.front();
      g_queue.pop_front();
    }
    serve_connection(fd, *store, conn);
    ::close(fd);
  }
}

static bool spawn_worker(Store* store) {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return false;
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  int rc = pthread_create(&thread, &attr, worker_entry, store);
  pthread_attr_destroy(&attr);
  return rc == 0;
}

// Queue an accepted socket. Spawns a fresh worker only when every existing worker is busy, so a
// 50-100 connection burst never stalls behind a fixed pool, and idle traffic keeps the basic 16.
static void dispatch_connection(int fd, Store* store) {
  bool need_worker = false;
  {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    g_queue.push_back(fd);
    if ((int)g_queue.size() > g_idle.load(std::memory_order_relaxed) &&
        g_total.load(std::memory_order_relaxed) < kMaxWorkers) {
      g_total.fetch_add(1, std::memory_order_relaxed);
      need_worker = true;
    }
  }
  g_queue_cv.notify_one();
  if (need_worker && !spawn_worker(store))
    g_total.fetch_sub(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- listeners

static int create_listener(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 4096) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// ---------------------------------------------------------------- tiny HTTP client (register/done)

struct Url {
  std::string host;
  int port = 80;
  std::string base;  // path prefix without trailing slash
};

static bool parse_url(const std::string& url, Url& out) {
  if (!starts_with(url, "http://")) return false;
  std::string rest = url.substr(7);
  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  out.base = slash == std::string::npos ? std::string() : rest.substr(slash);
  while (!out.base.empty() && out.base.back() == '/') out.base.pop_back();
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    out.host = authority.substr(0, colon);
    out.port = std::atoi(authority.c_str() + colon + 1);
  } else {
    out.host = authority;
    out.port = 80;
  }
  return !out.host.empty();
}

static bool http_call(const Url& url, const char* method, const std::string& path,
                      const std::string& body, int& status, std::string& response) {
  int fd = -1;
  struct sockaddr_in v4;
  std::memset(&v4, 0, sizeof(v4));
  v4.sin_family = AF_INET;
  v4.sin_port = htons((uint16_t)url.port);
  if (inet_pton(AF_INET, url.host.c_str(), &v4.sin_addr) == 1) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && ::connect(fd, (struct sockaddr*)&v4, sizeof(v4)) != 0) {
      ::close(fd);
      fd = -1;
    }
  } else {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* info = nullptr;
    std::string port = itoa10(url.port);
    if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &info) != 0 || !info) return false;
    for (struct addrinfo* ai = info; ai; ai = ai->ai_next) {
      fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0) continue;
      if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
      ::close(fd);
      fd = -1;
    }
    freeaddrinfo(info);
  }
  if (fd < 0) return false;

  std::string request;
  request.reserve(256 + body.size());
  request += method;
  request.push_back(' ');
  request += url.base + path;
  request += " HTTP/1.1\r\nHost: ";
  request += url.host;
  if (url.port != 80) {
    request.push_back(':');
    append_uint(request, (unsigned long long)url.port);
  }
  request += "\r\nAccept: */*\r\nConnection: close\r\n";
  if (!body.empty() || std::strcmp(method, "POST") == 0) {
    request += "Content-Type: application/json\r\nContent-Length: ";
    append_uint(request, body.size());
    request += "\r\n";
  }
  request += "\r\n";
  request += body;
  if (!send_all(fd, request.data(), request.size())) {
    ::close(fd);
    return false;
  }
  response.clear();
  char buf[8192];
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      response.append(buf, (size_t)n);
      if (response.size() > (1u << 20)) break;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  ::close(fd);
  size_t line_end = response.find("\r\n");
  if (line_end == std::string::npos) return false;
  int code = 0;
  if (std::sscanf(response.c_str(), "HTTP/%*s %d", &code) != 1) return false;
  status = code;
  return true;
}

static bool response_done(const std::string& response) {
  size_t body_at = response.find("\r\n\r\n");
  if (body_at == std::string::npos) return false;
  std::string body = response.substr(body_at + 4);
  JVal value;
  if (!json_parse(body, value)) return false;
  return jbool(value, "done", false);
}

static bool register_port(const Url& url, int port) {
  std::string body = "{\"port\":" + itoa10(port) + "}";
  long long delay_ms = 50;
  for (int attempt = 0; attempt < 400 && !g_stop; attempt++) {
    int status = 0;
    std::string response;
    if (http_call(url, "POST", "/register", body, status, response)) {
      if (status == 200 || status == 409) return true;
      if (status == 400 || status == 405) return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    if (delay_ms < 2000) delay_ms *= 2;
  }
  return false;
}

static void wait_for_done(const Url& url) {
  long long delay_ms = 50;
  while (!g_stop) {
    int status = 0;
    std::string response;
    if (http_call(url, "GET", "/done", std::string(), status, response)) {
      if (status == 200 && response_done(response)) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    if (delay_ms < 500) delay_ms += 25;
  }
}

}  // namespace kestrel

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  using namespace kestrel;

  struct rlimit rl;
  rl.rlim_cur = 1 << 20;
  rl.rlim_max = 1 << 20;
  setrlimit(RLIMIT_NOFILE, &rl);

  const char* env_url = std::getenv("MAKEFUN_ENV_URL");
  if (!env_url || !*env_url) {
    std::fprintf(stderr, "[forge] MAKEFUN_ENV_URL is not set: there is no env to register with\n");
    return 1;
  }
  std::string url_text = env_url;
  while (!url_text.empty() && url_text.back() == '/') url_text.pop_back();
  Url env_target;
  if (!parse_url(url_text, env_target)) {
    std::fprintf(stderr, "[forge] MAKEFUN_ENV_URL is not an http URL\n");
    return 1;
  }
  const char* token_env = std::getenv("MAKEFUN_SHOP_TOKEN");
  std::string token = token_env ? token_env : "";
  std::string login = std::getenv("MAKEFUN_FORGE_LOGIN") ? std::getenv("MAKEFUN_FORGE_LOGIN") : "";
  if (login.empty()) login = "adaiversen";

  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGPIPE, SIG_IGN);

  Store store(login, token);

  int listen_fd = create_listener(0);
  if (listen_fd < 0) {
    std::fprintf(stderr, "[forge] listen: %s\n", std::strerror(errno));
    return 1;
  }
  struct sockaddr_in bound;
  socklen_t bound_len = sizeof(bound);
  getsockname(listen_fd, (struct sockaddr*)&bound, &bound_len);
  int port = ntohs(bound.sin_port);

  // 16 warm blocking workers; each costs a 256 KiB stack and one pid.
  for (int i = 0; i < kWorkersStart; i++) {
    if (!spawn_worker(&store)) break;
    g_total.fetch_add(1, std::memory_order_relaxed);
  }

  std::printf("[forge] listening on port %d (%d warm blocking workers)\n", port,
              g_total.load(std::memory_order_relaxed));
  std::fflush(stdout);

  if (!register_port(env_target, port)) {
    std::fprintf(stderr, "[forge] register never succeeded\n");
    return 1;
  }
  std::printf("[forge] registered; waiting for the env to say done\n");
  std::fflush(stdout);

  std::thread([&env_target]() {
    wait_for_done(env_target);
    std::fflush(stdout);
    std::_Exit(0);
  }).detach();

  struct timeval tv;
  tv.tv_sec = kReapMs / 1000;
  tv.tv_usec = (kReapMs % 1000) * 1000;

  while (!g_stop) {
    int fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // A client that abandons an idle keep-alive socket is reaped by recv()'s timeout.
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    dispatch_connection(fd, &store);
  }

  g_shutdown.store(true, std::memory_order_relaxed);
  g_queue_cv.notify_all();
  std::printf("[forge] done\n");
  std::fflush(stdout);
  std::_Exit(0);
}
