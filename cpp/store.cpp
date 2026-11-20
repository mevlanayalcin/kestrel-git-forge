// Store operations: repositories, the git graph, issues, comments, pulls and JSON views.
#include <algorithm>
#include <memory>
#include <unordered_set>

#include "forge.hpp"

namespace kestrel {

const char* kBaseTime = "2025-01-01T00:00:00Z";
const char* kJsonType = "application/json; charset=utf-8";

std::string b64(const std::string& data) { return b64_encode(data); }

// ---------------------------------------------------------------- store

static std::string repo_key(const std::string& owner, const std::string& name) {
  return to_lower(owner) + "/" + to_lower(name);
}

std::shared_ptr<Repo> Store::find_repo(const std::string& owner, const std::string& name) {
  std::shared_lock<std::shared_mutex> lock(mu);
  auto it = repos.find(repo_key(owner, name));
  return it == repos.end() ? nullptr : it->second;
}

std::shared_ptr<Repo> Store::create_repo(const std::string& owner, const std::string& name,
                                         const std::string& description, bool priv) {
  std::string key = repo_key(owner, name);
  std::unique_lock<std::shared_mutex> lock(mu);
  if (repos.find(key) != repos.end()) return nullptr;
  repo_seq++;
  auto repo = std::make_shared<Repo>();
  repo->id = repo_seq;
  repo->owner = owner;
  repo->name = name;
  repo->description = description;
  repo->private_ = priv;
  repo->default_branch = "main";
  repo->created = kBaseTime;
  repo->updated = kBaseTime;
  repo->pushed = kBaseTime;
  repos[key] = repo;
  order.push_back(repo);
  return repo;
}

bool Store::delete_repo(const std::string& owner, const std::string& name) {
  std::string key = repo_key(owner, name);
  std::unique_lock<std::shared_mutex> lock(mu);
  auto it = repos.find(key);
  if (it == repos.end()) return false;
  repos.erase(it);
  for (size_t i = 0; i < order.size(); i++) {
    if (repo_key(order[i]->owner, order[i]->name) == key) {
      order.erase(order.begin() + i);
      break;
    }
  }
  return true;
}

std::vector<std::shared_ptr<Repo>> Store::list_repos(const std::string& owner) {
  std::shared_lock<std::shared_mutex> lock(mu);
  std::vector<std::shared_ptr<Repo>> out;
  out.reserve(order.size());
  for (const auto& repo : order)
    if (ieq(repo->owner, owner)) out.push_back(repo);
  return out;
}

// ---------------------------------------------------------------- git graph

bool Repo::ancestor_l(const std::string& tip, const std::string& want) const {
  if (tip == want) return true;
  if (want.size() == 40) {
    auto tip_obj = object_l(tip);
    if (tip_obj && tip_obj->type == OBJ_COMMIT && tip_obj->data.size() >= 94 &&
        std::memcmp(tip_obj->data.data(), "tree ", 5) == 0 && tip_obj->data[45] == '\n' &&
        std::memcmp(tip_obj->data.data() + 46, "parent ", 7) == 0 && tip_obj->data[93] == '\n' &&
        std::memcmp(tip_obj->data.data() + 53, want.data(), 40) == 0) {
      return true;
    }
  }
  std::unordered_map<std::string, bool> seen;
  seen[tip] = true;
  std::vector<std::string> queue;
  queue.push_back(tip);
  size_t head = 0;
  while (head < queue.size()) {
    std::string oid = queue[head++];
    auto object = object_l(oid);
    if (!object || object->type != OBJ_COMMIT) continue;
    CommitView view = parse_commit(object->data);
    for (const auto& parent : view.parents) {
      if (parent == want) return true;
      if (!seen[parent]) {
        seen[parent] = true;
        queue.push_back(parent);
      }
    }
  }
  return false;
}

void Repo::closure_l(const std::vector<std::string>& roots,
                     const std::unordered_map<std::string, bool>* shallow,
                     std::unordered_map<std::string, bool>& set) const {
  std::vector<std::string> queue;
  queue.reserve(roots.size() + 16);
  for (const auto& root : roots) {
    if (root.empty() || root == kZeroOid) continue;
    if (!set[root]) {
      set[root] = true;
      queue.push_back(root);
    }
  }
  size_t head = 0;
  while (head < queue.size()) {
    std::string oid = queue[head++];
    auto object = object_l(oid);
    if (!object) continue;
    if (object->type == OBJ_COMMIT) {
      CommitView view = parse_commit(object->data);
      if (!view.tree.empty() && !set[view.tree]) {
        set[view.tree] = true;
        queue.push_back(view.tree);
      }
      if (shallow && shallow->find(oid) != shallow->end()) continue;
      for (const auto& parent : view.parents) {
        if (!set[parent]) {
          set[parent] = true;
          queue.push_back(parent);
        }
      }
    } else if (object->type == OBJ_TREE) {
      std::vector<TreeEntry> entries;
      if (!parse_tree(object->data, entries)) continue;
      for (const auto& entry : entries) {
        if (!set[entry.oid]) {
          set[entry.oid] = true;
          queue.push_back(entry.oid);
        }
      }
    } else if (object->type == OBJ_TAG) {
      TagView view = parse_tag(object->data);
      if (!view.object.empty() && !set[view.object]) {
        set[view.object] = true;
        queue.push_back(view.object);
      }
    }
  }
}

std::vector<std::string> Repo::depth_closure_l(const std::vector<std::string>& wants, int depth,
                                               std::unordered_map<std::string, bool>& set) const {
  struct Node {
    std::string oid;
    int level;
  };
  std::unordered_map<std::string, int> seen;
  std::vector<Node> queue;
  queue.reserve(wants.size());
  for (const auto& want : wants) {
    if (want.empty() || want == kZeroOid) continue;
    if (seen.find(want) != seen.end()) continue;
    seen[want] = 1;
    queue.push_back(Node{want, 1});
  }
  std::vector<std::string> boundaries;
  size_t head = 0;
  while (head < queue.size()) {
    Node current = queue[head++];
    set[current.oid] = true;
    auto object = object_l(current.oid);
    if (!object) continue;
    if (object->type == OBJ_COMMIT) {
      CommitView view = parse_commit(object->data);
      if (!view.tree.empty()) {
        std::vector<std::string> roots{view.tree};
        closure_l(roots, nullptr, set);
      }
      if (depth > 0 && current.level >= depth) {
        if (!view.parents.empty()) boundaries.push_back(current.oid);
        continue;
      }
      for (const auto& parent : view.parents) {
        if (seen.find(parent) != seen.end()) continue;
        seen[parent] = current.level + 1;
        queue.push_back(Node{parent, current.level + 1});
      }
    } else if (object->type == OBJ_TAG) {
      TagView view = parse_tag(object->data);
      if (!view.object.empty() && seen.find(view.object) == seen.end()) {
        seen[view.object] = current.level;
        queue.push_back(Node{view.object, current.level});
      }
    }
  }
  return boundaries;
}

void Repo::flatten_tree_l(const std::string& tree_oid, const std::string& prefix,
                          std::map<std::string, TreeEntry>& out) const {
  auto object = object_l(tree_oid);
  if (!object || object->type != OBJ_TREE) return;
  std::vector<TreeEntry> entries;
  if (!parse_tree(object->data, entries)) return;
  for (const auto& entry : entries) {
    std::string path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
    if (entry.mode == "40000") {
      flatten_tree_l(entry.oid, path, out);
      continue;
    }
    out[path] = TreeEntry{entry.mode, entry.name, entry.oid};
  }
}

std::string Repo::write_tree_from_files_l(const std::map<std::string, TreeEntry>& files) {
  struct Dir {
    std::map<std::string, TreeEntry> files;
    std::map<std::string, std::unique_ptr<Dir>> dirs;
  };
  auto root = std::make_unique<Dir>();
  for (const auto& kv : files) {
    const std::string& path = kv.first;
    TreeEntry entry = kv.second;
    std::vector<std::string> segments = split_str(path, '/');
    Dir* node = root.get();
    for (size_t i = 0; i + 1 < segments.size(); i++) {
      auto it = node->dirs.find(segments[i]);
      if (it == node->dirs.end()) {
        auto child = std::make_unique<Dir>();
        Dir* raw = child.get();
        node->dirs[segments[i]] = std::move(child);
        node = raw;
      } else {
        node = it->second.get();
      }
    }
    entry.name = segments.back();
    node->files[entry.name] = entry;
  }
  std::function<std::string(Dir*)> build = [&](Dir* node) -> std::string {
    std::vector<TreeEntry> entries;
    entries.reserve(node->files.size() + node->dirs.size());
    for (const auto& kv : node->files) entries.push_back(kv.second);
    for (const auto& kv : node->dirs) {
      entries.push_back(TreeEntry{"40000", kv.first, build(kv.second.get())});
    }
    return put_object_l(OBJ_TREE, build_tree(std::move(entries)));
  };
  return build(root.get());
}

std::map<std::string, TreeEntry> Repo::commit_files_l(const std::string& commit_oid) const {
  std::map<std::string, TreeEntry> files;
  auto object = object_l(commit_oid);
  if (!object || object->type != OBJ_COMMIT) return files;
  CommitView view = parse_commit(object->data);
  flatten_tree_l(view.tree, "", files);
  return files;
}

bool Repo::resolve_ref_l(const std::string& name, std::string& oid) const {
  std::string n = name.empty() ? default_branch : name;
  if (ref_l("refs/heads/" + n, oid)) return true;
  if (ref_l("refs/tags/" + n, oid)) return true;
  if (ref_l(n, oid)) return true;
  auto object = object_l(n);
  if (object && object->type == OBJ_COMMIT) {
    oid = n;
    return true;
  }
  return false;
}

bool Repo::tree_files_at_l(const std::string& ref_or_oid, std::map<std::string, TreeEntry>& files,
                           std::string& commit_oid) const {
  std::string oid = ref_or_oid;
  if (starts_with(oid, "refs/")) {
    std::string resolved;
    if (!ref_l(oid, resolved)) return false;
    oid = resolved;
  } else {
    std::string resolved;
    if (resolve_ref_l(oid, resolved)) oid = resolved;
  }
  auto object = object_l(oid);
  if (!object) return false;
  if (object->type == OBJ_TAG) {
    oid = parse_tag(object->data).object;
    object = object_l(oid);
    if (!object) return false;
  }
  if (object->type != OBJ_COMMIT) return false;
  files = commit_files_l(oid);
  commit_oid = oid;
  return true;
}

std::vector<std::string> Repo::walk_commits_l(const std::string& tip, int limit) const {
  std::vector<std::string> out;
  if (tip.empty()) return out;
  std::unordered_map<std::string, bool> seen;
  std::vector<std::string> queue;
  queue.push_back(tip);
  size_t head = 0;
  while (head < queue.size() && (limit <= 0 || (int)out.size() < limit)) {
    std::string oid = queue[head++];
    if (seen[oid]) continue;
    seen[oid] = true;
    auto object = object_l(oid);
    if (!object || object->type != OBJ_COMMIT) continue;
    out.push_back(oid);
    CommitView view = parse_commit(object->data);
    for (const auto& parent : view.parents)
      if (!seen[parent]) queue.push_back(parent);
  }
  return out;
}

std::string Repo::merge_base_l(const std::string& a, const std::string& b) const {
  std::unordered_map<std::string, bool> ancestors;
  std::vector<std::string> queue;
  queue.push_back(a);
  size_t head = 0;
  while (head < queue.size()) {
    std::string oid = queue[head++];
    if (ancestors[oid]) continue;
    ancestors[oid] = true;
    auto object = object_l(oid);
    if (object && object->type == OBJ_COMMIT) {
      for (const auto& p : parse_commit(object->data).parents) queue.push_back(p);
    }
  }
  queue.clear();
  queue.push_back(b);
  head = 0;
  std::unordered_map<std::string, bool> seen;
  while (head < queue.size()) {
    std::string oid = queue[head++];
    if (seen[oid]) continue;
    seen[oid] = true;
    if (ancestors.find(oid) != ancestors.end()) return oid;
    auto object = object_l(oid);
    if (object && object->type == OBJ_COMMIT) {
      for (const auto& p : parse_commit(object->data).parents) queue.push_back(p);
    }
  }
  return std::string();
}

std::shared_ptr<Issue> Repo::new_issue_l(long long id, const std::string& title, const std::string& body,
                                         const std::vector<std::string>& labels_in) {
  issue_seq++;
  auto issue = std::make_shared<Issue>();
  issue->id = id;
  issue->number = issue_seq;
  issue->title = title;
  issue->body = body;
  issue->state = "open";
  issue->labels = labels_in;
  issue->created = kBaseTime;
  issue->updated = kBaseTime;
  issues.push_back(issue);
  issue_by_number[issue->number] = issue;
  return issue;
}

std::shared_ptr<Pull> Repo::new_pull_l(long long id, const std::string& title, const std::string& body,
                                       const std::string& head, const std::string& base) {
  pull_seq++;
  auto pull = std::make_shared<Pull>();
  pull->id = id;
  pull->number = pull_seq;
  pull->title = title;
  pull->body = body;
  pull->state = "open";
  pull->head_ref = head;
  pull->base_ref = base;
  pull->created = kBaseTime;
  pull->updated = kBaseTime;
  pull->mergeable = "MERGEABLE";
  pulls.push_back(pull);
  return pull;
}

static int line_count(const std::string& text) {
  if (text.empty()) return 0;
  int count = 0;
  for (char c : text)
    if (c == '\n') count++;
  if (!text.empty() && text.back() != '\n') count++;
  return count;
}

std::vector<PRFile> Repo::pull_files_l(const Pull& pull) const {
  std::map<std::string, TreeEntry> base_files, head_files;
  std::string base_tip;
  if (ref_l("refs/heads/" + pull.base_ref, base_tip)) base_files = commit_files_l(base_tip);
  auto head_object = object_l(pull.head_sha);
  if (head_object && head_object->type == OBJ_COMMIT) head_files = commit_files_l(pull.head_sha);
  std::vector<std::string> paths;
  paths.reserve(head_files.size());
  for (const auto& kv : head_files) paths.push_back(kv.first);
  std::sort(paths.begin(), paths.end());
  std::vector<PRFile> files;
  files.reserve(paths.size());
  for (const auto& path : paths) {
    const TreeEntry& head = head_files[path];
    auto base_it = base_files.find(path);
    bool existed = base_it != base_files.end();
    if (existed && base_it->second.oid == head.oid) continue;
    PRFile file;
    file.filename = path;
    file.status = existed ? "modified" : "added";
    file.sha = head.oid;
    int added = 0, deleted = 0;
    auto head_blob = object_l(head.oid);
    if (head_blob) added = line_count(head_blob->data);
    if (existed) {
      auto base_blob = object_l(base_it->second.oid);
      if (base_blob) deleted = line_count(base_blob->data);
    }
    if (added == 0) added = 1;
    file.additions = added;
    file.deletions = deleted;
    file.changes = added + deleted;
    files.push_back(std::move(file));
  }
  for (const auto& kv : base_files) {
    if (head_files.find(kv.first) == head_files.end()) {
      PRFile file;
      file.filename = kv.first;
      file.status = "removed";
      files.push_back(std::move(file));
    }
  }
  return files;
}

bool Repo::merge_conflict_l(const std::string& base_tip, const std::string& head_tip) const {
  std::string base = merge_base_l(base_tip, head_tip);
  if (base.empty()) return false;
  auto base_files = commit_files_l(base);
  auto head_files = commit_files_l(head_tip);
  auto tip_files = commit_files_l(base_tip);
  for (const auto& kv : head_files) {
    const std::string& path = kv.first;
    const TreeEntry& head_entry = kv.second;
    auto base_it = base_files.find(path);
    bool in_base = base_it != base_files.end();
    if (in_base && base_it->second.oid == head_entry.oid) continue;
    auto tip_it = tip_files.find(path);
    bool in_tip = tip_it != tip_files.end();
    if (!in_tip) {
      if (in_base) return true;
      continue;
    }
    bool tip_in_base = in_base && base_it->second.oid == tip_it->second.oid;
    if (tip_in_base) continue;
    if (tip_it->second.oid != head_entry.oid) return true;
  }
  return false;
}

bool Repo::merge_squash_l(Pull& pull, const std::string& message, const std::string& author,
                          long long timestamp, std::string& commit_out) {
  std::string base_tip;
  if (!ref_l("refs/heads/" + pull.base_ref, base_tip)) return false;
  if (merge_conflict_l(base_tip, pull.head_sha)) return false;
  auto base_files = commit_files_l(base_tip);
  auto head_files = commit_files_l(pull.head_sha);
  std::string base_of_merge = merge_base_l(base_tip, pull.head_sha);
  auto merge_base_files = commit_files_l(base_of_merge);
  for (const auto& kv : head_files) {
    auto base_it = merge_base_files.find(kv.first);
    if (base_it != merge_base_files.end() && base_it->second.oid == kv.second.oid) continue;
    base_files[kv.first] = kv.second;
  }
  for (const auto& kv : merge_base_files) {
    if (head_files.find(kv.first) == head_files.end()) base_files.erase(kv.first);
  }
  std::string tree = write_tree_from_files_l(base_files);
  std::string commit_data =
      build_commit(tree, {base_tip}, author, author, timestamp, message);
  std::string commit = put_object_l(OBJ_COMMIT, commit_data);
  set_ref_l("refs/heads/" + pull.base_ref, commit);
  pull.merged = true;
  pull.state = "closed";
  pull.merged_at = kBaseTime;
  pull.merge_sha = commit;
  pull.updated = kBaseTime;
  pull.mergeable = "UNKNOWN";
  commit_out = commit;
  return true;
}

// ---------------------------------------------------------------- JSON views

void repo_json_l(std::string& out, const Repo& repo) {
  int open = 0;
  for (const auto& issue : repo.issues)
    if (issue->state == "open") open++;
  std::string full = repo.owner + "/" + repo.name;
  std::string api = "/api/v3/repos/" + full;
  out += "{";
  out += "\"id\":"; append_uint(out, (unsigned long long)repo.id);
  out += ",\"node_id\":\"R_"; append_uint(out, (unsigned long long)repo.id); out += "\"";
  out += ",\"name\":"; append_json_string(out, repo.name);
  out += ",\"full_name\":"; append_json_string(out, full);
  out += ",\"owner\":{\"login\":"; append_json_string(out, repo.owner);
  out += ",\"id\":1000,\"type\":\"User\",\"site_admin\":false}";
  out += ",\"private\":";
  out += repo.private_ ? "true" : "false";
  out += ",\"fork\":false";
  out += ",\"description\":"; append_json_string(out, repo.description);
  out += ",\"default_branch\":"; append_json_string(out, repo.default_branch);
  out += ",\"created_at\":"; append_json_string(out, repo.created);
  out += ",\"updated_at\":"; append_json_string(out, repo.updated);
  out += ",\"pushed_at\":"; append_json_string(out, repo.pushed);
  out += ",\"has_issues\":true,\"has_projects\":false,\"has_wiki\":false,\"has_downloads\":true";
  out += ",\"open_issues_count\":"; append_uint(out, (unsigned long long)open);
  out += ",\"forks_count\":0,\"stargazers_count\":0,\"watchers_count\":0";
  out += ",\"visibility\":\"";
  out += repo.private_ ? "private" : "public";
  out += "\"";
  out += ",\"html_url\":"; append_json_string(out, "http://github.local/" + full);
  out += ",\"url\":"; append_json_string(out, api);
  out += ",\"clone_url\":"; append_json_string(out, "http://github.local/" + full + ".git");
  out += ",\"git_url\":"; append_json_string(out, "git://github.local/" + full + ".git");
  out += ",\"ssh_url\":"; append_json_string(out, "git@github.local:" + full + ".git");
  out += ",\"archived\":false,\"disabled\":false,\"allow_squash_merge\":true}";
}

std::shared_ptr<const std::string> repo_json_cached(Store&, Repo& repo) {
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    if (repo.json_cache) return repo.json_cache;
  }
  std::string body;
  {
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    repo_json_l(body, repo);
  }
  auto value = std::make_shared<const std::string>(std::move(body));
  std::unique_lock<std::shared_mutex> lock(repo.mu);
  if (!repo.json_cache) repo.json_cache = value;
  return repo.json_cache;
}

static void append_labels_json(std::string& out, const Repo& repo, const Issue& issue) {
  out.push_back('[');
  for (size_t i = 0; i < issue.labels.size(); i++) {
    if (i) out.push_back(',');
    const std::string& name = issue.labels[i];
    std::string color = "ededed";
    long long id = 0;
    auto it = repo.labels.find(name);
    if (it != repo.labels.end()) {
      color = it->second.color;
      id = it->second.id;
    }
    out += "{\"id\":";
    append_uint(out, (unsigned long long)id);
    out += ",\"name\":";
    append_json_string(out, name);
    out += ",\"color\":";
    append_json_string(out, color);
    out += ",\"default\":false}";
  }
  out.push_back(']');
}

std::string issue_compact_json_l(const Repo& repo, const Issue& issue) {
  std::string out;
  out.reserve(256);
  out += "{\"id\":";
  append_uint(out, (unsigned long long)issue.id);
  out += ",\"number\":";
  append_uint(out, (unsigned long long)issue.number);
  out += ",\"title\":";
  append_json_string(out, issue.title);
  out += ",\"body\":";
  append_json_string(out, issue.body);
  out += ",\"state\":";
  append_json_string(out, issue.state);
  out += ",\"labels\":";
  append_labels_json(out, repo, issue);
  out += ",\"user\":{\"login\":";
  append_json_string(out, repo.owner);
  out += ",\"id\":1000,\"type\":\"User\"},\"comments\":";
  append_uint(out, (unsigned long long)issue.comments.size());
  out.push_back('}');
  return out;
}

void issue_full_json_l(std::string& out, const Repo& repo, const Issue& issue) {
  out += "{\"id\":";
  append_uint(out, (unsigned long long)issue.id);
  out += ",\"number\":";
  append_uint(out, (unsigned long long)issue.number);
  out += ",\"title\":";
  append_json_string(out, issue.title);
  out += ",\"body\":";
  append_json_string(out, issue.body);
  out += ",\"state\":";
  append_json_string(out, issue.state);
  out += ",\"labels\":";
  append_labels_json(out, repo, issue);
  out += ",\"user\":{\"login\":";
  append_json_string(out, repo.owner);
  out += ",\"id\":1000,\"type\":\"User\"},\"comments\":";
  append_uint(out, (unsigned long long)issue.comments.size());
  out += ",\"created_at\":";
  append_json_string(out, issue.created);
  out += ",\"updated_at\":";
  append_json_string(out, issue.updated);
  out += ",\"closed_at\":";
  if (issue.state == "closed")
    append_json_string(out, issue.closed_at);
  else
    out += "null";
  out += ",\"html_url\":";
  append_json_string(out, "http://github.local/" + repo.owner + "/" + repo.name + "/issues/" +
                              itoa10(issue.number));
  out += ",\"url\":";
  append_json_string(out, "/api/v3/repos/" + repo.owner + "/" + repo.name + "/issues/" +
                              itoa10(issue.number));
  out.push_back('}');
}

void comment_json_l(std::string& out, const Repo& repo, const Issue& issue, const Comment& comment) {
  out += "{\"id\":";
  append_uint(out, (unsigned long long)comment.id);
  out += ",\"node_id\":\"C_";
  append_uint(out, (unsigned long long)comment.id);
  out += "\",\"body\":";
  append_json_string(out, comment.body);
  out += ",\"user\":{\"login\":";
  append_json_string(out, repo.owner);
  out += ",\"id\":1000,\"type\":\"User\"},\"created_at\":";
  append_json_string(out, comment.created);
  out += ",\"updated_at\":";
  append_json_string(out, comment.created);
  std::string base = "/api/v3/repos/" + repo.owner + "/" + repo.name + "/issues/" + itoa10(issue.number);
  out += ",\"url\":";
  append_json_string(out, base + "/comments/" + itoa10(comment.id));
  out += ",\"issue_url\":";
  append_json_string(out, base);
  out.push_back('}');
}

void pull_json_l(std::string& out, const Repo& repo, const Pull& pull) {
  std::string head_sha = pull.head_sha;
  std::string base_sha = pull.base_sha;
  std::string tip;
  if (repo.ref_l("refs/heads/" + pull.base_ref, tip)) base_sha = tip;
  std::string head_label = repo.owner + ":" + pull.head_ref;
  out += "{\"id\":";
  append_uint(out, (unsigned long long)pull.id);
  out += ",\"node_id\":\"PR_";
  append_uint(out, (unsigned long long)pull.id);
  out += "\",\"number\":";
  append_uint(out, (unsigned long long)pull.number);
  out += ",\"state\":";
  append_json_string(out, pull.state);
  out += ",\"title\":";
  append_json_string(out, pull.title);
  out += ",\"body\":";
  append_json_string(out, pull.body);
  out += ",\"user\":{\"login\":";
  append_json_string(out, repo.owner);
  out += ",\"id\":1000,\"type\":\"User\"},\"created_at\":";
  append_json_string(out, pull.created);
  out += ",\"updated_at\":";
  append_json_string(out, pull.updated);
  out += ",\"closed_at\":null,\"merged_at\":";
  if (pull.merged)
    append_json_string(out, pull.merged_at);
  else
    out += "null";
  out += ",\"merge_commit_sha\":";
  append_json_string(out, pull.merge_sha);
  out += ",\"merged\":";
  out += pull.merged ? "true" : "false";
  out += ",\"draft\":false,\"mergeable\":";
  out += pull.mergeable == "MERGEABLE" ? "true" : "false";
  out += ",\"mergeable_state\":";
  append_json_string(out, pull.mergeable);
  out += ",\"merged_by\":null,\"comments\":0,\"commits\":1,\"additions\":0,\"deletions\":0,\"changed_files\":";
  append_uint(out, (unsigned long long)repo.pull_files_l(pull).size());
  out += ",\"head\":{\"ref\":";
  append_json_string(out, pull.head_ref);
  out += ",\"label\":";
  append_json_string(out, head_label);
  out += ",\"sha\":";
  append_json_string(out, head_sha);
  out += ",\"user\":{\"login\":";
  append_json_string(out, repo.owner);
  out += ",\"id\":1000,\"type\":\"User\"}}";
  out += ",\"base\":{\"ref\":";
  append_json_string(out, pull.base_ref);
  out += ",\"label\":";
  append_json_string(out, repo.owner + ":" + pull.base_ref);
  out += ",\"sha\":";
  append_json_string(out, base_sha);
  out += ",\"user\":{\"login\":";
  append_json_string(out, repo.owner);
  out += ",\"id\":1000,\"type\":\"User\"}}";
  std::string html = "http://github.local/" + repo.owner + "/" + repo.name + "/pull/" + itoa10(pull.number);
  std::string url = "/api/v3/repos/" + repo.owner + "/" + repo.name + "/pulls/" + itoa10(pull.number);
  out += ",\"html_url\":";
  append_json_string(out, html);
  out += ",\"url\":";
  append_json_string(out, url);
  out += ",\"diff_url\":";
  append_json_string(out, html + ".diff");
  out += ",\"patch_url\":";
  append_json_string(out, html + ".patch");
  out += ",\"review_comments\":0,\"maintainer_can_modify\":true}";
}

std::string json_array_join(const std::vector<std::string>& items) {
  std::string out;
  size_t total = 2;
  for (const auto& item : items) total += item.size() + 1;
  out.reserve(total);
  out.push_back('[');
  for (size_t i = 0; i < items.size(); i++) {
    if (i) out.push_back(',');
    out += items[i];
  }
  out.push_back(']');
  return out;
}

std::string query_param(const std::string& query, const std::string& name) {
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string::npos) amp = query.size();
    std::string pair = query.substr(pos, amp - pos);
    size_t eq = pair.find('=');
    std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
    std::string value = eq == std::string::npos ? std::string() : pair.substr(eq + 1);
    if (url_decode(key, true) == name) return url_decode(value, true);
    pos = amp + 1;
  }
  return std::string();
}

void write_json_text(Store&, Response& res, int status, std::string body) {
  res.status = status;
  res.content_type = kJsonType;
  res.body = std::move(body);
}

void write_raw_text(Store& store, Response& res, int status, const std::string& body) {
  write_json_text(store, res, status, body);
}

void fail_json(Store& store, Response& res, int status, const std::string& message) {
  std::string body = "{\"message\":";
  append_json_string(body, message);
  body += ",\"documentation_url\":\"https://docs.github.com/rest\"}";
  write_json_text(store, res, status, std::move(body));
}

}  // namespace kestrel
