// GitHub GraphQL v4: a small document parser and the schema surface gh reads.
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "forge.hpp"

namespace kestrel {

namespace {

// ---------------------------------------------------------------- AST

struct GField;
struct GFrag;

struct GSelect {
  std::shared_ptr<GField> field;
  std::shared_ptr<GFrag> frag;
};

struct GVal {
  enum Kind { STR, INT, FLT, BOOL, NUL, ENUM, VAR, LIST, OBJ } kind = NUL;
  std::string str;
  long long num = 0;
  double dbl = 0;
  bool flag = false;
  std::vector<GVal> list;
  std::vector<std::pair<std::string, GVal>> obj;
};

struct GField {
  std::string alias;
  std::string name;
  std::vector<std::pair<std::string, GVal>> args;
  std::vector<GSelect> selection;
};

struct GFrag {
  std::string type_name;
  std::vector<GSelect> selection;
};

struct GOperation {
  std::string kind = "query";
  std::string name;
  std::vector<GSelect> selection;
  bool failed = false;
};

struct GqlParser {
  const std::string& src;
  size_t pos = 0;

  explicit GqlParser(const std::string& s) : src(s) {}

  void skip_space() {
    while (pos < src.size()) {
      char c = src[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') {
        pos++;
        continue;
      }
      if (c == '#') {
        while (pos < src.size() && src[pos] != '\n') pos++;
        continue;
      }
      break;
    }
  }
  char peek() {
    skip_space();
    if (pos >= src.size()) return 0;
    return src[pos];
  }
  std::string name() {
    skip_space();
    size_t start = pos;
    while (pos < src.size()) {
      char c = src[pos];
      if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (pos > start && c >= '0' && c <= '9')) {
        pos++;
        continue;
      }
      break;
    }
    return src.substr(start, pos - start);
  }
  bool parse_string(std::string& out) {
    skip_space();
    if (pos >= src.size() || src[pos] != '"') return false;
    pos++;
    out.clear();
    while (pos < src.size()) {
      char c = src[pos++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos >= src.size()) break;
      char esc = src[pos++];
      switch (esc) {
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'u':
          if (pos + 4 <= src.size()) {
            int cp = 0;
            bool good = true;
            for (int i = 0; i < 4; i++) {
              int v = hexval(src[pos + i]);
              if (v < 0) good = false;
              cp = cp * 16 + (v < 0 ? 0 : v);
            }
            if (good) {
              if (cp < 0x80) {
                out.push_back((char)cp);
              } else if (cp < 0x800) {
                out.push_back((char)(0xC0 | (cp >> 6)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
              } else {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
              }
            }
            pos += 4;
          }
          break;
        default:
          out.push_back(esc);
      }
    }
    return false;
  }

  GVal parse_value() {
    skip_space();
    GVal value;
    if (pos >= src.size()) return value;
    char c = src[pos];
    if (c == '$') {
      pos++;
      value.kind = GVal::VAR;
      value.str = name();
      return value;
    }
    if (c == '[') {
      pos++;
      value.kind = GVal::LIST;
      while (peek() != ']' && pos < src.size()) {
        value.list.push_back(parse_value());
        skip_space();
        if (pos < src.size() && src[pos] == ',') pos++;
      }
      if (pos < src.size()) pos++;
      return value;
    }
    if (c == '{') {
      pos++;
      value.kind = GVal::OBJ;
      while (peek() != '}' && pos < src.size()) {
        std::string key = name();
        if (peek() == ':') pos++;
        value.obj.emplace_back(key, parse_value());
        skip_space();
        if (pos < src.size() && src[pos] == ',') pos++;
      }
      if (pos < src.size()) pos++;
      return value;
    }
    if (c == '"') {
      value.kind = GVal::STR;
      parse_string(value.str);
      return value;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = pos;
      if (c == '-') pos++;
      bool is_float = false;
      while (pos < src.size()) {
        char d = src[pos];
        if (d >= '0' && d <= '9') {
          pos++;
          continue;
        }
        if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
          is_float = true;
          pos++;
          continue;
        }
        break;
      }
      std::string text = src.substr(start, pos - start);
      if (is_float) {
        value.kind = GVal::FLT;
        value.dbl = std::strtod(text.c_str(), nullptr);
        value.num = (long long)value.dbl;
      } else {
        value.kind = GVal::INT;
        value.num = std::strtoll(text.c_str(), nullptr, 10);
      }
      return value;
    }
    std::string word = name();
    if (word == "true") {
      value.kind = GVal::BOOL;
      value.flag = true;
    } else if (word == "false") {
      value.kind = GVal::BOOL;
      value.flag = false;
    } else if (word == "null") {
      value.kind = GVal::NUL;
    } else {
      value.kind = GVal::ENUM;
      value.str = word;
    }
    return value;
  }

  std::vector<GSelect> parse_selection_set() {
    std::vector<GSelect> out;
    if (peek() != '{') return out;
    pos++;
    for (;;) {
      skip_space();
      if (pos >= src.size() || src[pos] == '}') {
        if (pos < src.size()) pos++;
        return out;
      }
      if (src.compare(pos, 3, "...") == 0) {
        pos += 3;
        GSelect entry;
        if (peek() == '@') {
          while (pos < src.size() && src[pos] != ' ' && src[pos] != '\n' && src[pos] != '{') pos++;
        }
        if (peek() != '{') {
          std::string word = name();
          if (word != "on") continue;
          entry.frag = std::make_shared<GFrag>();
          entry.frag->type_name = name();
          entry.frag->selection = parse_selection_set();
          out.push_back(std::move(entry));
          continue;
        }
        entry.frag = std::make_shared<GFrag>();
        entry.frag->selection = parse_selection_set();
        out.push_back(std::move(entry));
        continue;
      }
      auto field = std::make_shared<GField>();
      std::string first = name();
      if (first.empty()) {
        pos++;
        continue;
      }
      skip_space();
      if (pos < src.size() && src[pos] == ':') {
        pos++;
        field->alias = first;
        field->name = name();
      } else {
        field->name = first;
        field->alias = first;
      }
      if (peek() == '(') {
        pos++;
        for (;;) {
          skip_space();
          if (pos >= src.size() || src[pos] == ')') {
            if (pos < src.size()) pos++;
            break;
          }
          std::string key = name();
          if (key.empty()) {
            pos++;
            continue;
          }
          if (peek() == ':') pos++;
          field->args.emplace_back(key, parse_value());
          skip_space();
          if (pos < src.size() && src[pos] == ',') pos++;
        }
      }
      while (peek() == '@') {
        while (pos < src.size() && src[pos] != ' ' && src[pos] != '\n' && src[pos] != '{' &&
               src[pos] != ')')
          pos++;
        if (peek() == '(') {
          int depth = 0;
          while (pos < src.size()) {
            if (src[pos] == '(') depth++;
            if (src[pos] == ')') {
              depth--;
              if (depth == 0) {
                pos++;
                break;
              }
            }
            pos++;
          }
        }
      }
      field->selection = parse_selection_set();
      GSelect entry;
      entry.field = std::move(field);
      out.push_back(std::move(entry));
    }
  }
};

GOperation parse_document(const std::string& src) {
  GqlParser parser(src);
  parser.skip_space();
  GOperation op;
  std::string word = parser.name();
  if (word == "query") {
    op.kind = "query";
  } else if (word == "mutation") {
    op.kind = "mutation";
  } else if (word == "subscription") {
    op.failed = true;
    return op;
  } else {
    parser.pos = 0;
  }
  if (word == "query" || word == "mutation") {
    parser.skip_space();
    if (parser.peek() != '(' && parser.peek() != '{') op.name = parser.name();
    if (parser.peek() == '(') {
      int depth = 0;
      while (parser.pos < src.size()) {
        if (src[parser.pos] == '(') depth++;
        if (src[parser.pos] == ')') {
          depth--;
          if (depth == 0) {
            parser.pos++;
            break;
          }
        }
        parser.pos++;
      }
    }
  }
  op.selection = parser.parse_selection_set();
  return op;
}

// ---------------------------------------------------------------- schema tables

const std::vector<std::string>& type_fields(const std::string& name) {
  static const std::vector<std::string> empty;
  static const std::map<std::string, std::vector<std::string>> table = {
      {"User", {"id", "login", "name", "email", "databaseId", "avatarUrl", "url"}},
      {"Repository",
       {"id", "databaseId", "name", "nameWithOwner", "owner", "defaultBranchRef",
        "viewerPermission", "hasIssuesEnabled", "issue", "issues", "pullRequests", "pullRequest",
        "description", "isPrivate", "createdAt", "updatedAt", "url", "stargazerCount", "forkCount"}},
      {"Issue",
       {"id", "number", "title", "state", "body", "createdAt", "updatedAt", "closed", "closedAt",
        "url", "comments", "author", "labels"}},
      {"PullRequest",
       {"id", "number", "state", "merged", "headRefName", "baseRefName", "mergeable", "files", "title",
        "body", "createdAt", "updatedAt", "isDraft", "mergedAt", "mergeCommit", "changedFiles", "url",
        "headRefOid", "baseRefOid"}},
      {"Ref", {"name", "prefix", "target"}},
      {"IssueConnection", {"nodes", "pageInfo", "totalCount", "edges"}},
      {"PullRequestConnection", {"nodes", "pageInfo", "totalCount", "edges"}},
      {"PullRequestChangedFileConnection", {"nodes", "pageInfo", "totalCount", "edges"}},
      {"PageInfo", {"hasNextPage", "hasPreviousPage", "startCursor", "endCursor"}},
      {"PullRequestChangedFile", {"path", "additions", "deletions", "changeType"}},
  };
  auto it = table.find(name);
  return it == table.end() ? empty : it->second;
}

const std::string& type_kind(const std::string& name) {
  static const std::string empty;
  static const std::map<std::string, std::string> table = {
      {"User", "OBJECT"},          {"Repository", "OBJECT"},
      {"Issue", "OBJECT"},         {"PullRequest", "OBJECT"},
      {"Ref", "OBJECT"},           {"IssueConnection", "OBJECT"},
      {"PullRequestConnection", "OBJECT"}, {"PullRequestChangedFileConnection", "OBJECT"},
      {"PageInfo", "OBJECT"},      {"PullRequestChangedFile", "OBJECT"},
      {"SearchType", "ENUM"},      {"IssueState", "ENUM"},
      {"PullRequestState", "ENUM"}, {"MergeableState", "ENUM"},
      {"OrderDirection", "ENUM"},  {"IssueOrderField", "ENUM"},
  };
  auto it = table.find(name);
  return it == table.end() ? empty : it->second;
}

const std::vector<std::string>& enum_values(const std::string& name) {
  static const std::vector<std::string> empty;
  static const std::map<std::string, std::vector<std::string>> table = {
      {"SearchType", {"ISSUE", "USER", "REPOSITORY", "PULL_REQUEST", "DISCUSSION"}},
      {"IssueState", {"OPEN", "CLOSED"}},
      {"PullRequestState", {"OPEN", "CLOSED", "MERGED"}},
      {"MergeableState", {"MERGEABLE", "CONFLICTING", "UNKNOWN"}},
      {"OrderDirection", {"ASC", "DESC"}},
      {"IssueOrderField", {"CREATED_AT", "UPDATED_AT", "COMMENTS"}},
  };
  auto it = table.find(name);
  return it == table.end() ? empty : it->second;
}

std::string cursor_for(int index) { return b64_encode("cursor:" + itoa10(index)); }

int index_from_cursor(const std::string& cursor) {
  if (cursor.empty()) return 0;
  std::string raw;
  if (!b64_decode(cursor, raw)) return 0;
  if (starts_with(raw, "cursor:")) {
    const char* text = raw.c_str() + 7;
    char* end = nullptr;
    long v = std::strtol(text, &end, 10);
    if (end != text && *end == '\0' && v >= 0) return (int)v;
  }
  return 0;
}

struct Page {
  std::vector<JVal> items;
  bool has_next = false;
  std::string end_cursor;
};

Page paginate(std::vector<JVal> items, int first, const std::string& after) {
  if (first <= 0) first = (int)items.size();
  int start = index_from_cursor(after);
  if (start > (int)items.size()) start = (int)items.size();
  int end = start + first;
  if (end > (int)items.size()) end = (int)items.size();
  Page page;
  for (int i = start; i < end; i++) page.items.push_back(std::move(items[i]));
  page.has_next = end < (int)items.size();
  page.end_cursor = cursor_for(end);
  return page;
}

// ---------------------------------------------------------------- executor

struct GqlExec {
  Store* store;
  std::vector<std::pair<std::string, GVal>> vars;
  std::vector<JVal> errors;

  const GVal* var(const std::string& name) const {
    for (const auto& kv : vars)
      if (kv.first == name) return &kv.second;
    return nullptr;
  }

  JVal resolve(const GVal& value) {
    switch (value.kind) {
      case GVal::STR:
        return JVal::string(value.str);
      case GVal::INT:
        return JVal::integer(value.num);
      case GVal::FLT:
        return JVal::integer(value.num);
      case GVal::BOOL:
        return JVal::boolean(value.flag);
      case GVal::NUL:
        return JVal::null();
      case GVal::ENUM:
        return JVal::string(value.str);
      case GVal::VAR: {
        const GVal* actual = var(value.str);
        return actual ? resolve(*actual) : JVal::null();
      }
      case GVal::LIST: {
        JVal out = JVal::array();
        for (const auto& entry : value.list) out.push(resolve(entry));
        return out;
      }
      case GVal::OBJ: {
        JVal out = JVal::object();
        for (const auto& kv : value.obj) out.set(kv.first, resolve(kv.second));
        return out;
      }
    }
    return JVal::null();
  }

  const GVal* arg(const GField& field, const std::string& name) const {
    for (const auto& kv : field.args)
      if (kv.first == name) return &kv.second;
    return nullptr;
  }
  std::string arg_string(const GField& field, const std::string& name) {
    const GVal* v = arg(field, name);
    if (!v) return std::string();
    JVal r = resolve(*v);
    return r.type == JVal::STR ? r.s : std::string();
  }
  int arg_int(const GField& field, const std::string& name, int fallback) {
    const GVal* v = arg(field, name);
    if (!v) return fallback;
    JVal r = resolve(*v);
    if (r.type == JVal::INT) return (int)r.i;
    if (r.type == JVal::DBL) return (int)r.d;
    return fallback;
  }
  JVal arg_value(const GField& field, const std::string& name) {
    const GVal* v = arg(field, name);
    if (!v) return JVal::null();
    return resolve(*v);
  }

  void fail_field(const std::string& type_name, const std::string& field_name) {
    JVal error = JVal::object();
    error.set("message", JVal::string("Field '" + field_name + "' doesn't exist on type '" + type_name + "'"));
    JVal path = JVal::array();
    path.push(JVal::string(field_name));
    error.set("path", std::move(path));
    JVal location = JVal::object();
    location.set("line", JVal::integer(1));
    location.set("column", JVal::integer(1));
    JVal locations = JVal::array();
    locations.push(std::move(location));
    error.set("locations", std::move(locations));
    errors.push_back(std::move(error));
  }

  std::string global_id(const std::string& kind, long long id) {
    return b64_encode(kind + ":" + itoa10(id));
  }
  bool parse_global_id(const std::string& text, std::string& kind, long long& id) {
    std::string raw;
    if (!b64_decode(text, raw)) return false;
    size_t colon = raw.find(':');
    if (colon == std::string::npos) return false;
    kind = raw.substr(0, colon);
    const char* start = raw.c_str() + colon + 1;
    char* end = nullptr;
    long long value = std::strtoll(start, &end, 10);
    if (end == start || *end != '\0') return false;
    id = value;
    return true;
  }

  using Resolver = std::function<bool(const std::string&, const GField&, JVal&)>;

  JVal object(const std::string& type_name, const std::vector<GSelect>& selection,
              const Resolver& resolve_field) {
    JVal out = JVal::object();
    for (const auto& entry : selection) {
      if (entry.frag) {
        if (!entry.frag->type_name.empty() && entry.frag->type_name != type_name) continue;
        JVal inner = object(type_name, entry.frag->selection, resolve_field);
        for (auto& kv : inner.obj) out.set(kv.first, kv.second);
        continue;
      }
      if (!entry.field) continue;
      JVal value;
      bool known = resolve_field(entry.field->name, *entry.field, value);
      if (!known) {
        fail_field(type_name, entry.field->name);
        out.set(entry.field->alias, JVal::null());
      } else {
        out.set(entry.field->alias, std::move(value));
      }
    }
    return out;
  }

  std::shared_ptr<Repo> repo_by_id(long long id) {
    std::vector<std::shared_ptr<Repo>> repos;
    {
      std::shared_lock<std::shared_mutex> lock(store->mu);
      repos = store->order;
    }
    for (const auto& repo : repos)
      if (repo->id == id) return repo;
    return nullptr;
  }

  std::pair<std::shared_ptr<Repo>, std::shared_ptr<Issue>> issue_by_id(long long id) {
    std::vector<std::shared_ptr<Repo>> repos;
    {
      std::shared_lock<std::shared_mutex> lock(store->mu);
      repos = store->order;
    }
    for (const auto& repo : repos) {
      std::shared_lock<std::shared_mutex> lock(repo->mu);
      for (const auto& issue : repo->issues)
        if (issue->id == id) return {repo, issue};
    }
    return {nullptr, nullptr};
  }

  std::pair<std::shared_ptr<Repo>, std::shared_ptr<Pull>> pull_by_id(long long id) {
    std::vector<std::shared_ptr<Repo>> repos;
    {
      std::shared_lock<std::shared_mutex> lock(store->mu);
      repos = store->order;
    }
    for (const auto& repo : repos) {
      std::shared_lock<std::shared_mutex> lock(repo->mu);
      for (const auto& pull : repo->pulls)
        if (pull->id == id) return {repo, pull};
    }
    return {nullptr, nullptr};
  }

  JVal select_user(const std::string& login, const std::vector<GSelect>& selection) {
    return object("User", selection, [&](const std::string& name, const GField&, JVal& out) {
      if (name == "login" || name == "name") {
        out = JVal::string(login);
        return true;
      }
      if (name == "id") {
        out = JVal::string(global_id("user", store->user.id));
        return true;
      }
      if (name == "databaseId") {
        out = JVal::integer(store->user.id);
        return true;
      }
      if (name == "email") {
        out = JVal::string(login + "@example.invalid");
        return true;
      }
      if (name == "url") {
        out = JVal::string("http://github.local/" + login);
        return true;
      }
      if (name == "avatarUrl") {
        out = JVal::string("http://github.local/avatar.png");
        return true;
      }
      return false;
    });
  }

  JVal select_repository(const Repo& repo, const std::vector<GSelect>& selection) {
    return object("Repository", selection, [&](const std::string& name, const GField& field, JVal& out) {
      return repository_field(repo, name, field, out);
    });
  }

  JVal select_repository_node(const Repo& repo, const std::vector<GSelect>& selection) {
    JVal out = select_repository(repo, selection);
    out.set("id", JVal::string(global_id("repo", repo.id)));
    return out;
  }

  bool repository_field(const Repo& repo, const std::string& name, const GField& field, JVal& out) {
    if (name == "id") {
      out = JVal::string(global_id("repo", repo.id));
      return true;
    }
    if (name == "databaseId") {
      out = JVal::integer(repo.id);
      return true;
    }
    if (name == "name") {
      out = JVal::string(repo.name);
      return true;
    }
    if (name == "nameWithOwner") {
      out = JVal::string(repo.owner + "/" + repo.name);
      return true;
    }
    if (name == "owner") {
      out = select_user(repo.owner, field.selection);
      return true;
    }
    if (name == "defaultBranchRef") {
      std::string tip;
      {
        std::shared_lock<std::shared_mutex> lock(repo.mu);
        if (!repo.ref_l("refs/heads/" + repo.default_branch, tip)) {
          out = JVal::null();
          return true;
        }
      }
      std::string branch = repo.default_branch;
      out = object("Ref", field.selection, [&](const std::string& inner, const GField&, JVal& value) {
        if (inner == "name") {
          value = JVal::string(branch);
          return true;
        }
        if (inner == "prefix") {
          value = JVal::string("refs/heads/");
          return true;
        }
        if (inner == "target") {
          JVal target = JVal::object();
          target.set("oid", JVal::string(tip));
          value = std::move(target);
          return true;
        }
        return false;
      });
      return true;
    }
    if (name == "viewerPermission") {
      out = JVal::string("ADMIN");
      return true;
    }
    if (name == "hasIssuesEnabled") {
      out = JVal::boolean(true);
      return true;
    }
    if (name == "description") {
      std::shared_lock<std::shared_mutex> lock(repo.mu);
      out = JVal::string(repo.description);
      return true;
    }
    if (name == "isPrivate") {
      out = JVal::boolean(repo.private_);
      return true;
    }
    if (name == "createdAt" || name == "updatedAt") {
      out = JVal::string(kBaseTime);
      return true;
    }
    if (name == "url") {
      out = JVal::string("http://github.local/" + repo.owner + "/" + repo.name);
      return true;
    }
    if (name == "stargazerCount" || name == "forkCount") {
      out = JVal::integer(0);
      return true;
    }
    if (name == "issue") {
      int number = arg_int(field, "number", 0);
      std::shared_lock<std::shared_mutex> lock(repo.mu);
      auto issue = repo.find_issue_l(number);
      if (!issue) {
        out = JVal::null();
        return true;
      }
      out = select_issue(repo, *issue, field.selection);
      return true;
    }
    if (name == "issueOrPullRequest") {
      out = JVal::null();
      return true;
    }
    if (name == "issues") {
      out = issue_connection(repo, field);
      return true;
    }
    if (name == "pullRequests") {
      out = pull_connection(repo, field);
      return true;
    }
    if (name == "pullRequest") {
      int number = arg_int(field, "number", 0);
      std::shared_lock<std::shared_mutex> lock(repo.mu);
      auto pull = repo.find_pull_l(number);
      if (!pull) {
        out = JVal::null();
        return true;
      }
      out = select_pull(repo, *pull, field.selection);
      return true;
    }
    return false;
  }

  static std::vector<GSelect> selection_of(const GField& field, const std::string& name) {
    for (const auto& entry : field.selection)
      if (entry.field && entry.field->name == name) return entry.field->selection;
    return {};
  }

  static void sort_issues(std::vector<std::shared_ptr<Issue>>& issues, bool descending) {
    if (descending) {
      std::stable_sort(issues.begin(), issues.end(),
                       [](const std::shared_ptr<Issue>& a, const std::shared_ptr<Issue>& b) {
                         return a->number > b->number;
                       });
    } else {
      std::stable_sort(issues.begin(), issues.end(),
                       [](const std::shared_ptr<Issue>& a, const std::shared_ptr<Issue>& b) {
                         return a->number < b->number;
                       });
    }
  }

  JVal issue_connection(const Repo& repo, const GField& field) {
    int first = arg_int(field, "first", 100);
    std::string after = arg_string(field, "after");
    std::map<std::string, bool> state_filter;
    JVal states = arg_value(field, "states");
    if (states.type == JVal::ARR) {
      for (const auto& state : states.arr)
        if (state.type == JVal::STR) state_filter[to_lower(state.s)] = true;
    }
    bool descending = false;
    JVal order = arg_value(field, "orderBy");
    if (order.type == JVal::OBJ) {
      const JVal* direction = jget(order, "direction");
      if (direction && direction->type == JVal::STR && ieq(direction->s, "DESC")) descending = true;
    }
    std::vector<std::shared_ptr<Issue>> issues;
    {
      std::shared_lock<std::shared_mutex> lock(repo.mu);
      for (const auto& issue : repo.issues) {
        if (!state_filter.empty() &&
            state_filter.find(to_lower(issue->state)) == state_filter.end())
          continue;
        issues.push_back(issue);
      }
      sort_issues(issues, descending);
      std::vector<GSelect> nodes = selection_of(field, "nodes");
      std::vector<JVal> items;
      items.reserve(issues.size());
      for (const auto& issue : issues) items.push_back(select_issue(repo, *issue, nodes));
      Page page = paginate(std::move(items), first, after);
      return connection("IssueConnection", page, (int)issues.size(), field);
    }
  }

  JVal pull_connection(const Repo& repo, const GField& field) {
    int first = arg_int(field, "first", 100);
    std::string after = arg_string(field, "after");
    std::shared_lock<std::shared_mutex> lock(repo.mu);
    std::vector<JVal> items;
    int total = 0;
    std::vector<GSelect> nodes = selection_of(field, "nodes");
    for (const auto& pull : repo.pulls) {
      if (pull->state != "open") continue;
      items.push_back(select_pull(repo, *pull, nodes));
      total++;
    }
    Page page = paginate(std::move(items), first, after);
    return connection("PullRequestConnection", page, total, field);
  }

  JVal connection(const std::string& type_name, const Page& page, int total, const GField& field) {
    return object(type_name, field.selection,
                  [&](const std::string& name, const GField& inner, JVal& out) {
                    if (name == "nodes") {
                      JVal arr = JVal::array();
                      for (const auto& item : page.items) arr.push(item);
                      out = std::move(arr);
                      return true;
                    }
                    if (name == "totalCount") {
                      out = JVal::integer(total);
                      return true;
                    }
                    if (name == "edges") {
                      JVal arr = JVal::array();
                      for (size_t i = 0; i < page.items.size(); i++) {
                        JVal edge = JVal::object();
                        edge.set("node", page.items[i]);
                        edge.set("cursor", JVal::string(cursor_for((int)i + 1)));
                        arr.push(std::move(edge));
                      }
                      out = std::move(arr);
                      return true;
                    }
                    if (name == "pageInfo") {
                      out = object("PageInfo", inner.selection,
                                   [&](const std::string& info, const GField&, JVal& value) {
                                     if (info == "hasNextPage") {
                                       value = JVal::boolean(page.has_next);
                                       return true;
                                     }
                                     if (info == "hasPreviousPage") {
                                       value = JVal::boolean(false);
                                       return true;
                                     }
                                     if (info == "startCursor") {
                                       value = page.items.empty() ? JVal::null()
                                                                  : JVal::string(cursor_for(0));
                                       return true;
                                     }
                                     if (info == "endCursor") {
                                       value = page.items.empty() ? JVal::null()
                                                                  : JVal::string(page.end_cursor);
                                       return true;
                                     }
                                     return false;
                                   });
                      return true;
                    }
                    return false;
                  });
  }

  JVal select_issue(const Repo& repo, const Issue& issue, const std::vector<GSelect>& selection) {
    return object("Issue", selection, [&](const std::string& name, const GField& field, JVal& out) {
      if (name == "id") {
        out = JVal::string(global_id("issue", issue.id));
        return true;
      }
      if (name == "number") {
        out = JVal::integer(issue.number);
        return true;
      }
      if (name == "title") {
        out = JVal::string(issue.title);
        return true;
      }
      if (name == "state") {
        std::string state = issue.state;
        for (char& c : state)
          if (c >= 'a' && c <= 'z') c = char(c - 32);
        out = JVal::string(state);
        return true;
      }
      if (name == "body") {
        out = JVal::string(issue.body);
        return true;
      }
      if (name == "createdAt" || name == "updatedAt") {
        out = JVal::string(issue.created);
        return true;
      }
      if (name == "closed") {
        out = JVal::boolean(issue.state == "closed");
        return true;
      }
      if (name == "closedAt") {
        out = issue.state == "closed" ? JVal::string(kBaseTime) : JVal::null();
        return true;
      }
      if (name == "url") {
        out = JVal::string("http://github.local/" + repo.owner + "/" + repo.name + "/issues/" +
                           itoa10(issue.number));
        return true;
      }
      if (name == "author") {
        out = select_user(repo.owner, field.selection);
        return true;
      }
      if (name == "labels") {
        out = JVal::null();
        return true;
      }
      if (name == "comments") {
        JVal nodes = JVal::array();
        int total = 0;
        for (const auto& comment : issue.comments) {
          JVal node = JVal::object();
          node.set("id", JVal::string(global_id("comment", comment->id)));
          node.set("body", JVal::string(comment->body));
          nodes.push(std::move(node));
          total++;
        }
        JVal wrap = JVal::object();
        wrap.set("nodes", std::move(nodes));
        wrap.set("totalCount", JVal::integer(total));
        out = std::move(wrap);
        return true;
      }
      return false;
    });
  }

  JVal select_pull(const Repo& repo, const Pull& pull, const std::vector<GSelect>& selection) {
    return object("PullRequest", selection,
                  [&](const std::string& name, const GField& field, JVal& out) {
                    if (name == "id") {
                      out = JVal::string(global_id("pr", pull.id));
                      return true;
                    }
                    if (name == "number") {
                      out = JVal::integer(pull.number);
                      return true;
                    }
                    if (name == "state") {
                      if (pull.merged) {
                        out = JVal::string("MERGED");
                      } else {
                        std::string state = pull.state;
                        for (char& c : state)
                          if (c >= 'a' && c <= 'z') c = char(c - 32);
                        out = JVal::string(state);
                      }
                      return true;
                    }
                    if (name == "merged") {
                      out = JVal::boolean(pull.merged);
                      return true;
                    }
                    if (name == "headRefName") {
                      out = JVal::string(pull.head_ref);
                      return true;
                    }
                    if (name == "baseRefName") {
                      out = JVal::string(pull.base_ref);
                      return true;
                    }
                    if (name == "mergeable") {
                      out = JVal::string(pull.merged ? "UNKNOWN" : "MERGEABLE");
                      return true;
                    }
                    if (name == "files") {
                      int first = arg_int(field, "first", 100);
                      std::vector<PRFile> files = repo.pull_files_l(pull);
                      std::vector<GSelect> nodes = selection_of(field, "nodes");
                      std::vector<JVal> items;
                      items.reserve(files.size());
                      for (const auto& file : files) {
                        items.push_back(object(
                            "PullRequestChangedFile", nodes,
                            [&](const std::string& inner, const GField&, JVal& value) {
                              if (inner == "path") {
                                value = JVal::string(file.filename);
                                return true;
                              }
                              if (inner == "additions") {
                                value = JVal::integer(file.additions);
                                return true;
                              }
                              if (inner == "deletions") {
                                value = JVal::integer(file.deletions);
                                return true;
                              }
                              if (inner == "changeType") {
                                std::string status = file.status;
                                for (char& c : status)
                                  if (c >= 'a' && c <= 'z') c = char(c - 32);
                                value = JVal::string(status);
                                return true;
                              }
                              return false;
                            }));
                      }
                      Page page = paginate(std::move(items), first, "");
                      out = connection("PullRequestChangedFileConnection", page, (int)files.size(),
                                       field);
                      return true;
                    }
                    if (name == "title") {
                      out = JVal::string(pull.title);
                      return true;
                    }
                    if (name == "body") {
                      out = JVal::string(pull.body);
                      return true;
                    }
                    if (name == "createdAt" || name == "updatedAt") {
                      out = JVal::string(pull.created);
                      return true;
                    }
                    if (name == "isDraft") {
                      out = JVal::boolean(false);
                      return true;
                    }
                    if (name == "mergedAt") {
                      out = pull.merged ? JVal::string(kBaseTime) : JVal::null();
                      return true;
                    }
                    if (name == "mergeCommit") {
                      if (!pull.merged) {
                        out = JVal::null();
                      } else {
                        JVal commit = JVal::object();
                        commit.set("oid", JVal::string(pull.merge_sha));
                        commit.set("commitUrl", JVal::string(""));
                        out = std::move(commit);
                      }
                      return true;
                    }
                    if (name == "changedFiles") {
                      out = JVal::integer((long long)repo.pull_files_l(pull).size());
                      return true;
                    }
                    if (name == "url") {
                      out = JVal::string("http://github.local/" + repo.owner + "/" + repo.name +
                                         "/pull/" + itoa10(pull.number));
                      return true;
                    }
                    if (name == "headRefOid") {
                      out = JVal::string(pull.head_sha);
                      return true;
                    }
                    if (name == "baseRefOid") {
                      out = JVal::string(pull.base_sha);
                      return true;
                    }
                    return false;
                  });
  }

  JVal field_list(const std::string& type_name) {
    JVal arr = JVal::array();
    for (const auto& name : type_fields(type_name)) {
      JVal item = JVal::object();
      item.set("name", JVal::string(name));
      item.set("description", JVal::string("field " + name));
      item.set("isDeprecated", JVal::boolean(false));
      item.set("deprecationReason", JVal::null());
      arr.push(std::move(item));
    }
    return arr;
  }

  JVal enum_list(const std::string& type_name) {
    JVal arr = JVal::array();
    for (const auto& name : enum_values(type_name)) {
      JVal item = JVal::object();
      item.set("name", JVal::string(name));
      item.set("description", JVal::string("value " + name));
      item.set("isDeprecated", JVal::boolean(false));
      item.set("deprecationReason", JVal::null());
      arr.push(std::move(item));
    }
    return arr;
  }

  JVal root_field(const GField& field, bool& known) {
    known = true;
    if (field.name == "viewer") return select_user(store->user.login, field.selection);
    if (field.name == "repository") {
      std::string owner = arg_string(field, "owner");
      std::string name = arg_string(field, "name");
      auto repo = store->find_repo(owner, name);
      if (!repo) return JVal::null();
      return select_repository(*repo, field.selection);
    }
    if (field.name == "node") {
      std::string id = arg_string(field, "id");
      std::string kind;
      long long number = 0;
      if (!parse_global_id(id, kind, number)) return JVal::null();
      if (kind == "repo") {
        auto repo = repo_by_id(number);
        if (!repo) return JVal::null();
        return select_repository_node(*repo, field.selection);
      }
      if (kind == "issue") {
        auto found = issue_by_id(number);
        if (!found.first) return JVal::null();
        std::shared_lock<std::shared_mutex> lock(found.first->mu);
        return select_issue(*found.first, *found.second, field.selection);
      }
      if (kind == "pr") {
        auto found = pull_by_id(number);
        if (!found.first) return JVal::null();
        std::shared_lock<std::shared_mutex> lock(found.first->mu);
        return select_pull(*found.first, *found.second, field.selection);
      }
      return JVal::null();
    }
    if (field.name == "__type") {
      std::string name = arg_string(field, "name");
      const std::string& kind = type_kind(name);
      if (kind.empty()) return JVal::null();
      return object("__Type", field.selection,
                    [&](const std::string& inner, const GField&, JVal& out) {
                      if (inner == "name") {
                        out = JVal::string(name);
                        return true;
                      }
                      if (inner == "kind") {
                        out = JVal::string(kind);
                        return true;
                      }
                      if (inner == "description") {
                        out = JVal::string("type " + name);
                        return true;
                      }
                      if (inner == "fields") {
                        out = kind == "OBJECT" ? field_list(name) : JVal::array();
                        return true;
                      }
                      if (inner == "enumValues") {
                        out = kind == "ENUM" ? enum_list(name) : JVal::array();
                        return true;
                      }
                      if (inner == "interfaces" || inner == "possibleTypes") {
                        out = JVal::array();
                        return true;
                      }
                      return false;
                    });
    }
    known = false;
    fail_field("Query", field.name);
    return JVal::null();
  }

  static std::string input_string(const JVal& input, const std::string& key) {
    const JVal* v = jget(input, key);
    return v && v->type == JVal::STR ? v->s : std::string();
  }

  JVal mutation(const GField& field, bool& known) {
    known = true;
    JVal input = arg_value(field, "input");
    if (input.type != JVal::OBJ) input = JVal::object();
    if (field.name == "createIssue") {
      std::string kind;
      long long id = 0;
      if (!parse_global_id(input_string(input, "repositoryId"), kind, id) || kind != "repo")
        return JVal::null();
      auto repo = repo_by_id(id);
      if (!repo) return JVal::null();
      std::unique_lock<std::shared_mutex> lock(repo->mu);
      auto issue = repo->new_issue_l(store->next_issue_id(), input_string(input, "title"),
                                     input_string(input, "body"), {});
      JVal body = JVal::object();
      body.set("issue", select_issue(*repo, *issue, selection_of(field, "issue")));
      return body;
    }
    if (field.name == "closeIssue") {
      std::string kind;
      long long id = 0;
      if (!parse_global_id(input_string(input, "issueId"), kind, id) || kind != "issue")
        return JVal::null();
      auto found = issue_by_id(id);
      if (!found.first) return JVal::null();
      std::unique_lock<std::shared_mutex> lock(found.first->mu);
      found.second->state = "closed";
      found.second->closed_at = kBaseTime;
      found.second->updated = kBaseTime;
      found.second->cached.reset();
      JVal body = JVal::object();
      body.set("issue", select_issue(*found.first, *found.second, selection_of(field, "issue")));
      return body;
    }
    if (field.name == "createPullRequest") {
      std::string kind;
      long long id = 0;
      if (!parse_global_id(input_string(input, "repositoryId"), kind, id) || kind != "repo")
        return JVal::null();
      auto repo = repo_by_id(id);
      if (!repo) return JVal::null();
      std::string head = input_string(input, "headRefName");
      std::string base = input_string(input, "baseRefName");
      if (base.empty()) base = repo->default_branch;
      std::unique_lock<std::shared_mutex> lock(repo->mu);
      std::string head_tip, base_tip;
      if (!repo->ref_l("refs/heads/" + head, head_tip) || !repo->ref_l("refs/heads/" + base, base_tip))
        return JVal::null();
      auto pull = repo->new_pull_l(store->next_pull_id(), input_string(input, "title"),
                                   input_string(input, "body"), head, base);
      pull->head_sha = head_tip;
      pull->base_sha = base_tip;
      JVal body = JVal::object();
      body.set("pullRequest",
               select_pull(*repo, *pull, selection_of(field, "pullRequest")));
      return body;
    }
    if (field.name == "mergePullRequest") {
      std::string kind;
      long long id = 0;
      if (!parse_global_id(input_string(input, "pullRequestId"), kind, id) || kind != "pr")
        return JVal::null();
      auto found = pull_by_id(id);
      if (!found.first) return JVal::null();
      std::unique_lock<std::shared_mutex> lock(found.first->mu);
      if (!found.second->merged) {
        std::string author = store->user.name + " <" + store->user.email + ">";
        std::string message = found.second->title + " (#" + itoa10(found.second->number) + ")\n";
        std::string commit;
        found.first->merge_squash_l(*found.second, message, author,
                                    1735689600 + (long long)found.first->objects.size(), commit);
      }
      JVal body = JVal::object();
      body.set("pullRequest",
               select_pull(*found.first, *found.second, selection_of(field, "pullRequest")));
      return body;
    }
    if (field.name == "addComment") {
      std::string kind;
      long long id = 0;
      if (!parse_global_id(input_string(input, "subjectId"), kind, id) || kind != "issue")
        return JVal::null();
      auto found = issue_by_id(id);
      if (!found.first) return JVal::null();
      std::unique_lock<std::shared_mutex> lock(found.first->mu);
      auto comment = std::make_shared<Comment>();
      comment->id = store->next_comment_id();
      comment->body = input_string(input, "body");
      comment->login = found.first->owner;
      comment->created = kBaseTime;
      found.second->comments.push_back(comment);
      JVal node = JVal::object();
      node.set("id", JVal::string(global_id("comment", comment->id)));
      node.set("body", JVal::string(comment->body));
      JVal edge = JVal::object();
      edge.set("node", std::move(node));
      JVal body = JVal::object();
      body.set("commentEdge", std::move(edge));
      return body;
    }
    known = false;
    fail_field("Mutation", field.name);
    return JVal::null();
  }
};

GVal value_from_json(const JVal& value) {
  GVal out;
  switch (value.type) {
    case JVal::NUL:
      out.kind = GVal::NUL;
      break;
    case JVal::STR:
      out.kind = GVal::STR;
      out.str = value.s;
      break;
    case JVal::BOOL:
      out.kind = GVal::BOOL;
      out.flag = value.b;
      break;
    case JVal::INT:
      out.kind = GVal::INT;
      out.num = value.i;
      break;
    case JVal::DBL:
      out.kind = GVal::INT;
      out.num = (long long)value.d;
      break;
    case JVal::ARR:
      out.kind = GVal::LIST;
      for (const auto& item : value.arr) out.list.push_back(value_from_json(item));
      break;
    case JVal::OBJ:
      out.kind = GVal::OBJ;
      for (const auto& kv : value.obj) out.obj.emplace_back(kv.first, value_from_json(kv.second));
      break;
  }
  return out;
}

JVal one_graphql(Store& store, const std::string& query, const JVal& variables) {
  GOperation op = parse_document(query);
  GqlExec exec;
  exec.store = &store;
  if (op.failed) {
    JVal error = JVal::object();
    error.set("message", JVal::string("subscriptions are not supported"));
    JVal out = JVal::object();
    out.set("data", JVal::null());
    JVal errors = JVal::array();
    errors.push(std::move(error));
    out.set("errors", std::move(errors));
    return out;
  }
  if (variables.type == JVal::OBJ)
    for (const auto& kv : variables.obj) exec.vars.emplace_back(kv.first, value_from_json(kv.second));
  JVal data = JVal::object();
  for (const auto& entry : op.selection) {
    if (!entry.field) continue;
    bool known = true;
    JVal value;
    if (op.kind == "mutation")
      value = exec.mutation(*entry.field, known);
    else
      value = exec.root_field(*entry.field, known);
    data.set(entry.field->alias, std::move(value));
  }
  JVal out = JVal::object();
  out.set("data", std::move(data));
  if (!exec.errors.empty()) {
    JVal errors = JVal::array();
    for (auto& error : exec.errors) errors.push(std::move(error));
    out.set("errors", std::move(errors));
  }
  return out;
}

JVal graphql_body(Store& store, const std::string& body, int& status) {
  std::string trimmed = trim_ws(body);
  if (trimmed.empty()) {
    status = 400;
    JVal out = JVal::object();
    out.set("message", JVal::string("Problems parsing JSON"));
    return out;
  }
  status = 200;
  if (trimmed[0] == '[') {
    JVal parsed;
    if (!json_parse(body, parsed) || parsed.type != JVal::ARR) {
      status = 400;
      JVal out = JVal::object();
      out.set("message", JVal::string("Problems parsing JSON"));
      return out;
    }
    JVal responses = JVal::array();
    for (const auto& operation : parsed.arr) {
      std::string query = jstr(operation, "query");
      const JVal* variables = jget(operation, "variables");
      responses.push(one_graphql(store, query, variables ? *variables : JVal::null()));
    }
    return responses;
  }
  JVal parsed;
  if (!json_parse(body, parsed) || parsed.type != JVal::OBJ) {
    status = 400;
    JVal out = JVal::object();
    out.set("message", JVal::string("Problems parsing JSON"));
    return out;
  }
  std::string query = jstr(parsed, "query");
  if (trim_ws(query).empty()) {
    JVal out = JVal::object();
    out.set("data", JVal::null());
    JVal errors = JVal::array();
    JVal error = JVal::object();
    error.set("message", JVal::string("query is required"));
    errors.push(std::move(error));
    out.set("errors", std::move(errors));
    return out;
  }
  const JVal* variables = jget(parsed, "variables");
  return one_graphql(store, query, variables ? *variables : JVal::null());
}

}  // namespace

void handle_graphql(Store& store, const Request& req, Response& res) {
  if (req.method != "POST") {
    fail_json(store, res, 405, "Method Not Allowed");
    return;
  }
  if (!store.token.empty()) {
    std::string header = req.header("authorization");
    std::string value = header;
    if (starts_with(value, "Bearer ")) value = value.substr(7);
    value = trim_ws(value);
    if (header.empty() || value != store.token) {
      fail_json(store, res, 401, "Requires authentication");
      return;
    }
  }
  int status = 200;
  JVal body = graphql_body(store, req.body, status);
  write_json_text(store, res, status, json_text(body));
}

}  // namespace kestrel
