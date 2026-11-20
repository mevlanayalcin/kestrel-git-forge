// Model Context Protocol over Streamable HTTP: server/discover, the legacy handshake, tools/list
// and the nine tools the suite exercises.
#include <string>
#include <vector>

#include "forge.hpp"

namespace kestrel {

namespace {

const char* kMcpRevision = "2026-07-28";

JVal prop(const char* kind, const char* description) {
  JVal value = JVal::object();
  value.set("type", JVal::string(kind));
  value.set("description", JVal::string(description));
  return value;
}

JVal schema(std::vector<std::string> required, std::vector<std::pair<std::string, JVal>> properties) {
  JVal value = JVal::object();
  value.set("type", JVal::string("object"));
  JVal props = JVal::object();
  for (auto& kv : properties) props.set(kv.first, std::move(kv.second));
  value.set("properties", std::move(props));
  JVal req = JVal::array();
  for (auto& name : required) req.push(JVal::string(name));
  value.set("required", std::move(req));
  return value;
}

JVal tool_spec(const char* name, const char* title, const char* description, JVal input_schema) {
  JVal tool = JVal::object();
  tool.set("name", JVal::string(name));
  tool.set("title", JVal::string(title));
  tool.set("description", JVal::string(description));
  tool.set("inputSchema", std::move(input_schema));
  return tool;
}

JVal mcp_tools() {
  JVal tools = JVal::array();
  tools.push(tool_spec(
      "create_repo", "Create repository",
      "Create a repository in the forge for the authenticated user.",
      schema({"name"},
             {{"owner", prop("string", "repository owner login (defaults to the authenticated user)")},
              {"name", prop("string", "repository name")},
              {"description", prop("string", "repository description")},
              {"private", prop("boolean", "whether the repository is private")}})));
  tools.push(tool_spec("get_repo", "Get repository", "Read one repository by owner and name.",
                       schema({"owner", "repo"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")}})));
  JVal label_items = prop("string", "label name");
  JVal labels = JVal::object();
  labels.set("type", JVal::string("array"));
  labels.set("items", std::move(label_items));
  labels.set("description", JVal::string("label names"));
  tools.push(tool_spec("create_issue", "Create issue", "Open an issue in a repository.",
                       schema({"owner", "repo", "title"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")},
                               {"title", prop("string", "issue title")},
                               {"body", prop("string", "issue body")},
                               {"labels", std::move(labels)}})));
  tools.push(tool_spec("list_issues", "List issues",
                       "List the issues of a repository, filtered by state.",
                       schema({"owner", "repo"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")},
                               {"state", prop("string", "open, closed or all")}})));
  tools.push(tool_spec("create_pull_request", "Create pull request",
                       "Open a pull request from a head branch into a base branch.",
                       schema({"owner", "repo", "title", "head", "base"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")},
                               {"title", prop("string", "pull request title")},
                               {"body", prop("string", "pull request body")},
                               {"head", prop("string", "head branch name")},
                               {"base", prop("string", "base branch name")}})));
  tools.push(tool_spec("merge_pull_request", "Merge pull request",
                       "Squash-merge an open pull request into its base branch.",
                       schema({"owner", "repo", "pull_number"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")},
                               {"pull_number", prop("integer", "pull request number")}})));
  tools.push(tool_spec("close_issue", "Close issue", "Close an issue by number.",
                       schema({"owner", "repo", "issue_number"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")},
                               {"issue_number", prop("integer", "issue number")}})));
  tools.push(tool_spec("add_comment", "Add comment", "Add a comment to an issue.",
                       schema({"owner", "repo", "issue_number", "body"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")},
                               {"issue_number", prop("integer", "issue number")},
                               {"body", prop("string", "comment body")}})));
  tools.push(tool_spec("list_pulls", "List pull requests",
                       "List the pull requests of a repository.",
                       schema({"owner", "repo"},
                              {{"owner", prop("string", "repository owner login")},
                               {"repo", prop("string", "repository name")},
                               {"state", prop("string", "open, closed or all")}})));
  return tools;
}

std::string str_arg(const JVal& args, const char* name) {
  const JVal* v = jget(args, name);
  return v && v->type == JVal::STR ? v->s : std::string();
}

long long int_arg(const JVal& args, const char* name) {
  const JVal* v = jget(args, name);
  if (!v) return 0;
  if (v->type == JVal::INT) return v->i;
  if (v->type == JVal::DBL) return (long long)v->d;
  if (v->type == JVal::STR) {
    char* end = nullptr;
    long long value = std::strtoll(v->s.c_str(), &end, 10);
    if (end != v->s.c_str() && *end == '\0') return value;
  }
  return 0;
}

bool bool_arg(const JVal& args, const char* name) {
  const JVal* v = jget(args, name);
  return v && v->type == JVal::BOOL ? v->b : false;
}

bool missing_arg(const JVal& args, const char* name) {
  const JVal* v = jget(args, name);
  if (!v || v->type == JVal::NUL) return true;
  if (v->type == JVal::STR && trim_ws(v->s).empty()) return true;
  return false;
}

bool missing_any(const JVal& args, std::initializer_list<const char*> names) {
  for (const char* name : names)
    if (missing_arg(args, name)) return true;
  return false;
}

JVal tool_content(const JVal& value) {
  std::string text = json_text(value);
  JVal block = JVal::object();
  block.set("type", JVal::string("text"));
  block.set("text", JVal::string(text));
  JVal content = JVal::array();
  content.push(std::move(block));
  JVal out = JVal::object();
  out.set("content", std::move(content));
  out.set("structuredContent", value);
  out.set("isError", JVal::boolean(false));
  return out;
}

JVal tool_error(const std::string& message) {
  JVal block = JVal::object();
  block.set("type", JVal::string("text"));
  block.set("text", JVal::string(message));
  JVal content = JVal::array();
  content.push(std::move(block));
  JVal out = JVal::object();
  out.set("content", std::move(content));
  out.set("isError", JVal::boolean(true));
  return out;
}

JVal parse_json_text(const std::string& text) {
  JVal value;
  if (!json_parse(text, value)) return JVal::null();
  return value;
}

struct ToolCall {
  JVal result;
  int rpc_code = 0;
  std::string rpc_message;
};

ToolCall call_tool(Store& store, const std::string& name, const JVal& args) {
  ToolCall call;
  std::string owner = str_arg(args, "owner");
  if (owner.empty()) owner = store.user.login;
  std::string repo_name = str_arg(args, "repo");
  if (repo_name.empty()) repo_name = str_arg(args, "name");

  if (name == "create_repo" || name == "create_repository") {
    if (missing_arg(args, "name") && missing_arg(args, "repo")) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: name is required";
      return call;
    }
    auto repo = store.create_repo(owner, repo_name, str_arg(args, "description"),
                                  bool_arg(args, "private"));
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " already exists");
      return call;
    }
    call.result = tool_content(parse_json_text(*repo_json_cached(store, *repo)));
    return call;
  }
  if (name == "get_repo" || name == "get_repository") {
    if (missing_any(args, {"owner", "repo"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner and repo are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    call.result = tool_content(parse_json_text(*repo_json_cached(store, *repo)));
    return call;
  }
  if (name == "create_issue") {
    if (missing_any(args, {"owner", "repo", "title"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner, repo and title are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    std::vector<std::string> labels;
    const JVal* list = jget(args, "labels");
    if (list && list->type == JVal::ARR)
      for (const auto& entry : list->arr)
        if (entry.type == JVal::STR) labels.push_back(entry.s);
    std::unique_lock<std::shared_mutex> lock(repo->mu);
    for (const auto& label : labels) {
      if (repo->labels.find(label) == repo->labels.end()) {
        lock.unlock();
        call.result = tool_error("label " + label + " does not exist");
        return call;
      }
    }
    auto issue = repo->new_issue_l(store.next_issue_id(), str_arg(args, "title"),
                                   str_arg(args, "body"), labels);
    std::string text;
    issue_full_json_l(text, *repo, *issue);
    call.result = tool_content(parse_json_text(text));
    return call;
  }
  if (name == "list_issues") {
    if (missing_any(args, {"owner", "repo"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner and repo are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    std::string state = str_arg(args, "state");
    if (state.empty()) state = "open";
    std::shared_lock<std::shared_mutex> lock(repo->mu);
    JVal issues = JVal::array();
    int total = 0;
    for (const auto& issue : repo->issues) {
      if (state != "all" && issue->state != state) continue;
      std::string text;
      issue_full_json_l(text, *repo, *issue);
      issues.push(parse_json_text(text));
      total++;
    }
    JVal result = JVal::object();
    result.set("issues", std::move(issues));
    result.set("total_count", JVal::integer(total));
    call.result = tool_content(result);
    return call;
  }
  if (name == "create_pull_request") {
    if (missing_any(args, {"owner", "repo", "title", "head", "base"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner, repo, title, head and base are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    std::string head = str_arg(args, "head");
    std::string base = str_arg(args, "base");
    std::unique_lock<std::shared_mutex> lock(repo->mu);
    std::string head_tip, base_tip;
    if (!repo->ref_l("refs/heads/" + head, head_tip) ||
        !repo->ref_l("refs/heads/" + base, base_tip)) {
      lock.unlock();
      call.result = tool_error("head or base branch does not exist");
      return call;
    }
    auto pull = repo->new_pull_l(store.next_pull_id(), str_arg(args, "title"),
                                 str_arg(args, "body"), head, base);
    pull->head_sha = head_tip;
    pull->base_sha = base_tip;
    std::string text;
    pull_json_l(text, *repo, *pull);
    call.result = tool_content(parse_json_text(text));
    return call;
  }
  if (name == "merge_pull_request") {
    if (missing_any(args, {"owner", "repo", "pull_number"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner, repo and pull_number are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    long long number = int_arg(args, "pull_number");
    std::unique_lock<std::shared_mutex> lock(repo->mu);
    auto pull = repo->find_pull_l((int)number);
    if (!pull) {
      lock.unlock();
      call.result = tool_error("pull request #" + itoa10(number) + " not found");
      return call;
    }
    if (!pull->merged) {
      std::string author = store.user.name + " <" + store.user.email + ">";
      std::string message = pull->title + " (#" + itoa10(pull->number) + ")\n";
      std::string commit;
      if (!repo->merge_squash_l(*pull, message, author,
                                1735689600 + (long long)repo->objects.size(), commit)) {
        lock.unlock();
        call.result = tool_error("pull request is not mergeable");
        return call;
      }
    }
    std::string text;
    pull_json_l(text, *repo, *pull);
    call.result = tool_content(parse_json_text(text));
    return call;
  }
  if (name == "close_issue") {
    if (missing_any(args, {"owner", "repo", "issue_number"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner, repo and issue_number are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    std::unique_lock<std::shared_mutex> lock(repo->mu);
    auto issue = repo->find_issue_l((int)int_arg(args, "issue_number"));
    if (!issue) {
      lock.unlock();
      call.result = tool_error("issue not found");
      return call;
    }
    issue->state = "closed";
    issue->closed_at = kBaseTime;
    issue->invalidate_cache();
    std::string text;
    issue_full_json_l(text, *repo, *issue);
    call.result = tool_content(parse_json_text(text));
    return call;
  }
  if (name == "add_comment") {
    if (missing_any(args, {"owner", "repo", "issue_number", "body"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner, repo, issue_number and body are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    std::unique_lock<std::shared_mutex> lock(repo->mu);
    auto issue = repo->find_issue_l((int)int_arg(args, "issue_number"));
    if (!issue) {
      lock.unlock();
      call.result = tool_error("issue not found");
      return call;
    }
    auto comment = std::make_shared<Comment>();
    comment->id = store.next_comment_id();
    comment->body = str_arg(args, "body");
    comment->login = owner;
    comment->created = kBaseTime;
    issue->comments.push_back(comment);
    issue->invalidate_cache();
    std::string text;
    comment_json_l(text, *repo, *issue, *comment);
    call.result = tool_content(parse_json_text(text));
    return call;
  }
  if (name == "list_pulls") {
    if (missing_any(args, {"owner", "repo"})) {
      call.rpc_code = -32602;
      call.rpc_message = "Invalid params: owner and repo are required";
      return call;
    }
    auto repo = store.find_repo(owner, repo_name);
    if (!repo) {
      call.result = tool_error("repository " + owner + "/" + repo_name + " not found");
      return call;
    }
    std::string state = str_arg(args, "state");
    if (state.empty()) state = "open";
    std::shared_lock<std::shared_mutex> lock(repo->mu);
    JVal pulls = JVal::array();
    int total = 0;
    for (const auto& pull : repo->pulls) {
      if (state != "all" && pull->state != state) continue;
      std::string text;
      pull_json_l(text, *repo, *pull);
      pulls.push(parse_json_text(text));
      total++;
    }
    JVal result = JVal::object();
    result.set("pulls", std::move(pulls));
    result.set("total_count", JVal::integer(total));
    call.result = tool_content(result);
    return call;
  }
  call.rpc_code = -32602;
  call.rpc_message = "Unknown tool: " + name;
  return call;
}

void mcp_result(Store& store, Response& res, const JVal& id, const JVal& result) {
  JVal out = JVal::object();
  out.set("jsonrpc", JVal::string("2.0"));
  out.set("id", id);
  out.set("result", result);
  write_json_text(store, res, 200, json_text(out));
}

void mcp_error(Store& store, Response& res, int status, const JVal& id, int code,
               const std::string& message) {
  JVal error = JVal::object();
  error.set("code", JVal::integer(code));
  error.set("message", JVal::string(message));
  JVal out = JVal::object();
  out.set("jsonrpc", JVal::string("2.0"));
  out.set("id", id);
  out.set("error", std::move(error));
  write_json_text(store, res, status, json_text(out));
}

}  // namespace

void handle_mcp(Store& store, const Request& req, Response& res) {
  if (req.method == "DELETE") {
    res.status = 200;
    res.body.clear();
    res.content_type.clear();
    return;
  }
  if (req.method != "POST") {
    res.set_header("Allow", "POST");
    res.status = 405;
    res.body.clear();
    res.content_type.clear();
    return;
  }
  JVal body;
  if (!json_parse(req.body, body) || body.type != JVal::OBJ) {
    mcp_error(store, res, 400, JVal::null(), -32700, "Parse error: the body is not JSON");
    return;
  }
  std::string method = jstr(body, "method");
  const JVal* id_ptr = jget(body, "id");
  JVal id = id_ptr ? *id_ptr : JVal::null();
  bool is_notification = !id_ptr || id_ptr->type == JVal::NUL;
  if (method.empty()) {
    mcp_error(store, res, 400, id, -32600, "Invalid Request");
    return;
  }
  const JVal* params_ptr = jget(body, "params");
  JVal params = params_ptr && params_ptr->type == JVal::OBJ ? *params_ptr : JVal::object();

  if (method == "server/discover") {
    JVal result = JVal::object();
    result.set("resultType", JVal::string("complete"));
    JVal versions = JVal::array();
    versions.push(JVal::string(kMcpRevision));
    versions.push(JVal::string("2025-11-25"));
    versions.push(JVal::string("2025-06-18"));
    result.set("supportedVersions", std::move(versions));
    JVal tools = JVal::object();
    tools.set("listChanged", JVal::boolean(false));
    JVal capabilities = JVal::object();
    capabilities.set("tools", std::move(tools));
    result.set("capabilities", std::move(capabilities));
    JVal server_info = JVal::object();
    server_info.set("name", JVal::string("kestrel-forge"));
    server_info.set("version", JVal::string("1.0.0"));
    result.set("serverInfo", std::move(server_info));
    result.set("instructions",
               JVal::string("A GitHub-compatible forge: repositories, issues, pull requests."));
    mcp_result(store, res, id, result);
    return;
  }
  if (method == "initialize") {
    std::string version = kMcpRevision;
    std::string given = jstr(params, "protocolVersion");
    if (!given.empty()) version = given;
    JVal result = JVal::object();
    result.set("protocolVersion", JVal::string(version));
    JVal tools = JVal::object();
    tools.set("listChanged", JVal::boolean(false));
    JVal capabilities = JVal::object();
    capabilities.set("tools", std::move(tools));
    result.set("capabilities", std::move(capabilities));
    JVal server_info = JVal::object();
    server_info.set("name", JVal::string("kestrel-forge"));
    server_info.set("version", JVal::string("1.0.0"));
    result.set("serverInfo", std::move(server_info));
    mcp_result(store, res, id, result);
    return;
  }
  if (method == "notifications/initialized" || method == "notifications/cancelled") {
    res.status = 202;
    res.content_type = kJsonType;
    res.body.clear();
    return;
  }
  if (method == "ping") {
    mcp_result(store, res, id, JVal::object());
    return;
  }
  if (method == "tools/list") {
    JVal result = JVal::object();
    result.set("tools", mcp_tools());
    mcp_result(store, res, id, result);
    return;
  }
  if (method == "tools/call") {
    if (is_notification) {
      res.status = 202;
      res.content_type = kJsonType;
      res.body.clear();
      return;
    }
    std::string name = jstr(params, "name");
    const JVal* arguments = jget(params, "arguments");
    JVal args = arguments && arguments->type == JVal::OBJ ? *arguments : JVal::object();
    ToolCall call = call_tool(store, name, args);
    if (call.rpc_code != 0) {
      mcp_error(store, res, 200, id, call.rpc_code, call.rpc_message);
      return;
    }
    mcp_result(store, res, id, call.result);
    return;
  }
  mcp_error(store, res, 200, id, -32601, "Method not found: " + method);
}

}  // namespace kestrel
