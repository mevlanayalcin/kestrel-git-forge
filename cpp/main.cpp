// The process: bind an ephemeral port, register it, serve blocking keep-alive workers until /done.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "forge.hpp"

namespace kestrel {

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

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

// ---------------------------------------------------------------- connection reader

struct Conn {
  int fd;
  char buf[65536];
  size_t pos = 0;
  size_t len = 0;

  bool fill() {
    for (;;) {
      if (pos < len) return true;
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n > 0) {
        pos = 0;
        len = (size_t)n;
        return true;
      }
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
  }

  bool read_line(std::string& out) {
    out.clear();
    for (;;) {
      char* start = buf + pos;
      size_t avail = len - pos;
      void* nl = std::memchr(start, '\n', avail);
      if (nl) {
        size_t index = (char*)nl - start;
        out.append(start, index);
        pos += index + 1;
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      out.append(start, avail);
      pos = len;
      if (out.size() > 16384) return false;
      if (!fill()) return false;
    }
  }

  bool read_n(size_t count, std::string& out) {
    out.clear();
    out.reserve(count);
    while (out.size() < count) {
      if (pos >= len && !fill()) return false;
      size_t take = len - pos;
      if (take > count - out.size()) take = count - out.size();
      out.append(buf + pos, take);
      pos += take;
    }
    return true;
  }
};

static bool read_request(Conn& conn, Request& req, bool& keep_alive) {
  std::string line;
  if (!conn.read_line(line)) return false;
  while (line.empty()) {
    if (!conn.read_line(line)) return false;
  }
  size_t sp1 = line.find(' ');
  size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
  req.method = line.substr(0, sp1);
  req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string version = line.substr(sp2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return false;

  req.headers.clear();
  size_t total = 0;
  for (;;) {
    if (!conn.read_line(line)) return false;
    if (line.empty()) break;
    total += line.size();
    if (total > 65536) return false;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = to_lower(trim_ws(line.substr(0, colon)));
    std::string value = trim_ws(line.substr(colon + 1));
    req.headers.emplace_back(std::move(name), std::move(value));
  }

  req.host = req.header("host");
  std::string target = req.target;
  size_t question = target.find('?');
  if (question == std::string::npos) {
    req.path = url_decode(target, false);
    req.query.clear();
  } else {
    req.path = url_decode(target.substr(0, question), false);
    req.query = target.substr(question + 1);
  }

  std::string connection = to_lower(req.header("connection"));
  if (version == "HTTP/1.0")
    keep_alive = connection.find("keep-alive") != std::string::npos;
  else
    keep_alive = connection.find("close") == std::string::npos;

  std::string expect = to_lower(req.header("expect"));
  if (expect.find("100-continue") != std::string::npos) {
    static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
    send_all(conn.fd, kContinue, sizeof(kContinue) - 1);
  }

  std::string transfer = to_lower(req.header("transfer-encoding"));
  std::string length_header = req.header("content-length");
  req.body.clear();
  if (transfer.find("chunked") != std::string::npos) {
    for (;;) {
      if (!conn.read_line(line)) return false;
      size_t semi = line.find(';');
      std::string size_text = semi == std::string::npos ? line : line.substr(0, semi);
      size_text = trim_ws(size_text);
      char* end = nullptr;
      unsigned long long size = std::strtoull(size_text.c_str(), &end, 16);
      if (end == size_text.c_str()) return false;
      if (size == 0) {
        for (;;) {
          if (!conn.read_line(line)) return false;
          if (line.empty()) break;
        }
        break;
      }
      if (size > (256ull << 20)) return false;
      std::string chunk;
      if (!conn.read_n((size_t)size, chunk)) return false;
      req.body += chunk;
      std::string crlf;
      if (!conn.read_n(2, crlf)) return false;
    }
  } else if (!length_header.empty()) {
    char* end = nullptr;
    unsigned long long size = std::strtoull(length_header.c_str(), &end, 10);
    if (end == length_header.c_str() || size > (256ull << 20)) return false;
    if (size > 0 && !conn.read_n((size_t)size, req.body)) return false;
  }
  return true;
}

static void send_response(Store& store, int fd, const Response& res, bool keep_alive) {
  thread_local std::string out;
  out.clear();
  out.reserve(res.body.size() + 512);
  out += "HTTP/1.1 ";
  out += itoa10(res.status);
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
  if (res.with_request_id) {
    out += "X-GitHub-Request-Id: ";
    out += store.next_request_id();
    out += "\r\n";
  }
  for (const auto& kv : res.headers) {
    out += kv.first;
    out += ": ";
    out += kv.second;
    out += "\r\n";
  }
  if (!keep_alive) out += "Connection: close\r\n";
  out += "\r\n";
  if (has_body) out += res.body;
  send_all(fd, out.data(), out.size());
}

static void serve_connection(Store& store, int fd) {
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  Conn conn;
  conn.fd = fd;
  while (!g_stop) {
    Request req;
    bool keep_alive = true;
    if (!read_request(conn, req, keep_alive)) break;
    Response res;
    try {
      route(store, req, res);
    } catch (...) {
      res = Response();
      fail_json(store, res, 500, "Internal Server Error");
    }
    if (res.close_after) keep_alive = false;
    send_response(store, fd, res, keep_alive);
    if (!keep_alive) break;
  }
  ::close(fd);
}

struct ConnQueue {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<int> fds;
  bool stopping = false;

  void push(int fd) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (fds.size() >= 8192) {
        ::close(fd);
        return;
      }
      fds.push_back(fd);
    }
    cv.notify_one();
  }
  int pop() {
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&] { return stopping || !fds.empty(); });
    if (fds.empty()) return -1;
    int fd = fds.front();
    fds.pop_front();
    return fd;
  }
};

static void* accept_loop(void* raw) {
  auto* pair = (std::pair<int, ConnQueue*>*)raw;
  int listen_fd = pair->first;
  ConnQueue* queue = pair->second;
  for (;;) {
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (g_stop) break;
      if (errno == EINTR) continue;
      if (errno == EMFILE || errno == ENFILE) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      break;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    queue->push(fd);
  }
  return nullptr;
}

static void* worker_main(void* raw) {
  auto* pair = (std::pair<Store*, ConnQueue*>*)raw;
  Store* store = pair->first;
  ConnQueue* queue = pair->second;
  for (;;) {
    int fd = queue->pop();
    if (fd < 0) break;
    serve_connection(*store, fd);
  }
  return nullptr;
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
  for (int attempt = 0; attempt < 400; attempt++) {
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

  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::fprintf(stderr, "[forge] socket: %s\n", std::strerror(errno));
    return 1;
  }
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;
  if (::bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    std::fprintf(stderr, "[forge] bind: %s\n", std::strerror(errno));
    return 1;
  }
  if (::listen(listen_fd, 4096) != 0) {
    std::fprintf(stderr, "[forge] listen: %s\n", std::strerror(errno));
    return 1;
  }
  socklen_t addr_len = sizeof(addr);
  getsockname(listen_fd, (struct sockaddr*)&addr, &addr_len);
  int port = ntohs(addr.sin_port);

  ConnQueue queue;
  std::pair<int, ConnQueue*> accept_args{listen_fd, &queue};
  std::pair<Store*, ConnQueue*> worker_args{&store, &queue};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  pthread_t acceptor;
  if (pthread_create(&acceptor, &attr, accept_loop, &accept_args) == 0) pthread_detach(acceptor);
  const int kWorkers = 96;
  for (int i = 0; i < kWorkers; i++) {
    pthread_t thread;
    if (pthread_create(&thread, &attr, worker_main, &worker_args) == 0) pthread_detach(thread);
  }
  pthread_attr_destroy(&attr);

  std::printf("[forge] listening on port %d\n", port);
  std::fflush(stdout);

  if (!register_port(env_target, port)) {
    std::fprintf(stderr, "[forge] register never succeeded\n");
    return 1;
  }
  std::printf("[forge] registered; waiting for the env to say done\n");
  std::fflush(stdout);
  wait_for_done(env_target);
  std::printf("[forge] done\n");
  std::fflush(stdout);
  // Workers are parked on the accept queue; process teardown must not wait on them.
  std::_Exit(0);
}
