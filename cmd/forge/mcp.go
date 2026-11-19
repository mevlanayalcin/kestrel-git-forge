package main

// Model Context Protocol over the Streamable HTTP transport at /mcp: revision 2026-07-28
// (server/discover, no handshake) plus the legacy initialize handshake, and the nine tools the
// suite exercises. Every tool acts on the same store REST and git serve.

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
)

const mcpRevision = "2026-07-28"

type mcpTool struct {
	Name        string         `json:"name"`
	Title       string         `json:"title,omitempty"`
	Description string         `json:"description"`
	InputSchema map[string]any `json:"inputSchema"`
}

func schema(required []string, properties map[string]any) map[string]any {
	return map[string]any{
		"type":       "object",
		"properties": properties,
		"required":   required,
	}
}

func prop(kind, description string) map[string]any {
	return map[string]any{"type": kind, "description": description}
}

func mcpTools() []mcpTool {
	return []mcpTool{
		{
			Name: "create_repo", Title: "Create repository",
			Description: "Create a repository in the forge for the authenticated user.",
			InputSchema: schema([]string{"name"}, map[string]any{
				"owner":       prop("string", "repository owner login (defaults to the authenticated user)"),
				"name":        prop("string", "repository name"),
				"description": prop("string", "repository description"),
				"private":     prop("boolean", "whether the repository is private"),
			}),
		},
		{
			Name: "get_repo", Title: "Get repository",
			Description: "Read one repository by owner and name.",
			InputSchema: schema([]string{"owner", "repo"}, map[string]any{
				"owner": prop("string", "repository owner login"),
				"repo":  prop("string", "repository name"),
			}),
		},
		{
			Name: "create_issue", Title: "Create issue",
			Description: "Open an issue in a repository.",
			InputSchema: schema([]string{"owner", "repo", "title"}, map[string]any{
				"owner":  prop("string", "repository owner login"),
				"repo":   prop("string", "repository name"),
				"title":  prop("string", "issue title"),
				"body":   prop("string", "issue body"),
				"labels": map[string]any{"type": "array", "items": prop("string", "label name"), "description": "label names"},
			}),
		},
		{
			Name: "list_issues", Title: "List issues",
			Description: "List the issues of a repository, filtered by state.",
			InputSchema: schema([]string{"owner", "repo"}, map[string]any{
				"owner": prop("string", "repository owner login"),
				"repo":  prop("string", "repository name"),
				"state": prop("string", "open, closed or all"),
			}),
		},
		{
			Name: "create_pull_request", Title: "Create pull request",
			Description: "Open a pull request from a head branch into a base branch.",
			InputSchema: schema([]string{"owner", "repo", "title", "head", "base"}, map[string]any{
				"owner": prop("string", "repository owner login"),
				"repo":  prop("string", "repository name"),
				"title": prop("string", "pull request title"),
				"body":  prop("string", "pull request body"),
				"head":  prop("string", "head branch name"),
				"base":  prop("string", "base branch name"),
			}),
		},
		{
			Name: "merge_pull_request", Title: "Merge pull request",
			Description: "Squash-merge an open pull request into its base branch.",
			InputSchema: schema([]string{"owner", "repo", "pull_number"}, map[string]any{
				"owner":       prop("string", "repository owner login"),
				"repo":        prop("string", "repository name"),
				"pull_number": prop("integer", "pull request number"),
			}),
		},
		{
			Name: "close_issue", Title: "Close issue",
			Description: "Close an issue by number.",
			InputSchema: schema([]string{"owner", "repo", "issue_number"}, map[string]any{
				"owner":        prop("string", "repository owner login"),
				"repo":         prop("string", "repository name"),
				"issue_number": prop("integer", "issue number"),
			}),
		},
		{
			Name: "add_comment", Title: "Add comment",
			Description: "Add a comment to an issue.",
			InputSchema: schema([]string{"owner", "repo", "issue_number", "body"}, map[string]any{
				"owner":        prop("string", "repository owner login"),
				"repo":         prop("string", "repository name"),
				"issue_number": prop("integer", "issue number"),
				"body":         prop("string", "comment body"),
			}),
		},
		{
			Name: "list_pulls", Title: "List pull requests",
			Description: "List the pull requests of a repository.",
			InputSchema: schema([]string{"owner", "repo"}, map[string]any{
				"owner": prop("string", "repository owner login"),
				"repo":  prop("string", "repository name"),
				"state": prop("string", "open, closed or all"),
			}),
		},
	}
}

type mcpRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}

func (s *Store) handleMCP(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodDelete {
		w.WriteHeader(200)
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 4<<20))
	if err != nil {
		s.mcpError(w, 400, nil, -32700, "Parse error")
		return
	}
	var request mcpRequest
	if err := json.Unmarshal(body, &request); err != nil {
		s.mcpError(w, 400, nil, -32700, "Parse error: the body is not JSON")
		return
	}
	if request.Method == "" {
		s.mcpError(w, 400, request.ID, -32600, "Invalid Request")
		return
	}
	isNotification := len(request.ID) == 0 || string(request.ID) == "null"

	var params map[string]any
	if len(request.Params) > 0 {
		_ = json.Unmarshal(request.Params, &params)
	}

	switch request.Method {
	case "server/discover":
		s.mcpResult(w, request.ID, map[string]any{
			"resultType":        "complete",
			"supportedVersions": []string{mcpRevision, "2025-11-25", "2025-06-18"},
			"capabilities":      map[string]any{"tools": map[string]any{"listChanged": false}},
			"serverInfo":        map[string]any{"name": "kestrel-forge", "version": "1.0.0"},
			"instructions":      "A GitHub-compatible forge: repositories, issues, pull requests.",
		})
	case "initialize":
		version := mcpRevision
		if params != nil {
			if value, ok := params["protocolVersion"].(string); ok && value != "" {
				version = value
			}
		}
		s.mcpResult(w, request.ID, map[string]any{
			"protocolVersion": version,
			"capabilities":    map[string]any{"tools": map[string]any{"listChanged": false}},
			"serverInfo":      map[string]any{"name": "kestrel-forge", "version": "1.0.0"},
		})
	case "notifications/initialized", "notifications/cancelled":
		w.Header().Set("Content-Type", jsonType)
		w.WriteHeader(http.StatusAccepted)
	case "ping":
		s.mcpResult(w, request.ID, map[string]any{})
	case "tools/list":
		s.mcpResult(w, request.ID, map[string]any{"tools": mcpTools()})
	case "tools/call":
		if isNotification {
			w.WriteHeader(http.StatusAccepted)
			return
		}
		name, _ := params["name"].(string)
		arguments := map[string]any{}
		if raw, ok := params["arguments"].(map[string]any); ok {
			arguments = raw
		}
		result, rpcCode, rpcMessage := s.callTool(name, arguments)
		if rpcCode != 0 {
			s.mcpError(w, 200, request.ID, rpcCode, rpcMessage)
			return
		}
		s.mcpResult(w, request.ID, result)
	default:
		s.mcpError(w, 200, request.ID, -32601, "Method not found: "+request.Method)
	}
}

func (s *Store) mcpResult(w http.ResponseWriter, id json.RawMessage, result any) {
	body, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": rawID(id), "result": result})
	w.Header().Set("Content-Type", jsonType)
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(200)
	w.Write(body)
}

func (s *Store) mcpError(w http.ResponseWriter, status int, id json.RawMessage, code int, message string) {
	body, _ := json.Marshal(map[string]any{
		"jsonrpc": "2.0",
		"id":      rawID(id),
		"error":   map[string]any{"code": code, "message": message},
	})
	w.Header().Set("Content-Type", jsonType)
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(status)
	w.Write(body)
}

func rawID(id json.RawMessage) any {
	if len(id) == 0 {
		return nil
	}
	var value any
	if err := json.Unmarshal(id, &value); err != nil {
		return nil
	}
	return value
}

// toolContent wraps a JSON value as an MCP tool result.
func toolContent(value any) map[string]any {
	text, _ := json.Marshal(value)
	return map[string]any{
		"content":           []any{map[string]any{"type": "text", "text": string(text)}},
		"structuredContent": value,
		"isError":           false,
	}
}

func toolError(message string) map[string]any {
	return map[string]any{
		"content": []any{map[string]any{"type": "text", "text": message}},
		"isError": true,
	}
}

func stringArg(args map[string]any, name string) string {
	if value, ok := args[name].(string); ok {
		return value
	}
	return ""
}

func intArg(args map[string]any, name string) int {
	switch value := args[name].(type) {
	case float64:
		return int(value)
	case int:
		return value
	case string:
		if parsed, err := strconv.Atoi(value); err == nil {
			return parsed
		}
	}
	return 0
}

func boolArg(args map[string]any, name string) bool {
	if value, ok := args[name].(bool); ok {
		return value
	}
	return false
}

func (s *Store) callTool(name string, args map[string]any) (map[string]any, int, string) {
	missing := func(fields ...string) bool {
		for _, field := range fields {
			switch args[field].(type) {
			case nil:
				return true
			case string:
				if strings.TrimSpace(args[field].(string)) == "" {
					return true
				}
			}
		}
		return false
	}
	owner := stringArg(args, "owner")
	if owner == "" {
		owner = s.user.Login
	}
	repoName := stringArg(args, "repo")
	if repoName == "" {
		repoName = stringArg(args, "name")
	}
	switch name {
	case "create_repo", "create_repository":
		if missing("name") && missing("repo") {
			return nil, -32602, "Invalid params: name is required"
		}
		repo, ok := s.createRepo(owner, repoName, stringArg(args, "description"), boolArg(args, "private"))
		if !ok {
			return toolError("repository " + owner + "/" + repoName + " already exists"), 0, ""
		}
		return toolContent(s.repoJSON(repo)), 0, ""
	case "get_repo", "get_repository":
		if missing("owner", "repo") {
			return nil, -32602, "Invalid params: owner and repo are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		return toolContent(s.repoJSON(repo)), 0, ""
	case "create_issue":
		if missing("owner", "repo", "title") {
			return nil, -32602, "Invalid params: owner, repo and title are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		labels := []string{}
		if list, ok := args["labels"].([]any); ok {
			for _, entry := range list {
				if text, ok := entry.(string); ok {
					labels = append(labels, text)
				}
			}
		}
		repo.mu.Lock()
		for _, label := range labels {
			if repo.Labels[label] == nil {
				repo.mu.Unlock()
				return toolError("label " + label + " does not exist"), 0, ""
			}
		}
		issue := repo.newIssue(s.nextIssueID(), stringArg(args, "title"), stringArg(args, "body"), labels)
		body := s.issueJSONLocked(repo, issue)
		repo.mu.Unlock()
		return toolContent(body), 0, ""
	case "list_issues":
		if missing("owner", "repo") {
			return nil, -32602, "Invalid params: owner and repo are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		state := stringArg(args, "state")
		if state == "" {
			state = "open"
		}
		repo.mu.RLock()
		issues := make([]any, 0, len(repo.Issues))
		for _, issue := range repo.Issues {
			if state == "all" || issue.State == state {
				issues = append(issues, s.issueJSONLocked(repo, issue))
			}
		}
		repo.mu.RUnlock()
		return toolContent(map[string]any{"issues": issues, "total_count": len(issues)}), 0, ""
	case "create_pull_request":
		if missing("owner", "repo", "title", "head", "base") {
			return nil, -32602, "Invalid params: owner, repo, title, head and base are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		head := stringArg(args, "head")
		base := stringArg(args, "base")
		repo.mu.Lock()
		headTip, headOK := repo.Refs["refs/heads/"+head]
		baseTip, baseOK := repo.Refs["refs/heads/"+base]
		if !headOK || !baseOK {
			repo.mu.Unlock()
			return toolError("head or base branch does not exist"), 0, ""
		}
		pull := repo.newPull(s.nextPullID(), stringArg(args, "title"), stringArg(args, "body"), head, base)
		pull.HeadSHA = headTip
		pull.BaseSHA = baseTip
		body := s.pullJSON(repo, pull)
		repo.mu.Unlock()
		return toolContent(body), 0, ""
	case "merge_pull_request":
		if missing("owner", "repo", "pull_number") {
			return nil, -32602, "Invalid params: owner, repo and pull_number are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		number := intArg(args, "pull_number")
		repo.mu.Lock()
		pull := repo.findPull(number)
		if pull == nil {
			repo.mu.Unlock()
			return toolError("pull request #" + strconv.Itoa(number) + " not found"), 0, ""
		}
		if !pull.Merged {
			author := s.user.Name + " <" + s.user.Email + ">"
			message := pull.Title + " (#" + strconv.Itoa(pull.Number) + ")\n"
			if _, ok := repo.mergeSquash(pull, message, author, 1735689600+int64(len(repo.Objects))); !ok {
				repo.mu.Unlock()
				return toolError("pull request is not mergeable"), 0, ""
			}
		}
		body := s.pullJSON(repo, pull)
		repo.mu.Unlock()
		return toolContent(body), 0, ""
	case "close_issue":
		if missing("owner", "repo", "issue_number") {
			return nil, -32602, "Invalid params: owner, repo and issue_number are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		repo.mu.Lock()
		issue := repo.findIssue(intArg(args, "issue_number"))
		if issue == nil {
			repo.mu.Unlock()
			return toolError("issue not found"), 0, ""
		}
		issue.State = "closed"
		issue.ClosedAt = baseTime
		invalidateIssue(issue)
		body := s.issueJSONLocked(repo, issue)
		repo.mu.Unlock()
		return toolContent(body), 0, ""
	case "add_comment":
		if missing("owner", "repo", "issue_number", "body") {
			return nil, -32602, "Invalid params: owner, repo, issue_number and body are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		repo.mu.Lock()
		issue := repo.findIssue(intArg(args, "issue_number"))
		if issue == nil {
			repo.mu.Unlock()
			return toolError("issue not found"), 0, ""
		}
		comment := &Comment{ID: s.nextCommentID(), Body: stringArg(args, "body"), Login: owner, Created: baseTime}
		issue.Comments = append(issue.Comments, comment)
		invalidateIssue(issue)
		body := s.commentJSON(repo, issue, comment)
		repo.mu.Unlock()
		return toolContent(body), 0, ""
	case "list_pulls":
		if missing("owner", "repo") {
			return nil, -32602, "Invalid params: owner and repo are required"
		}
		repo := s.findRepo(owner, repoName)
		if repo == nil {
			return toolError("repository " + owner + "/" + repoName + " not found"), 0, ""
		}
		state := stringArg(args, "state")
		if state == "" {
			state = "open"
		}
		repo.mu.RLock()
		pulls := make([]any, 0, len(repo.Pulls))
		for _, pull := range repo.Pulls {
			if state == "all" || pull.State == state {
				pulls = append(pulls, s.pullJSON(repo, pull))
			}
		}
		repo.mu.RUnlock()
		return toolContent(map[string]any{"pulls": pulls, "total_count": len(pulls)}), 0, ""
	}
	return nil, -32602, fmt.Sprintf("Unknown tool: %s", name)
}
