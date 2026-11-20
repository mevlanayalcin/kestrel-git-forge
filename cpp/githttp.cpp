// Git smart HTTP: advertisements, protocol v2 ls-refs/fetch, v0 fallback and receive-pack.
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "forge.hpp"

namespace kestrel {

static const char* kAgentName = "kestrel-forge/1.0";

// A complete, immutable HTTP/1.1 response (status line + headers + body) for a git route. The
// reactor can hand these bytes to send() with no per-request framing or copies.
static std::shared_ptr<const std::string> git_http(const char* content_type, std::string body) {
  std::string out;
  out.reserve(body.size() + 256);
  out += "HTTP/1.1 200 OK\r\nContent-Type: ";
  out += content_type;
  out += "\r\nContent-Length: ";
  append_uint(out, body.size());
  out += "\r\nCache-Control: no-cache\r\n\r\n";
  out += body;
  return std::make_shared<const std::string>(std::move(out));
}

static bool split_pkt_lines(const std::string& body, std::vector<std::string>& lines, size_t& rest) {
  size_t pos = 0;
  for (;;) {
    if (pos + 4 <= body.size() && std::memcmp(body.data() + pos, "PACK", 4) == 0) {
      rest = pos;
      return true;
    }
    if (pos + 4 > body.size()) {
      if (pos == body.size()) {
        rest = pos;
        return true;
      }
      return false;
    }
    int size = 0;
    for (int i = 0; i < 4; i++) {
      int v = hexval(body[pos + i]);
      if (v < 0) return false;
      size = (size << 4) | v;
    }
    if (size <= 2) {
      pos += 4;
      continue;
    }
    if ((size_t)size < 4 || pos + (size_t)size > body.size()) return false;
    lines.emplace_back(body.substr(pos + 4, (size_t)size - 4));
    pos += (size_t)size;
  }
}

static std::string first_line(const std::vector<std::string>& lines) {
  for (const auto& line : lines) {
    if (!line.empty()) {
      size_t end = line.size();
      while (end > 0 && line[end - 1] == '\n') end--;
      return line.substr(0, end);
    }
  }
  return std::string();
}

static std::string advertisement_v2() {
  std::string out;
  out += pkt_line("version 2\n");
  out += pkt_line("ls-refs=unborn\n");
  out += pkt_line("fetch=shallow\n");
  out += pkt_line("object-format=sha1\n");
  out += kPktFlush;
  return out;
}

static bool is_default_ls_refs(const std::vector<std::string>& args) {
  bool peel = false, symrefs = false, unborn = false;
  for (const auto& raw : args) {
    std::string arg = trim_ws(raw);
    if (arg == "peel")
      peel = true;
    else if (arg == "symrefs")
      symrefs = true;
    else if (arg == "unborn")
      unborn = true;
    else if (starts_with(arg, "agent=") || starts_with(arg, "object-format=") || arg.empty())
      continue;
    else
      return false;
  }
  return peel && symrefs && unborn;
}

// Callers hold repo.mu (shared or unique).
static std::string advertisement_v0_l(const Repo& repo, const std::string& service) {
  std::string caps = std::string("agent=") + kAgentName;
  if (service == "git-upload-pack")
    caps += " multi_ack_detailed side-band-64k thin-pack ofs-delta shallow no-progress include-tag object-format=sha1";
  else
    caps += " report-status delete-refs side-band-64k quiet ofs-delta";

  std::string buf;
  buf += pkt_line("# service=" + service + "\n");
  buf += kPktFlush;

  std::vector<std::string> names;
  names.reserve(repo.refs.size());
  for (const auto& kv : repo.refs) names.push_back(kv.first);
  std::sort(names.begin(), names.end());

  std::string head;
  auto head_it = repo.refs.find("refs/heads/" + repo.default_branch);
  if (head_it != repo.refs.end()) head = head_it->second;

  struct Line {
    std::string oid;
    std::string name;
  };
  std::vector<Line> out;
  out.reserve(names.size() + 1);
  if (!head.empty()) {
    out.push_back(Line{head, "HEAD"});
    caps += " symref=HEAD:refs/heads/" + repo.default_branch;
  }
  for (const auto& name : names) out.push_back(Line{repo.refs.at(name), name});

  std::map<std::string, std::string> peels;
  for (const auto& name : names) {
    auto object = repo.object_l(repo.refs.at(name));
    if (object && object->type == OBJ_TAG) peels[name] = parse_tag(object->data).object;
  }

  if (out.empty()) {
    buf += pkt_line(std::string(kZeroOid) + " capabilities^{}\x00" + caps + "\n");
    buf += kPktFlush;
    return buf;
  }
  for (size_t i = 0; i < out.size(); i++) {
    std::string suffix = i == 0 ? std::string(1, '\0') + caps : std::string();
    buf += pkt_line(out[i].oid + " " + out[i].name + suffix + "\n");
    auto peel_it = peels.find(out[i].name);
    if (peel_it != peels.end()) buf += pkt_line(peel_it->second + " " + out[i].name + "^{}\n");
  }
  buf += kPktFlush;
  return buf;
}

static std::shared_ptr<const std::string> cached_advertisement(Repo& repo, const std::string& service) {
  bool receive = service == "git-receive-pack";
  long long gen = 0;
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    gen = repo.refs_gen;
    if (receive) {
      if (repo.adv_recv_cache && repo.adv_recv_gen == gen) return repo.adv_recv_cache;
    } else {
      if (repo.adv_upload_cache && repo.adv_upload_gen == gen) return repo.adv_upload_cache;
    }
  }
  std::string body;
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    gen = repo.refs_gen;
    body = advertisement_v0_l(repo, service);
  }
  const char* content_type = receive ? "application/x-git-receive-pack-advertisement"
                                     : "application/x-git-upload-pack-advertisement";
  auto value = git_http(content_type, std::move(body));
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  if (repo.refs_gen != gen) return value;
  if (receive) {
    repo.adv_recv_cache = value;
    repo.adv_recv_gen = gen;
  } else {
    repo.adv_upload_cache = value;
    repo.adv_upload_gen = gen;
  }
  return value;
}

static std::string ls_refs_l(const Repo& repo, const std::vector<std::string>& args) {
  bool peel = false, symrefs = false;
  std::vector<std::string> prefixes;
  for (const auto& raw : args) {
    std::string arg = trim_ws(raw);
    if (arg == "peel")
      peel = true;
    else if (arg == "symrefs")
      symrefs = true;
    else if (arg == "unborn")
      continue;
    else if (starts_with(arg, "ref-prefix "))
      prefixes.push_back(arg.substr(11));
  }
  auto matches = [&](const std::string& name) {
    if (prefixes.empty()) return true;
    for (const auto& prefix : prefixes)
      if (starts_with(name, prefix)) return true;
    return false;
  };
  std::string buf;
  std::vector<std::string> names;
  names.reserve(repo.refs.size());
  for (const auto& kv : repo.refs)
    if (matches(kv.first)) names.push_back(kv.first);
  std::sort(names.begin(), names.end());
  auto head_it = repo.refs.find("refs/heads/" + repo.default_branch);
  if (prefixes.empty() && symrefs) {
    if (head_it != repo.refs.end()) {
      std::string line = head_it->second + " HEAD symref-target:refs/heads/" + repo.default_branch;
      buf += pkt_line(line + "\n");
    } else {
      buf += pkt_line("unborn HEAD symref-target:refs/heads/" + repo.default_branch + "\n");
    }
  }
  for (const auto& name : names) {
    std::string oid = repo.refs.at(name);
    std::string line = oid + " " + name;
    if (peel) {
      auto object = repo.object_l(oid);
      if (object && object->type == OBJ_TAG) line += " peeled:" + parse_tag(object->data).object;
    }
    buf += pkt_line(line + "\n");
  }
  buf += kPktFlush;
  return buf;
}

static std::shared_ptr<const std::string> cached_ls_refs(Repo& repo, const std::string& body,
                                                         const std::vector<std::string>& args) {
  (void)body;
  long long gen = 0;
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    gen = repo.refs_gen;
    if (repo.refs_cache && repo.refs_cache_gen == gen) return repo.refs_cache;
  }
  std::string built;
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    gen = repo.refs_gen;
    built = ls_refs_l(repo, args);
  }
  auto value = git_http("application/x-git-upload-pack-result", std::move(built));
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  if (repo.refs_gen != gen) return value;
  repo.refs_cache = value;
  repo.refs_cache_gen = gen;
  return value;
}

struct FetchResult {
  std::string body;
  std::vector<std::string> shallow;
  std::vector<std::string> object_ids;
};

static FetchResult build_fetch_v2_locked(const Repo& repo, const std::vector<std::string>& wants,
                                         const std::vector<std::string>& haves,
                                         const std::vector<std::string>& client_shallow, int depth) {
  std::unordered_map<std::string, bool> set;
  std::vector<std::string> boundaries = repo.depth_closure_l(wants, depth, set);
  if (!haves.empty()) {
    std::unordered_map<std::string, bool> subtract;
    std::unordered_map<std::string, bool> shallow_set;
    for (const auto& oid : client_shallow) shallow_set[oid] = true;
    repo.closure_l(haves, &shallow_set, subtract);
    for (auto it = set.begin(); it != set.end();) {
      if (subtract.find(it->first) != subtract.end())
        it = set.erase(it);
      else
        ++it;
    }
  }
  std::vector<std::string> oids;
  oids.reserve(set.size());
  for (const auto& kv : set) oids.push_back(kv.first);
  std::sort(oids.begin(), oids.end());
  std::vector<std::shared_ptr<const Object>> objects;
  objects.reserve(oids.size());
  for (const auto& oid : oids) {
    auto object = repo.object_l(oid);
    if (object) objects.push_back(object);
  }
  std::string pack = write_pack(objects);
  std::string buf;
  if (!boundaries.empty()) {
    buf += pkt_line("shallow-info\n");
    for (const auto& oid : boundaries) buf += pkt_line("shallow " + oid + "\n");
    buf += kPktFlush;
  }
  buf += pkt_line("packfile\n");
  const size_t max_band = 65000;
  size_t pos = 0;
  while (pos < pack.size()) {
    size_t take = std::min(max_band, pack.size() - pos);
    std::string payload;
    payload.reserve(take + 1);
    payload.push_back('\x01');
    payload.append(pack.data() + pos, take);
    buf += pkt_line(payload);
    pos += take;
  }
  buf += kPktFlush;
  return FetchResult{std::move(buf), std::move(boundaries), std::move(oids)};
}

static std::string build_fetch_v0_locked(const Repo& repo, const std::vector<std::string>& wants,
                                         const std::vector<std::string>& haves,
                                         const std::vector<std::string>& client_shallow, int depth) {
  FetchResult result = build_fetch_v2_locked(repo, wants, haves, client_shallow, depth);
  std::string buf;
  buf += pkt_line("NAK\n");
  std::vector<std::string> lines;
  size_t rest = 0;
  if (split_pkt_lines(result.body, lines, rest)) {
    bool in_pack = false;
    for (const auto& line : lines) {
      if (line == "packfile\n") {
        in_pack = true;
        continue;
      }
      if (line == "shallow-info\n") continue;
      if (!in_pack) {
        if (starts_with(line, "shallow ")) buf += pkt_line(line);
        continue;
      }
      buf += pkt_line(line);
    }
  }
  buf += kPktFlush;
  return buf;
}

// The shared, generation-keyed upload-pack HTTP response cache: the clone path serves send() only.
static std::shared_ptr<const std::string> cached_upload(
    Repo& repo, const std::string& kind, const std::string& request_body,
    const std::function<std::shared_ptr<const std::string>()>& build) {
  std::string key;
  long long gen = 0;
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    gen = repo.refs_gen;
    key = kind + ":" + itoa10(gen) + ":";
    key += request_body;
    auto it = repo.upload_cache.find(key);
    if (it != repo.upload_cache.end()) return it->second;
  }
  std::lock_guard<std::mutex> build_lock(repo.build_mu);
  std::shared_ptr<const std::string> value;
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    gen = repo.refs_gen;
    key = kind + ":" + itoa10(gen) + ":";
    key += request_body;
    auto it = repo.upload_cache.find(key);
    if (it != repo.upload_cache.end()) return it->second;
    value = build();
  }
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  if (repo.refs_gen != gen) return value;
  if (repo.upload_cache_gen != gen) {
    repo.upload_cache.clear();
    repo.upload_cache_gen = gen;
  }
  repo.upload_cache[key] = value;
  return value;
}

// ---------------------------------------------------------------- receive-pack

struct PushCommand {
  std::string old_oid;
  std::string new_oid;
  std::string ref;
};

static bool valid_oid(const std::string& oid) {
  if (oid.size() != 40) return false;
  for (char c : oid)
    if (hexval(c) < 0) return false;
  return true;
}

static std::string sideband_report(const std::string& inner) {
  std::string buf;
  const size_t max_band = 65000;
  size_t pos = 0;
  while (pos < inner.size()) {
    size_t take = std::min(max_band, inner.size() - pos);
    std::string payload;
    payload.reserve(take + 1);
    payload.push_back('\x01');
    payload.append(inner.data() + pos, take);
    buf += pkt_line(payload);
    pos += take;
  }
  buf += kPktFlush;
  return buf;
}

static bool push_wants_sideband(const std::string& body) {
  std::vector<std::string> lines;
  size_t rest = 0;
  if (!split_pkt_lines(body, lines, rest) || lines.empty()) return false;
  const std::string& first = lines[0];
  size_t nul = first.find('\0');
  if (nul == std::string::npos) return false;
  return first.find("side-band-64k", nul + 1) != std::string::npos;
}

static bool unpack_push(Repo& repo, const std::string& body, std::string& report, std::string& err) {
  std::vector<std::string> lines;
  size_t pack_at = 0;
  if (!split_pkt_lines(body, lines, pack_at)) {
    err = "bad pkt-line";
    return false;
  }
  if (lines.empty()) {
    err = "empty command list";
    return false;
  }
  std::vector<PushCommand> commands;
  commands.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); i++) {
    std::string text = lines[i];
    while (!text.empty() && text.back() == '\n') text.pop_back();
    if (i == 0) {
      size_t nul = text.find('\0');
      if (nul != std::string::npos) text.resize(nul);
    }
    size_t sp1 = text.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : text.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
      err = "bad command line";
      return false;
    }
    commands.push_back(PushCommand{text.substr(0, sp1), text.substr(sp1 + 1, sp2 - sp1 - 1),
                                   text.substr(sp2 + 1)});
  }

  std::unique_lock<std::shared_mutex> lock(repo.mu);
  std::string pack_data = body.substr(pack_at);
  std::string unpack_line = "unpack ok";
  if (!pack_data.empty()) {
    std::vector<std::pair<std::string, std::shared_ptr<const Object>>> objects;
    std::string pack_err;
    bool ok = read_pack(pack_data,
                        [&](const std::string& oid) { return repo.object_l(oid); }, objects, pack_err);
    if (!ok) {
      unpack_line = "unpack " + pack_err;
    } else {
      for (auto& kv : objects) {
        if (repo.objects.find(kv.first) == repo.objects.end()) repo.objects[kv.first] = std::move(kv.second);
      }
    }
  }
  std::string out;
  out += pkt_line(unpack_line + "\n");
  for (const auto& command : commands) {
    if (unpack_line != "unpack ok") {
      out += pkt_line("ng " + command.ref + " unpacker error\n");
      continue;
    }
    if (command.new_oid == kZeroOid) {
      repo.delete_ref_l(command.ref);
      out += pkt_line("ok " + command.ref + "\n");
      continue;
    }
    if (!valid_oid(command.new_oid)) {
      out += pkt_line("ng " + command.ref + " bad object\n");
      continue;
    }
    if (!repo.object_l(command.new_oid)) {
      out += pkt_line("ng " + command.ref + " missing object\n");
      continue;
    }
    std::string current;
    bool exists = repo.ref_l(command.ref, current);
    if (!exists) {
      repo.set_ref_l(command.ref, command.new_oid);
      out += pkt_line("ok " + command.ref + "\n");
      continue;
    }
    if (command.old_oid != current || !repo.ancestor_l(command.new_oid, current)) {
      out += pkt_line("ng " + command.ref + " non-fast-forward\n");
      continue;
    }
    if (command.new_oid != current) repo.set_ref_l(command.ref, command.new_oid);
    out += pkt_line("ok " + command.ref + "\n");
  }
  out += kPktFlush;
  report = std::move(out);
  return true;
}

static bool parse_fetch_args(const std::string& body, std::vector<std::string>& wants,
                             std::vector<std::string>& haves, std::vector<std::string>& shallow,
                             int& depth) {
  std::vector<std::string> lines;
  size_t rest = 0;
  if (!split_pkt_lines(body, lines, rest)) return false;
  for (const auto& raw : lines) {
    std::string text = trim_ws(raw);
    if (starts_with(text, "want ")) {
      auto fields = split_ws(text);
      if (fields.size() >= 2) wants.push_back(fields[1]);
    } else if (starts_with(text, "have ")) {
      auto fields = split_ws(text);
      if (fields.size() >= 2) haves.push_back(fields[1]);
    } else if (starts_with(text, "shallow ")) {
      auto fields = split_ws(text);
      if (fields.size() >= 2) shallow.push_back(fields[1]);
    } else if (starts_with(text, "deepen ")) {
      std::string value = trim_ws(text.substr(7));
      if (!value.empty()) {
        bool digits = true;
        for (char c : value)
          if (c < '0' || c > '9') digits = false;
        if (digits) depth = std::atoi(value.c_str());
      }
    }
  }
  return true;
}

void handle_git(Store& store, const Request& req, Response& res) {
  size_t index = req.path.find(".git/");
  if (index == std::string::npos) {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  std::string name = req.path.substr(0, index);
  if (!name.empty() && name[0] == '/') name.erase(0, 1);
  size_t slash = name.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= name.size()) {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  std::string owner = name.substr(0, slash);
  std::string repo_name = name.substr(slash + 1);
  if (repo_name.find('/') != std::string::npos) {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  auto repo = store.find_repo(owner, repo_name);
  if (!repo) {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  std::string suffix = req.path.substr(index + 5);
  res.with_request_id = false;
  auto write_git = [&](const char* content_type, std::string body) {
    res.status = 200;
    res.content_type = content_type;
    res.body = std::move(body);
    res.set_header("Cache-Control", "no-cache");
  };
  auto write_full = [&](std::shared_ptr<const std::string> full) {
    res.status = 200;
    res.full = std::move(full);
  };

  if (suffix == "info/refs") {
    if (req.method != "GET") {
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::string service = query_param(req.query, "service");
    if (service == "git-upload-pack") {
      if (req.header("git-protocol").find("version=2") != std::string::npos) {
        // The v2 capability advertisement is a constant: frame it once, share it forever.
        static const std::shared_ptr<const std::string> kAdvV2 =
            git_http("application/x-git-upload-pack-advertisement", advertisement_v2());
        write_full(kAdvV2);
      } else {
        write_full(cached_advertisement(*repo, service));
      }
    } else if (service == "git-receive-pack") {
      write_full(cached_advertisement(*repo, service));
    } else {
      fail_json(store, res, 404, "Not Found");
    }
    return;
  }

  if (suffix == "git-upload-pack") {
    if (req.method != "POST") {
      fail_json(store, res, 404, "Not Found");
      return;
    }
    bool v2 = req.header("git-protocol").find("version=2") != std::string::npos;
    if (!v2) {
      std::vector<std::string> wants, haves, shallow;
      int depth = 0;
      if (!parse_fetch_args(req.body, wants, haves, shallow, depth)) {
        fail_json(store, res, 400, "Bad Request");
        return;
      }
      write_full(cached_upload(*repo, "v0", req.body, [&]() {
        return git_http("application/x-git-upload-pack-result",
                        build_fetch_v0_locked(*repo, wants, haves, shallow, depth));
      }));
      return;
    }
    std::vector<std::string> lines;
    size_t rest = 0;
    if (!split_pkt_lines(req.body, lines, rest) || lines.empty()) {
      fail_json(store, res, 400, "Bad Request");
      return;
    }
    std::string command = trim_ws(first_line(lines));
    if (starts_with(command, "command=ls-refs")) {
      std::vector<std::string> args;
      for (size_t i = 1; i < lines.size(); i++) args.push_back(trim_ws(lines[i]));
      std::shared_ptr<const std::string> body;
      if (is_default_ls_refs(args))
        body = cached_ls_refs(*repo, req.body, args);
      else
        body = cached_upload(*repo, "ls", req.body, [&]() {
          return git_http("application/x-git-upload-pack-result", ls_refs_l(*repo, args));
        });
      write_full(std::move(body));
      return;
    }
    if (starts_with(command, "command=fetch")) {
      std::vector<std::string> wants, haves, client_shallow;
      int depth = 0;
      for (size_t i = 1; i < lines.size(); i++) {
        std::string text = trim_ws(lines[i]);
        if (starts_with(text, "want ")) {
          auto fields = split_ws(text);
          if (fields.size() >= 2) wants.push_back(fields[1]);
        } else if (starts_with(text, "have ")) {
          auto fields = split_ws(text);
          if (fields.size() >= 2) haves.push_back(fields[1]);
        } else if (starts_with(text, "shallow ")) {
          auto fields = split_ws(text);
          if (fields.size() >= 2) client_shallow.push_back(fields[1]);
        } else if (starts_with(text, "deepen ")) {
          std::string value = trim_ws(text.substr(7));
          if (!value.empty()) {
            bool digits = true;
            for (char c : value)
              if (c < '0' || c > '9') digits = false;
            if (digits) depth = std::atoi(value.c_str());
          }
        }
      }
      if (wants.empty()) {
        write_full(git_http("application/x-git-upload-pack-result", kPktFlush));
        return;
      }
      if (depth == 1 && wants.size() == 1 && haves.empty()) {
        // per-commit pack cache: immutable object data, keyed by the commit
        std::shared_ptr<const std::string> body;
        std::string key = "d1:" + wants[0];
        {
          std::shared_lock<std::shared_mutex> lock(repo->mu);
          auto it = repo->pack_cache.find(key);
          if (it != repo->pack_cache.end()) body = it->second;
        }
        if (!body) {
          std::lock_guard<std::mutex> build_lock(repo->build_mu);
          {
            std::shared_lock<std::shared_mutex> lock(repo->mu);
            auto it = repo->pack_cache.find(key);
            if (it != repo->pack_cache.end()) body = it->second;
          }
          if (!body) {
            FetchResult built;
            {
              std::shared_lock<std::shared_mutex> lock(repo->mu);
              built = build_fetch_v2_locked(*repo, wants, {}, {}, depth);
            }
            body = git_http("application/x-git-upload-pack-result", std::move(built.body));
            std::unique_lock<std::shared_mutex> lock(repo->mu);
            repo->pack_cache[key] = body;
          }
        }
        write_full(std::move(body));
        return;
      }
      write_full(cached_upload(*repo, "f2", req.body, [&]() {
        return git_http("application/x-git-upload-pack-result",
                        build_fetch_v2_locked(*repo, wants, haves, client_shallow, depth).body);
      }));
      return;
    }
    fail_json(store, res, 400, "Bad Request");
    return;
  }

  if (suffix == "git-receive-pack") {
    if (req.method != "POST") {
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::string report, err;
    if (!unpack_push(*repo, req.body, report, err)) {
      fail_json(store, res, 400, "Bad Request");
      return;
    }
    if (push_wants_sideband(req.body)) report = sideband_report(report);
    write_git("application/x-git-receive-pack-result", std::move(report));
    return;
  }

  fail_json(store, res, 404, "Not Found");
}

}  // namespace kestrel
