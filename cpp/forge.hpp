// The forge's data model, wire helpers and surface handlers.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sha1.hpp"
#include "util.hpp"

namespace kestrel {

extern const char* kBaseTime;
extern const char* kZeroOid;
extern const char* kJsonType;

// ---------------------------------------------------------------- git objects

enum ObjType { OBJ_COMMIT = 1, OBJ_TREE = 2, OBJ_BLOB = 3, OBJ_TAG = 4 };

struct Object {
  int type = 0;
  std::string data;
};

inline const char* obj_type_name(int t) {
  switch (t) {
    case OBJ_COMMIT:
      return "commit";
    case OBJ_TREE:
      return "tree";
    case OBJ_BLOB:
      return "blob";
    case OBJ_TAG:
      return "tag";
  }
  return "unknown";
}

inline int obj_type_from_name(const std::string& s) {
  if (s == "commit") return OBJ_COMMIT;
  if (s == "tree") return OBJ_TREE;
  if (s == "blob") return OBJ_BLOB;
  if (s == "tag") return OBJ_TAG;
  return 0;
}

std::string object_id(int type, const std::string& data);

struct TreeEntry {
  std::string mode;
  std::string name;
  std::string oid;
};

bool parse_tree(const std::string& data, std::vector<TreeEntry>& out);
std::string build_tree(std::vector<TreeEntry> entries);
bool tree_name_less(const TreeEntry& a, const TreeEntry& b);

struct CommitView {
  std::string tree;
  std::vector<std::string> parents;
  std::string author;
  std::string committer;
  std::string message;
};
CommitView parse_commit(const std::string& data);

struct TagView {
  std::string object;
  std::string type;
  std::string tag;
};
TagView parse_tag(const std::string& data);

std::string build_commit(const std::string& tree, const std::vector<std::string>& parents,
                         const std::string& author, const std::string& committer, long long timestamp,
                         const std::string& message);

std::string write_pack(const std::vector<std::shared_ptr<const Object>>& objects);
bool read_pack(const std::string& data,
               const std::function<std::shared_ptr<const Object>(const std::string&)>& resolve,
               std::vector<std::pair<std::string, std::shared_ptr<const Object>>>& out, std::string& err);

// pack framing bits shared by githttp
std::string pkt_line(const std::string& payload);
extern const std::string kPktFlush;

// ---------------------------------------------------------------- store

struct Label {
  long long id = 0;
  std::string name;
  std::string color;
};

struct Comment {
  long long id = 0;
  std::string body;
  std::string login;
  std::string created;
};

struct Issue {
  long long id = 0;
  int number = 0;
  std::string title;
  std::string body;
  std::string state = "open";
  std::vector<std::string> labels;
  std::string created;
  std::string updated;
  std::string closed_at;
  std::vector<std::shared_ptr<Comment>> comments;
  std::shared_ptr<const std::string> cached;
};

struct PRFile {
  std::string filename;
  std::string status;
  int additions = 0;
  int deletions = 0;
  int changes = 0;
  std::string sha;
};

struct Pull {
  long long id = 0;
  int number = 0;
  std::string title;
  std::string body;
  std::string state = "open";
  std::string head_ref;
  std::string base_ref;
  std::string head_sha;
  std::string base_sha;
  bool merged = false;
  std::string merged_at;
  std::string merge_sha;
  std::string created;
  std::string updated;
  std::string mergeable = "MERGEABLE";
};

struct Repo {
  mutable std::shared_mutex mu;
  std::mutex build_mu;
  long long id = 0;
  std::string owner;
  std::string name;
  std::string description;
  bool private_ = false;
  std::string default_branch = "main";
  std::string created;
  std::string updated;
  std::string pushed;

  std::unordered_map<std::string, std::shared_ptr<const Object>> objects;
  std::map<std::string, std::string> refs;
  long long refs_gen = 0;

  std::unordered_map<std::string, Label> labels;
  std::vector<std::shared_ptr<Issue>> issues;
  int issue_seq = 0;
  std::unordered_map<int, std::shared_ptr<Issue>> issue_by_number;
  std::vector<std::shared_ptr<Pull>> pulls;
  int pull_seq = 0;
  long long comment_seq = 0;
  long long label_seq = 0;

  std::shared_ptr<const std::string> json_cache;
  std::map<std::string, std::shared_ptr<const std::string>> pack_cache;
  std::shared_ptr<const std::string> adv_upload_cache;
  std::shared_ptr<const std::string> adv_recv_cache;
  std::shared_ptr<const std::string> refs_cache;
  long long adv_upload_gen = -1;
  long long adv_recv_gen = -1;
  long long refs_cache_gen = -1;
  std::unordered_map<std::string, std::shared_ptr<const std::string>> upload_cache;
  long long upload_cache_gen = -1;

  // locked helpers: the caller holds mu in the documented mode.
  std::shared_ptr<const Object> object_l(const std::string& oid) const {
    auto it = objects.find(oid);
    return it == objects.end() ? nullptr : it->second;
  }
  bool ref_l(const std::string& name, std::string& oid) const {
    auto it = refs.find(name);
    if (it == refs.end()) return false;
    oid = it->second;
    return true;
  }
  void set_ref_l(const std::string& name, const std::string& oid) {
    refs[name] = oid;
    refs_gen++;
    pushed = kBaseTime;
  }
  void delete_ref_l(const std::string& name) {
    refs.erase(name);
    refs_gen++;
  }
  std::string put_object_l(int type, const std::string& data) {
    std::string oid = object_id(type, data);
    if (objects.find(oid) == objects.end())
      objects[oid] = std::make_shared<const Object>(Object{type, data});
    return oid;
  }
  bool ancestor_l(const std::string& tip, const std::string& want) const;
  void closure_l(const std::vector<std::string>& roots,
                 const std::unordered_map<std::string, bool>* shallow,
                 std::unordered_map<std::string, bool>& set) const;
  std::vector<std::string> depth_closure_l(const std::vector<std::string>& wants, int depth,
                                           std::unordered_map<std::string, bool>& set) const;
  void flatten_tree_l(const std::string& tree_oid, const std::string& prefix,
                      std::map<std::string, TreeEntry>& out) const;
  std::string write_tree_from_files_l(const std::map<std::string, TreeEntry>& files);
  std::map<std::string, TreeEntry> commit_files_l(const std::string& commit_oid) const;
  bool tree_files_at_l(const std::string& ref_or_oid, std::map<std::string, TreeEntry>& files,
                       std::string& commit_oid) const;
  bool resolve_ref_l(const std::string& name, std::string& oid) const;
  std::vector<std::string> walk_commits_l(const std::string& tip, int limit) const;
  std::string merge_base_l(const std::string& a, const std::string& b) const;

  std::shared_ptr<Issue> find_issue_l(int number) const {
    auto it = issue_by_number.find(number);
    return it == issue_by_number.end() ? nullptr : it->second;
  }
  std::shared_ptr<Pull> find_pull_l(int number) const {
    for (const auto& p : pulls)
      if (p->number == number) return p;
    return nullptr;
  }
  std::shared_ptr<Issue> new_issue_l(long long id, const std::string& title, const std::string& body,
                                     const std::vector<std::string>& labels);
  std::shared_ptr<Pull> new_pull_l(long long id, const std::string& title, const std::string& body,
                                   const std::string& head, const std::string& base);
  std::vector<PRFile> pull_files_l(const Pull& pull) const;
  bool merge_conflict_l(const std::string& base_tip, const std::string& head_tip) const;
  bool merge_squash_l(Pull& pull, const std::string& message, const std::string& author,
                      long long timestamp, std::string& commit_out);
};

struct User {
  long long id = 1000;
  std::string login;
  std::string name;
  std::string email;
};

struct Store {
  std::shared_mutex mu;
  std::string login;
  std::string token;
  long long repo_seq = 1000;
  std::atomic<long long> issue_id_seq{0};
  std::atomic<long long> pr_id_seq{0};
  std::atomic<long long> comment_seq{0};
  std::unordered_map<std::string, std::shared_ptr<Repo>> repos;
  std::vector<std::shared_ptr<Repo>> order;
  std::atomic<unsigned long long> request_seq{0};
  User user;

  Store(const std::string& login_in, const std::string& token_in) {
    login = login_in;
    token = token_in;
    user = User{1000, login_in, login_in, login_in + "@example.invalid"};
  }

  std::string next_request_id() { return request_id_hex(++request_seq); }
  std::shared_ptr<Repo> find_repo(const std::string& owner, const std::string& name);
  std::shared_ptr<Repo> create_repo(const std::string& owner, const std::string& name,
                                    const std::string& description, bool priv);
  bool delete_repo(const std::string& owner, const std::string& name);
  std::vector<std::shared_ptr<Repo>> list_repos(const std::string& owner);
  long long next_issue_id() { return (++issue_id_seq) + 10000; }
  long long next_pull_id() { return (++pr_id_seq) + 20000; }
  long long next_comment_id() { return (++comment_seq) + 30000; }
};

// ---------------------------------------------------------------- wire

struct Request {
  std::string method;
  std::string target;
  std::string path;
  std::string query;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string host;

  std::string header(const std::string& lower_name) const {
    for (const auto& kv : headers)
      if (kv.first == lower_name) return kv.second;
    return std::string();
  }
  bool has(const std::string& lower_name) const {
    for (const auto& kv : headers)
      if (kv.first == lower_name) return true;
    return false;
  }
};

struct Response {
  int status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool with_request_id = true;
  bool close_after = false;

  void set_header(const std::string& k, const std::string& v) { headers.emplace_back(k, v); }
};

void route(Store& store, const Request& req, Response& res);
void handle_rest(Store& store, const Request& req, Response& res, const std::string& rest_path);
void handle_git(Store& store, const Request& req, Response& res);
void handle_graphql(Store& store, const Request& req, Response& res);
void handle_mcp(Store& store, const Request& req, Response& res);

// JSON writers (callers hold the right repo lock where noted)
void repo_json_l(std::string& out, const Repo& repo);
std::shared_ptr<const std::string> repo_json_cached(Store& store, Repo& repo);
std::string issue_compact_json_l(const Repo& repo, const Issue& issue);
void issue_full_json_l(std::string& out, const Repo& repo, const Issue& issue);
void comment_json_l(std::string& out, const Repo& repo, const Issue& issue, const Comment& comment);
void pull_json_l(std::string& out, const Repo& repo, const Pull& pull);
std::string json_array_join(const std::vector<std::string>& items);

std::string query_param(const std::string& query, const std::string& name);

void fail_json(Store& store, Response& res, int status, const std::string& message);
void write_json_text(Store& store, Response& res, int status, std::string body);
void write_raw_text(Store& store, Response& res, int status, const std::string& body);

// base64 helper mirroring the suite's API surface
std::string b64(const std::string& data);

}  // namespace kestrel
