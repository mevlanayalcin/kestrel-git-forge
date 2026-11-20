// The router and the GitHub REST v3 surface.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "forge.hpp"

namespace kestrel {

static bool authorized(Store& store, const Request& req) {
  if (store.token.empty()) return true;
  std::string header = req.header("authorization");
  if (header.empty()) return false;
  std::string value = header;
  if (starts_with(value, "Bearer ")) value = value.substr(7);
  value = trim_ws(value);
  return value == store.token;
}

void route(Store& store, const Request& req, Response& res) {
  const std::string& path = req.path;
  if (path == "/healthz") {
    write_json_text(store, res, 200, "{\"ok\":true}");
    return;
  }
  if (path == "/api/graphql") {
    handle_graphql(store, req, res);
    return;
  }
  if (path == "/mcp") {
    if (!authorized(store, req)) {
      fail_json(store, res, 401, "Requires authentication");
      return;
    }
    handle_mcp(store, req, res);
    return;
  }
  if (path == "/api/v3" || starts_with(path, "/api/v3/")) {
    if (!authorized(store, req)) {
      fail_json(store, res, 401, "Requires authentication");
      return;
    }
    handle_rest(store, req, res, path.substr(7));
    return;
  }
  if (path.find(".git/") != std::string::npos) {
    handle_git(store, req, res);
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

// ---------------------------------------------------------------- helpers

static bool read_json_body(Store& store, const Request& req, Response& res, JVal& out) {
  if (trim_ws(req.body).empty()) {
    out = JVal::object();
    return true;
  }
  if (!json_parse(req.body, out) || out.type != JVal::OBJ) {
    fail_json(store, res, 400, "Problems parsing JSON");
    return false;
  }
  return true;
}

static int atoi_default(const std::string& text, int fallback) {
  if (text.empty()) return fallback;
  char* end = nullptr;
  long v = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') return fallback;
  return (int)v;
}

static bool parse_positive_int(const std::string& text, int& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  long v = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || v <= 0 || v > 2147483647L) return false;
  out = (int)v;
  return true;
}

static std::string join_from(const std::vector<std::string>& parts, size_t start, const char* sep) {
  std::string out;
  for (size_t i = start; i < parts.size(); i++) {
    if (i > start) out += sep;
    out += parts[i];
  }
  return out;
}

static std::string last_segment(const std::string& path) {
  size_t at = path.rfind('/');
  if (at == std::string::npos) return path;
  return path.substr(at + 1);
}

static void split_identity(const std::string& author, std::string& name, std::string& email) {
  std::string a = trim_ws(author);
  size_t open = a.find('<');
  size_t close_at = a.find('>');
  if (open != std::string::npos && open > 0 && close_at != std::string::npos && close_at > open) {
    name = trim_ws(a.substr(0, open));
    email = trim_ws(a.substr(open + 1, close_at - open - 1));
  } else {
    name = a;
    email.clear();
  }
}

static void trim_message(std::string& message) {
  if (!message.empty() && message.back() == '\n') message.pop_back();
}

// ---------------------------------------------------------------- top-level REST

static void handle_root(Store& store, const Request&, Response& res) {
  res.set_header("X-OAuth-Scopes", "repo, read:org");
  write_json_text(store, res, 200,
                  "{\"current_user_url\":\"/api/v3/user\",\"rate_limit_url\":\"/api/v3/rate_limit\","
                  "\"repository_url\":\"/api/v3/repos/{owner}/{repo}\","
                  "\"user_repositories_url\":\"/api/v3/users/{user}/repos{?type,page,per_page,sort}\","
                  "\"organization_url\":\"/api/v3/orgs/{org}\",\"emojis_url\":\"/api/v3/emojis\"}");
}

static void create_repo_rest(Store& store, const Request& req, Response& res) {
  JVal body;
  if (!read_json_body(store, req, res, body)) return;
  std::string name = jstr(body, "name");
  if (trim_ws(name).empty()) {
    fail_json(store, res, 422, "Repository creation failed: name is required");
    return;
  }
  auto repo = store.create_repo(store.user.login, name, jstr(body, "description"), jbool(body, "private"));
  if (!repo) {
    fail_json(store, res, 422, "Repository creation failed: a repository with that name already exists");
    return;
  }
  write_json_text(store, res, 201, *repo_json_cached(store, *repo));
}

static void list_repos_rest(Store& store, const Request& req, Response& res) {
  int page = atoi_default(query_param(req.query, "page"), 1);
  int per_page = atoi_default(query_param(req.query, "per_page"), 30);
  if (per_page <= 0) per_page = 30;
  if (per_page > 100) per_page = 100;
  if (page < 1) page = 1;
  auto repos = store.list_repos(store.user.login);
  int total = (int)repos.size();
  int start = (page - 1) * per_page;
  if (start > total) start = total;
  int end = start + per_page;
  if (end > total) end = total;
  std::vector<std::string> items;
  items.reserve(end > start ? end - start : 0);
  for (int i = start; i < end; i++) items.push_back(*repo_json_cached(store, *repos[i]));
  if (end < total) {
    std::string base = "http://" + req.host + "/api/v3/user/repos";
    res.set_header("Link", "<" + base + "?page=" + itoa10(page + 1) + "&per_page=" + itoa10(per_page) +
                               ">; rel=\"next\", <" + base + "?page=1>; rel=\"first\"");
  }
  write_json_text(store, res, 200, json_array_join(items));
}

static void handle_user_rest(Store& store, const Request& req, Response& res,
                             const std::vector<std::string>& parts) {
  if (parts.size() == 1) {
    auto repos = store.list_repos(store.user.login);
    std::string body = "{\"login\":";
    append_json_string(body, store.user.login);
    body += ",\"id\":1000,\"node_id\":\"U_1000\",\"type\":\"User\",\"name\":";
    append_json_string(body, store.user.name);
    body += ",\"email\":";
    append_json_string(body, store.user.email);
    body += ",\"site_admin\":false,\"html_url\":";
    append_json_string(body, "http://github.local/" + store.user.login);
    body += ",\"url\":\"/api/v3/user\",\"repos_url\":\"/api/v3/user/repos\",\"public_repos\":";
    append_uint(body, (unsigned long long)repos.size());
    body.push_back('}');
    write_json_text(store, res, 200, std::move(body));
    return;
  }
  if (parts[1] == "repos" && parts.size() == 2) {
    if (req.method == "POST")
      create_repo_rest(store, req, res);
    else
      list_repos_rest(store, req, res);
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

// ---------------------------------------------------------------- repo subroutes

static void branches_rest(Store& store, const Request& req, Response& res, Repo& repo,
                          const std::vector<std::string>& rest) {
  std::shared_lock<std::shared_mutex> lock(repo.mu);
  (void)req;
  if (!rest.empty()) {
    std::string name = join_from(rest, 0, "/");
    std::string oid;
    if (!repo.ref_l("refs/heads/" + name, oid)) {
      lock.unlock();
      fail_json(store, res, 404, "Branch not found");
      return;
    }
    std::string body = "{\"name\":";
    append_json_string(body, name);
    body += ",\"commit\":{\"sha\":";
    append_json_string(body, oid);
    body += "},\"protected\":false}";
    write_json_text(store, res, 200, std::move(body));
    return;
  }
  std::vector<std::string> refs;
  refs.reserve(repo.refs.size());
  for (const auto& kv : repo.refs)
    if (starts_with(kv.first, "refs/heads/")) refs.push_back(kv.first);
  std::sort(refs.begin(), refs.end());
  std::vector<std::string> items;
  items.reserve(refs.size());
  for (const auto& ref : refs) {
    std::string name = ref.substr(11);
    std::string item = "{\"name\":";
    append_json_string(item, name);
    item += ",\"commit\":{\"sha\":";
    append_json_string(item, repo.refs.at(ref));
    item += ",\"url\":";
    append_json_string(item, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/commits/" + repo.refs.at(ref));
    item += "},\"protected\":false}";
    items.push_back(std::move(item));
  }
  write_json_text(store, res, 200, json_array_join(items));
}

static void commits_rest(Store& store, const Request& req, Response& res, Repo& repo) {
  std::string sha = query_param(req.query, "sha");
  int per_page = atoi_default(query_param(req.query, "per_page"), 30);
  if (per_page <= 0 || per_page > 100) per_page = 30;
  std::shared_lock<std::shared_mutex> lock(repo.mu);
  std::string tip;
  if (sha.empty()) {
    repo.ref_l("refs/heads/" + repo.default_branch, tip);
  } else {
    repo.resolve_ref_l(sha, tip);
  }
  std::vector<std::string> commits = repo.walk_commits_l(tip, per_page);
  std::vector<std::string> items;
  items.reserve(commits.size());
  for (const auto& oid : commits) {
    auto object = repo.object_l(oid);
    if (!object) continue;
    CommitView view = parse_commit(object->data);
    trim_message(view.message);
    std::string name, email;
    split_identity(view.author, name, email);
    std::string item = "{\"sha\":";
    append_json_string(item, oid);
    item += ",\"node_id\":";
    append_json_string(item, "C_" + oid);
    item += ",\"commit\":{\"message\":";
    append_json_string(item, view.message);
    item += ",\"author\":{\"name\":";
    append_json_string(item, name);
    item += ",\"email\":";
    append_json_string(item, email);
    item += ",\"date\":";
    append_json_string(item, kBaseTime);
    item += "},\"committer\":{\"name\":";
    append_json_string(item, name);
    item += ",\"email\":";
    append_json_string(item, email);
    item += ",\"date\":";
    append_json_string(item, kBaseTime);
    item += "},\"tree\":{\"sha\":";
    append_json_string(item, view.tree);
    item += "}},\"parents\":[";
    for (size_t i = 0; i < view.parents.size(); i++) {
      if (i) item.push_back(',');
      item += "{\"sha\":";
      append_json_string(item, view.parents[i]);
      item.push_back('}');
    }
    item += "],\"url\":";
    append_json_string(item, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/commits/" + oid);
    item.push_back('}');
    items.push_back(std::move(item));
  }
  write_json_text(store, res, 200, json_array_join(items));
}

static void labels_rest(Store& store, const Request& req, Response& res, Repo& repo) {
  if (req.method != "POST") {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    std::vector<std::string> items;
    items.reserve(repo.labels.size());
    for (const auto& kv : repo.labels) {
      const Label& label = kv.second;
      std::string item = "{\"id\":";
      append_uint(item, (unsigned long long)label.id);
      item += ",\"name\":";
      append_json_string(item, label.name);
      item += ",\"color\":";
      append_json_string(item, label.color);
      item += ",\"default\":false}";
      items.push_back(std::move(item));
    }
    write_json_text(store, res, 200, json_array_join(items));
    return;
  }
  JVal body;
  if (!read_json_body(store, req, res, body)) return;
  std::string name = jstr(body, "name");
  if (name.empty()) {
    fail_json(store, res, 422, "Validation Failed: name is required");
    return;
  }
  std::string color = jstr(body, "color");
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  auto it = repo.labels.find(name);
  Label label;
  if (it == repo.labels.end()) {
    repo.label_seq++;
    label.id = store.next_comment_id();
    label.name = name;
    label.color = color.empty() ? "ededed" : color;
    repo.labels[name] = label;
  } else {
    label = it->second;
  }
  std::string out = "{\"id\":";
  append_uint(out, (unsigned long long)label.id);
  out += ",\"name\":";
  append_json_string(out, label.name);
  out += ",\"color\":";
  append_json_string(out, label.color);
  out += ",\"default\":false}";
  write_json_text(store, res, 201, std::move(out));
}

static void comments_rest(Store& store, const Request& req, Response& res, Repo& repo, int number) {
  if (req.method == "POST") {
    JVal body;
    if (!read_json_body(store, req, res, body)) return;
    std::unique_lock<std::shared_mutex> lock(repo.mu);
    auto issue = repo.find_issue_l(number);
    if (!issue) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    repo.comment_seq++;
    auto comment = std::make_shared<Comment>();
    comment->id = store.next_comment_id();
    comment->body = jstr(body, "body");
    comment->login = repo.owner;
    comment->created = kBaseTime;
    issue->comments.push_back(comment);
    issue->updated = kBaseTime;
    issue->cached.reset();
    std::string out;
    comment_json_l(out, repo, *issue, *comment);
    write_json_text(store, res, 201, std::move(out));
    return;
  }
  if (req.method == "GET") {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    auto issue = repo.find_issue_l(number);
    if (!issue) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::vector<std::string> items;
    items.reserve(issue->comments.size());
    for (const auto& comment : issue->comments) {
      std::string item;
      comment_json_l(item, repo, *issue, *comment);
      items.push_back(std::move(item));
    }
    write_json_text(store, res, 200, json_array_join(items));
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

static void issue_rest(Store& store, const Request& req, Response& res, Repo& repo, int number) {
  if (req.method == "GET") {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    auto issue = repo.find_issue_l(number);
    if (!issue) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    if (!issue->cached) issue->cached = std::make_shared<const std::string>(issue_compact_json_l(repo, *issue));
    std::string body = *issue->cached;
    write_json_text(store, res, 200, std::move(body));
    return;
  }
  if (req.method == "PATCH") {
    JVal body;
    if (!read_json_body(store, req, res, body)) return;
    std::unique_lock<std::shared_mutex> lock(repo.mu);
    auto issue = repo.find_issue_l(number);
    if (!issue) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    const JVal* v = jget(body, "title");
    if (v && v->type == JVal::STR) issue->title = v->s;
    v = jget(body, "body");
    if (v && v->type == JVal::STR) issue->body = v->s;
    v = jget(body, "state");
    if (v && v->type == JVal::STR) {
      if (v->s == "closed") {
        issue->state = "closed";
        issue->closed_at = kBaseTime;
      } else if (v->s == "open") {
        issue->state = "open";
        issue->closed_at.clear();
      }
    }
    v = jget(body, "labels");
    if (v && v->type == JVal::ARR) {
      issue->labels.clear();
      for (const auto& entry : v->arr)
        if (entry.type == JVal::STR) issue->labels.push_back(entry.s);
    }
    issue->updated = kBaseTime;
    issue->cached.reset();
    std::string out = issue_compact_json_l(repo, *issue);
    issue->cached = std::make_shared<const std::string>(out);
    write_json_text(store, res, 200, std::move(out));
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

static void create_issue_rest(Store& store, const Request& req, Response& res, Repo& repo) {
  JVal body;
  if (!read_json_body(store, req, res, body)) return;
  std::string title = jstr(body, "title");
  if (trim_ws(title).empty()) {
    fail_json(store, res, 422, "Validation Failed: title is required");
    return;
  }
  std::vector<std::string> labels;
  const JVal* lv = jget(body, "labels");
  if (lv && lv->type == JVal::ARR)
    for (const auto& entry : lv->arr)
      if (entry.type == JVal::STR) labels.push_back(entry.s);
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  for (const auto& name : labels) {
    if (repo.labels.find(name) == repo.labels.end()) {
      lock.unlock();
      fail_json(store, res, 422, "Validation Failed: label " + name + " does not exist");
      return;
    }
  }
  auto issue = repo.new_issue_l(store.next_issue_id(), title, jstr(body, "body"), labels);
  std::string out = issue_compact_json_l(repo, *issue);
  issue->cached = std::make_shared<const std::string>(out);
  write_json_text(store, res, 201, std::move(out));
}

static void issues_rest(Store& store, const Request& req, Response& res, Repo& repo,
                        const std::vector<std::string>& rest) {
  if (!rest.empty()) {
    int number = 0;
    if (!parse_positive_int(rest[0], number)) {
      fail_json(store, res, 404, "Not Found");
      return;
    }
    if (rest.size() > 1 && rest[1] == "comments") {
      comments_rest(store, req, res, repo, number);
      return;
    }
    issue_rest(store, req, res, repo, number);
    return;
  }
  if (req.method == "POST") {
    create_issue_rest(store, req, res, repo);
    return;
  }
  if (req.method != "GET") {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  std::string state = query_param(req.query, "state");
  if (state.empty()) state = "open";
  int per_page = atoi_default(query_param(req.query, "per_page"), 30);
  if (per_page <= 0) per_page = 30;
  if (per_page > 100) per_page = 100;
  int page = atoi_default(query_param(req.query, "page"), 1);
  if (page < 1) page = 1;
  std::shared_lock<std::shared_mutex> lock(repo.mu);
  std::vector<std::shared_ptr<Issue>> issues;
  issues.reserve(repo.issues.size());
  for (const auto& issue : repo.issues)
    if (state == "all" || issue->state == state) issues.push_back(issue);
  std::sort(issues.begin(), issues.end(),
            [](const std::shared_ptr<Issue>& a, const std::shared_ptr<Issue>& b) {
              return a->number < b->number;
            });
  std::string direction = query_param(req.query, "direction");
  if (direction.empty() || ieq(direction, "desc")) std::reverse(issues.begin(), issues.end());
  int total = (int)issues.size();
  int start = (page - 1) * per_page;
  if (start > total) start = total;
  int end = start + per_page;
  if (end > total) end = total;
  std::vector<std::string> items;
  items.reserve(end > start ? end - start : 0);
  for (int i = start; i < end; i++) {
    std::string item;
    issue_full_json_l(item, repo, *issues[i]);
    items.push_back(std::move(item));
  }
  write_json_text(store, res, 200, json_array_join(items));
}

// ---------------------------------------------------------------- contents

static void contents_rest(Store& store, const Request& req, Response& res, Repo& repo,
                          const std::vector<std::string>& rest) {
  std::string path = join_from(rest, 0, "/");
  if (req.method == "GET") {
    std::string ref = query_param(req.query, "ref");
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    std::map<std::string, TreeEntry> files;
    std::string commit_oid;
    if (!repo.tree_files_at_l(ref, files, commit_oid)) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    auto file_it = files.find(path);
    if (file_it != files.end()) {
      auto object = repo.object_l(file_it->second.oid);
      if (!object) {
        lock.unlock();
        fail_json(store, res, 404, "Not Found");
        return;
      }
      std::string item = "{\"type\":\"file\",\"encoding\":\"base64\",\"size\":";
      append_uint(item, (unsigned long long)object->data.size());
      item += ",\"name\":";
      append_json_string(item, last_segment(path));
      item += ",\"path\":";
      append_json_string(item, path);
      item += ",\"sha\":";
      append_json_string(item, file_it->second.oid);
      item += ",\"content\":";
      append_json_string(item, b64(object->data));
      item += ",\"url\":";
      append_json_string(item, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/contents/" + path);
      item += ",\"html_url\":";
      append_json_string(item, "http://github.local/" + repo.owner + "/" + repo.name + "/blob/" +
                                   repo.default_branch + "/" + path);
      item += ",\"git_url\":";
      append_json_string(item, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/git/blobs/" +
                                   file_it->second.oid);
      item += ",\"download_url\":";
      append_json_string(item, "http://github.local/" + repo.owner + "/" + repo.name + "/raw/" +
                                   repo.default_branch + "/" + path);
      item.push_back('}');
      write_json_text(store, res, 200, std::move(item));
      return;
    }
    std::string prefix = path;
    if (!prefix.empty() && !ends_with(prefix, "/")) prefix.push_back('/');
    std::map<std::string, bool> directories;
    std::vector<std::string> names;
    for (const auto& kv : files) {
      if (!starts_with(kv.first, prefix)) continue;
      std::string remainder = kv.first.substr(prefix.size());
      if (remainder.empty()) continue;
      size_t slash = remainder.find('/');
      if (slash != std::string::npos) {
        std::string dir = remainder.substr(0, slash);
        if (!directories[dir]) {
          directories[dir] = true;
          names.push_back(dir);
        }
        continue;
      }
      names.push_back(remainder);
    }
    if (names.empty()) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::sort(names.begin(), names.end());
    std::vector<std::string> items;
    items.reserve(names.size());
    for (const auto& name : names) {
      std::string child_path = prefix + name;
      if (directories[name]) {
        std::string item = "{\"type\":\"dir\",\"name\":";
        append_json_string(item, name);
        item += ",\"path\":";
        append_json_string(item, child_path);
        item += ",\"sha\":";
        append_json_string(item, object_id(OBJ_TREE, child_path));
        item += ",\"url\":";
        append_json_string(item, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/contents/" + child_path);
        item.push_back('}');
        items.push_back(std::move(item));
        continue;
      }
      auto entry_it = files.find(child_path);
      int size = 0;
      std::string sha;
      if (entry_it != files.end()) {
        sha = entry_it->second.oid;
        auto object = repo.object_l(sha);
        if (object) size = (int)object->data.size();
      }
      std::string item = "{\"type\":\"file\",\"name\":";
      append_json_string(item, name);
      item += ",\"path\":";
      append_json_string(item, child_path);
      item += ",\"sha\":";
      append_json_string(item, sha);
      item += ",\"size\":";
      append_uint(item, (unsigned long long)size);
      item += ",\"url\":";
      append_json_string(item, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/contents/" + child_path);
      item += ",\"download_url\":";
      append_json_string(item, "http://github.local/" + repo.owner + "/" + repo.name + "/raw/" +
                                   repo.default_branch + "/" + child_path);
      item.push_back('}');
      items.push_back(std::move(item));
    }
    write_json_text(store, res, 200, json_array_join(items));
    return;
  }

  if (req.method == "PUT") {
    JVal body;
    if (!read_json_body(store, req, res, body)) return;
    std::string data;
    if (!b64_decode(jstr(body, "content"), data)) {
      fail_json(store, res, 422, "Validation Failed: content is not valid base64");
      return;
    }
    std::string message = jstr(body, "message");
    if (message.empty()) message = "Update " + path;
    std::string branch = jstr(body, "branch");
    std::string sha = jstr(body, "sha");
    std::unique_lock<std::shared_mutex> lock(repo.mu);
    if (branch.empty()) branch = repo.default_branch;
    std::string ref_name = "refs/heads/" + branch;
    std::string parent;
    bool has_parent = repo.ref_l(ref_name, parent);
    std::map<std::string, TreeEntry> files;
    if (has_parent) files = repo.commit_files_l(parent);
    auto existing_it = files.find(path);
    bool existed = existing_it != files.end();
    if (!sha.empty()) {
      if (!existed || existing_it->second.oid != sha) {
        lock.unlock();
        fail_json(store, res, 409, "sha does not match the current file (409 Conflict)");
        return;
      }
    } else if (existed) {
      lock.unlock();
      fail_json(store, res, 422, "Invalid request: sha is required to update an existing file");
      return;
    }
    std::string blob_oid = repo.put_object_l(OBJ_BLOB, data);
    files[path] = TreeEntry{"100644", last_segment(path), blob_oid};
    std::string tree = repo.write_tree_from_files_l(files);
    std::string author = store.user.name + " <" + store.user.email + ">";
    if (message.empty() || message.back() != '\n') message.push_back('\n');
    std::vector<std::string> parents;
    if (has_parent) parents.push_back(parent);
    std::string commit = repo.put_object_l(
        OBJ_COMMIT, build_commit(tree, parents, author, author, 1735689600 + (long long)repo.objects.size(), message));
    repo.set_ref_l(ref_name, commit);
    int status = existed ? 200 : 201;
    std::string out = "{\"content\":{\"type\":\"file\",\"encoding\":\"base64\",\"size\":";
    append_uint(out, (unsigned long long)data.size());
    out += ",\"name\":";
    append_json_string(out, last_segment(path));
    out += ",\"path\":";
    append_json_string(out, path);
    out += ",\"sha\":";
    append_json_string(out, blob_oid);
    out += ",\"content\":";
    append_json_string(out, jstr(body, "content"));
    out += ",\"url\":";
    append_json_string(out, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/contents/" + path);
    out += "},\"commit\":{\"sha\":";
    append_json_string(out, commit);
    out += ",\"node_id\":";
    append_json_string(out, "C_" + commit);
    out += ",\"message\":";
    append_json_string(out, message);
    out += ",\"author\":{\"name\":";
    append_json_string(out, store.user.name);
    out += ",\"email\":";
    append_json_string(out, store.user.email);
    out += ",\"date\":";
    append_json_string(out, kBaseTime);
    out += "},\"committer\":{\"name\":";
    append_json_string(out, store.user.name);
    out += ",\"email\":";
    append_json_string(out, store.user.email);
    out += ",\"date\":";
    append_json_string(out, kBaseTime);
    out += "},\"tree\":{\"sha\":";
    append_json_string(out, tree);
    out += "},\"parents\":[";
    for (size_t i = 0; i < parents.size(); i++) {
      if (i) out.push_back(',');
      out += "{\"sha\":";
      append_json_string(out, parents[i]);
      out.push_back('}');
    }
    out += "]}}";
    write_json_text(store, res, status, std::move(out));
    return;
  }

  if (req.method == "DELETE") {
    JVal body;
    if (!read_json_body(store, req, res, body)) return;
    std::string branch = jstr(body, "branch");
    std::string sha = jstr(body, "sha");
    std::unique_lock<std::shared_mutex> lock(repo.mu);
    if (branch.empty()) branch = repo.default_branch;
    std::string ref_name = "refs/heads/" + branch;
    std::string parent;
    if (!repo.ref_l(ref_name, parent)) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    auto files = repo.commit_files_l(parent);
    auto existing_it = files.find(path);
    if (existing_it == files.end()) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    if (!sha.empty() && existing_it->second.oid != sha) {
      lock.unlock();
      fail_json(store, res, 409, "sha does not match the current file (409 Conflict)");
      return;
    }
    files.erase(path);
    std::string tree = repo.write_tree_from_files_l(files);
    std::string author = store.user.name + " <" + store.user.email + ">";
    std::string message = jstr(body, "message");
    if (message.empty()) message = "Delete " + path;
    if (message.empty() || message.back() != '\n') message.push_back('\n');
    std::string commit = repo.put_object_l(
        OBJ_COMMIT, build_commit(tree, {parent}, author, author, 1735689600 + (long long)repo.objects.size(), message));
    repo.set_ref_l(ref_name, commit);
    std::string out = "{\"content\":null,\"commit\":{\"sha\":";
    append_json_string(out, commit);
    out += ",\"node_id\":";
    append_json_string(out, "C_" + commit);
    out += ",\"message\":";
    append_json_string(out, message);
    out += ",\"author\":{\"name\":";
    append_json_string(out, store.user.name);
    out += ",\"email\":";
    append_json_string(out, store.user.email);
    out += ",\"date\":";
    append_json_string(out, kBaseTime);
    out += "},\"committer\":{\"name\":";
    append_json_string(out, store.user.name);
    out += ",\"email\":";
    append_json_string(out, store.user.email);
    out += ",\"date\":";
    append_json_string(out, kBaseTime);
    out += "},\"tree\":{\"sha\":";
    append_json_string(out, tree);
    out += "},\"parents\":[{\"sha\":";
    append_json_string(out, parent);
    out += "}]}}";
    write_json_text(store, res, 200, std::move(out));
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

// ---------------------------------------------------------------- pulls

static void create_pull_rest(Store& store, const Request& req, Response& res, Repo& repo) {
  JVal body;
  if (!read_json_body(store, req, res, body)) return;
  std::string head = jstr(body, "head");
  size_t colon = head.find(':');
  if (colon != std::string::npos) head = head.substr(colon + 1);
  std::string title = jstr(body, "title");
  if (head.empty() || title.empty()) {
    fail_json(store, res, 422, "Validation Failed: head and title are required");
    return;
  }
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  std::string head_tip;
  if (!repo.ref_l("refs/heads/" + head, head_tip)) {
    lock.unlock();
    fail_json(store, res, 422, "Validation Failed: head branch does not exist");
    return;
  }
  std::string base = jstr(body, "base");
  if (base.empty()) base = repo.default_branch;
  std::string base_tip;
  if (!repo.ref_l("refs/heads/" + base, base_tip)) {
    lock.unlock();
    fail_json(store, res, 422, "Validation Failed: base branch does not exist");
    return;
  }
  for (const auto& existing : repo.pulls) {
    if (existing->head_ref == head && existing->state == "open" && existing->base_ref == base) {
      lock.unlock();
      fail_json(store, res, 422, "Validation Failed: A pull request already exists for that head branch");
      return;
    }
  }
  auto pull = repo.new_pull_l(store.next_pull_id(), title, jstr(body, "body"), head, base);
  pull->head_sha = head_tip;
  pull->base_sha = base_tip;
  std::string out;
  pull_json_l(out, repo, *pull);
  write_json_text(store, res, 201, std::move(out));
}

static void merge_pull_rest(Store& store, const Request& req, Response& res, Repo& repo, int number) {
  if (req.method != "PUT" && req.method != "POST") {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  JVal body;
  if (!trim_ws(req.body).empty()) {
    if (!read_json_body(store, req, res, body)) return;
  }
  std::string commit_title = jstr(body, "commit_title");
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  auto pull = repo.find_pull_l(number);
  if (!pull) {
    lock.unlock();
    fail_json(store, res, 404, "Not Found");
    return;
  }
  if (pull->merged) {
    lock.unlock();
    fail_json(store, res, 405, "Pull Request is not mergeable");
    return;
  }
  std::string base_tip;
  if (!repo.ref_l("refs/heads/" + pull->base_ref, base_tip)) {
    lock.unlock();
    fail_json(store, res, 405, "Pull Request is not mergeable");
    return;
  }
  if (pull->head_sha.empty()) repo.ref_l("refs/heads/" + pull->head_ref, pull->head_sha);
  std::string message = commit_title;
  if (message.empty()) message = pull->title + " (#" + itoa10(pull->number) + ")\n";
  if (message.empty() || message.back() != '\n') message.push_back('\n');
  std::string author = store.user.name + " <" + store.user.email + ">";
  std::string commit;
  bool ok = repo.merge_squash_l(*pull, message, author,
                                1735689600 + (long long)repo.objects.size(), commit);
  lock.unlock();
  if (!ok) {
    fail_json(store, res, 405, "Pull Request is not mergeable");
    return;
  }
  std::string out = "{\"sha\":";
  append_json_string(out, commit);
  out += ",\"merged\":true,\"message\":\"Pull Request successfully merged\"}";
  write_json_text(store, res, 200, std::move(out));
}

static void pulls_rest(Store& store, const Request& req, Response& res, Repo& repo,
                       const std::vector<std::string>& rest) {
  if (rest.empty()) {
    if (req.method == "POST") {
      create_pull_rest(store, req, res, repo);
      return;
    }
    if (req.method == "GET") {
      std::string state = query_param(req.query, "state");
      if (state.empty()) state = "open";
      int per_page = atoi_default(query_param(req.query, "per_page"), 30);
      if (per_page <= 0 || per_page > 100) per_page = 30;
      std::shared_lock<std::shared_mutex> lock(repo.mu);
      std::vector<std::shared_ptr<Pull>> pulls;
      pulls.reserve(repo.pulls.size());
      for (const auto& pull : repo.pulls)
        if (state == "all" || pull->state == state) pulls.push_back(pull);
      std::sort(pulls.begin(), pulls.end(),
                [](const std::shared_ptr<Pull>& a, const std::shared_ptr<Pull>& b) {
                  return a->number < b->number;
                });
      if ((int)pulls.size() > per_page) pulls.erase(pulls.begin(), pulls.end() - per_page);
      std::vector<std::string> items;
      items.reserve(pulls.size());
      for (const auto& pull : pulls) {
        std::string item;
        pull_json_l(item, repo, *pull);
        items.push_back(std::move(item));
      }
      write_json_text(store, res, 200, json_array_join(items));
      return;
    }
    fail_json(store, res, 404, "Not Found");
    return;
  }
  int number = 0;
  if (!parse_positive_int(rest[0], number)) {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  if (rest.size() == 1) {
    if (req.method != "GET") {
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    auto pull = repo.find_pull_l(number);
    if (!pull) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::string out;
    pull_json_l(out, repo, *pull);
    write_json_text(store, res, 200, std::move(out));
    return;
  }
  if (rest[1] == "files") {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    auto pull = repo.find_pull_l(number);
    if (!pull) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::vector<PRFile> files = repo.pull_files_l(*pull);
    std::vector<std::string> items;
    items.reserve(files.size());
    for (const auto& file : files) {
      std::string item = "{\"filename\":";
      append_json_string(item, file.filename);
      item += ",\"status\":";
      append_json_string(item, file.status);
      item += ",\"additions\":";
      append_uint(item, (unsigned long long)file.additions);
      item += ",\"deletions\":";
      append_uint(item, (unsigned long long)file.deletions);
      item += ",\"changes\":";
      append_uint(item, (unsigned long long)file.changes);
      item += ",\"sha\":";
      append_json_string(item, file.sha);
      item += ",\"blob_url\":";
      append_json_string(item, "http://github.local/" + repo.owner + "/" + repo.name + "/blob/" +
                                   pull->head_sha + "/" + file.filename);
      item += ",\"raw_url\":";
      append_json_string(item, "http://github.local/" + repo.owner + "/" + repo.name + "/raw/" +
                                   pull->head_sha + "/" + file.filename);
      item.push_back('}');
      items.push_back(std::move(item));
    }
    write_json_text(store, res, 200, json_array_join(items));
    return;
  }
  if (rest[1] == "merge") {
    merge_pull_rest(store, req, res, repo, number);
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

// ---------------------------------------------------------------- git data REST

static void git_data_rest(Store& store, const Request& req, Response& res, Repo& repo,
                          const std::vector<std::string>& rest) {
  if (!rest.empty() && rest[0] == "refs") {
    if (req.method == "POST" && rest.size() == 1) {
      JVal body;
      if (!read_json_body(store, req, res, body)) return;
      std::string ref = jstr(body, "ref");
      std::string sha = jstr(body, "sha");
      if (!starts_with(ref, "refs/") || sha.empty()) {
        fail_json(store, res, 422, "Validation Failed: ref and sha are required");
        return;
      }
      std::unique_lock<std::shared_mutex> lock(repo.mu);
      if (repo.refs.find(ref) != repo.refs.end()) {
        lock.unlock();
        fail_json(store, res, 422, "Reference already exists");
        return;
      }
      if (!repo.object_l(sha)) {
        lock.unlock();
        fail_json(store, res, 422, "Object does not exist");
        return;
      }
      repo.set_ref_l(ref, sha);
      std::string out = "{\"ref\":";
      append_json_string(out, ref);
      out += ",\"node_id\":";
      append_json_string(out, "REF_" + sha);
      out += ",\"url\":";
      append_json_string(out, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/git/" + ref);
      out += ",\"object\":{\"sha\":";
      append_json_string(out, sha);
      out += ",\"type\":\"commit\",\"url\":";
      append_json_string(out, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/git/commits/" + sha);
      out += "}}";
      write_json_text(store, res, 201, std::move(out));
      return;
    }
    if (req.method == "GET" && rest.size() >= 2) {
      std::string name = join_from(rest, 1, "/");
      std::shared_lock<std::shared_mutex> lock(repo.mu);
      std::string oid;
      if (!repo.ref_l(name, oid)) {
        lock.unlock();
        fail_json(store, res, 404, "Not Found");
        return;
      }
      std::string out = "{\"ref\":";
      append_json_string(out, name);
      out += ",\"node_id\":";
      append_json_string(out, "REF_" + oid);
      out += ",\"url\":";
      append_json_string(out, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/git/" + name);
      out += ",\"object\":{\"sha\":";
      append_json_string(out, oid);
      out += ",\"type\":\"commit\",\"url\":";
      append_json_string(out, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/git/commits/" + oid);
      out += "}}";
      write_json_text(store, res, 200, std::move(out));
      return;
    }
  }
  if (rest.size() >= 2 && rest[0] == "commits" && req.method == "GET") {
    std::string oid = rest[1];
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    auto object = repo.object_l(oid);
    if (!object || object->type != OBJ_COMMIT) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    CommitView view = parse_commit(object->data);
    lock.unlock();
    std::string name, email;
    split_identity(view.author, name, email);
    std::string out = "{\"sha\":";
    append_json_string(out, oid);
    out += ",\"node_id\":";
    append_json_string(out, "C_" + oid);
    out += ",\"commit\":{\"message\":";
    append_json_string(out, view.message);
    out += ",\"author\":{\"name\":";
    append_json_string(out, name);
    out += ",\"email\":";
    append_json_string(out, email);
    out += ",\"date\":";
    append_json_string(out, kBaseTime);
    out += "},\"committer\":{\"name\":";
    append_json_string(out, name);
    out += ",\"email\":";
    append_json_string(out, email);
    out += ",\"date\":";
    append_json_string(out, kBaseTime);
    out += "},\"tree\":{\"sha\":";
    append_json_string(out, view.tree);
    out += "}},\"parents\":[";
    for (size_t i = 0; i < view.parents.size(); i++) {
      if (i) out.push_back(',');
      out += "{\"sha\":";
      append_json_string(out, view.parents[i]);
      out.push_back('}');
    }
    out += "]}";
    write_json_text(store, res, 200, std::move(out));
    return;
  }
  if (rest.size() >= 2 && rest[0] == "blobs" && req.method == "GET") {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    auto object = repo.object_l(rest[1]);
    if (!object) {
      lock.unlock();
      fail_json(store, res, 404, "Not Found");
      return;
    }
    std::string out = "{\"sha\":";
    append_json_string(out, rest[1]);
    out += ",\"size\":";
    append_uint(out, (unsigned long long)object->data.size());
    out += ",\"encoding\":\"base64\",\"content\":";
    append_json_string(out, b64(object->data));
    out.push_back('}');
    write_json_text(store, res, 200, std::move(out));
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

static void handle_repo_rest(Store& store, const Request& req, Response& res,
                             const std::vector<std::string>& parts) {
  if (parts.size() < 2) {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  std::string owner = parts[0], name = parts[1];
  auto repo = store.find_repo(owner, name);
  if (!repo) {
    fail_json(store, res, 404, "Not Found");
    return;
  }
  std::vector<std::string> rest(parts.begin() + 2, parts.end());
  if (rest.empty()) {
    if (req.method == "GET") {
      write_json_text(store, res, 200, *repo_json_cached(store, *repo));
      return;
    }
    if (req.method == "PATCH") {
      JVal body;
      if (!read_json_body(store, req, res, body)) return;
      {
        std::unique_lock<std::shared_mutex> lock(repo->mu);
        const JVal* v = jget(body, "description");
        if (v && v->type == JVal::STR) repo->description = v->s;
        v = jget(body, "private");
        if (v && v->type == JVal::BOOL) repo->private_ = v->b;
        repo->json_cache.reset();
      }
      write_json_text(store, res, 200, *repo_json_cached(store, *repo));
      return;
    }
    if (req.method == "DELETE") {
      store.delete_repo(owner, name);
      res.status = 204;
      res.body.clear();
      res.content_type.clear();
      return;
    }
    fail_json(store, res, 404, "Not Found");
    return;
  }
  if (rest[0] == "branches") {
    branches_rest(store, req, res, *repo, std::vector<std::string>(rest.begin() + 1, rest.end()));
  } else if (rest[0] == "commits") {
    commits_rest(store, req, res, *repo);
  } else if (rest[0] == "contents") {
    contents_rest(store, req, res, *repo, std::vector<std::string>(rest.begin() + 1, rest.end()));
  } else if (rest[0] == "labels") {
    labels_rest(store, req, res, *repo);
  } else if (rest[0] == "issues") {
    issues_rest(store, req, res, *repo, std::vector<std::string>(rest.begin() + 1, rest.end()));
  } else if (rest[0] == "pulls") {
    pulls_rest(store, req, res, *repo, std::vector<std::string>(rest.begin() + 1, rest.end()));
  } else if (rest[0] == "git") {
    git_data_rest(store, req, res, *repo, std::vector<std::string>(rest.begin() + 1, rest.end()));
  } else {
    fail_json(store, res, 404, "Not Found");
  }
}

void handle_rest(Store& store, const Request& req, Response& res, const std::string& rest_path) {
  std::string trimmed = rest_path;
  while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(0, 1);
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  std::vector<std::string> parts = split_str(trimmed, '/');
  if (parts.empty() || parts[0].empty()) {
    handle_root(store, req, res);
    return;
  }
  if (parts[0] == "meta") {
    write_json_text(store, res, 200,
                    "{\"installed_version\":\"1.0.0\",\"verifiable_password_authentication\":false}");
    return;
  }
  if (parts[0] == "user") {
    handle_user_rest(store, req, res, parts);
    return;
  }
  if (parts[0] == "repos") {
    handle_repo_rest(store, req, res, std::vector<std::string>(parts.begin() + 1, parts.end()));
    return;
  }
  fail_json(store, res, 404, "Not Found");
}

}  // namespace kestrel
