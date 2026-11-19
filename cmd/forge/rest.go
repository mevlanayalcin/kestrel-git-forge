package main

// GitHub REST v3 under /api/v3: repositories, contents (real git commits), branches, commits,
// labels, issues, comments and pull requests with a real squash merge.

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
)

const jsonType = "application/json; charset=utf-8"

func (s *Store) requestID() string {
	n := s.requestSeq.Add(1)
	return fmt.Sprintf("%016x", n)
}

func (s *Store) writeJSON(w http.ResponseWriter, status int, value any) {
	body, err := json.Marshal(value)
	if err != nil {
		body = []byte(`{"message":"Internal Server Error"}`)
		status = 500
	}
	s.writeRaw(w, status, body)
}

func (s *Store) writeRaw(w http.ResponseWriter, status int, body []byte) {
	header := w.Header()
	header.Set("Content-Type", jsonType)
	header.Set("Content-Length", strconv.Itoa(len(body)))
	header.Set("X-GitHub-Request-Id", s.requestID())
	header.Set("X-GitHub-Api-Version", "2022-11-28")
	w.WriteHeader(status)
	w.Write(body)
}

func (s *Store) fail(w http.ResponseWriter, status int, message string) {
	s.writeJSON(w, status, map[string]any{"message": message, "documentation_url": "https://docs.github.com/rest"})
}

// readJSON decodes a request body; a malformed body is answered 400 and reported false.
func (s *Store) readJSON(w http.ResponseWriter, r *http.Request, target any) bool {
	body, err := io.ReadAll(io.LimitReader(r.Body, 8<<20))
	if err != nil {
		s.fail(w, 400, "Problems parsing JSON")
		return false
	}
	if len(strings.TrimSpace(string(body))) == 0 {
		return true
	}
	if err := json.Unmarshal(body, target); err != nil {
		s.fail(w, 400, "Problems parsing JSON")
		return false
	}
	return true
}

func (s *Store) repoJSON(repo *Repo) map[string]any {
	repo.mu.RLock()
	defer repo.mu.RUnlock()
	open := 0
	for _, issue := range repo.Issues {
		if issue.State == "open" {
			open++
		}
	}
	visibility := "public"
	if repo.Private {
		visibility = "private"
	}
	full := repo.Owner + "/" + repo.Name
	api := "/api/v3/repos/" + full
	return map[string]any{
		"id":                 repo.ID,
		"node_id":            "R_" + strconv.FormatInt(repo.ID, 10),
		"name":               repo.Name,
		"full_name":          full,
		"owner":              map[string]any{"login": repo.Owner, "id": 1000, "type": "User", "site_admin": false},
		"private":            repo.Private,
		"fork":               false,
		"description":        repo.Description,
		"default_branch":     repo.DefaultBranch,
		"created_at":         repo.Created,
		"updated_at":         repo.Updated,
		"pushed_at":          repo.Pushed,
		"has_issues":         true,
		"has_projects":       false,
		"has_wiki":           false,
		"has_downloads":      true,
		"open_issues_count":  open,
		"forks_count":        0,
		"stargazers_count":   0,
		"watchers_count":     0,
		"visibility":         visibility,
		"html_url":           "http://github.local/" + full,
		"url":                api,
		"clone_url":          "http://github.local/" + full + ".git",
		"git_url":            "git://github.local/" + full + ".git",
		"ssh_url":            "git@github.local:" + full + ".git",
		"archived":           false,
		"disabled":           false,
		"allow_squash_merge": true,
	}
}

func (s *Store) issueJSONLocked(repo *Repo, issue *Issue) map[string]any {
	labels := make([]any, 0, len(issue.Labels))
	for _, name := range issue.Labels {
		label := repo.Labels[name]
		color := "ededed"
		id := int64(0)
		if label != nil {
			color = label.Color
			id = label.ID
		}
		labels = append(labels, map[string]any{"id": id, "name": name, "color": color, "default": false})
	}
	var closed any
	if issue.State == "closed" {
		closed = issue.ClosedAt
	}
	return map[string]any{
		"id":             issue.ID,
		"node_id":        "I_" + strconv.FormatInt(issue.ID, 10),
		"number":         issue.Number,
		"title":          issue.Title,
		"body":           issue.Body,
		"state":          issue.State,
		"state_reason":   nil,
		"locked":         issue.Locked,
		"labels":         labels,
		"user":           map[string]any{"login": repo.Owner, "id": 1000, "type": "User"},
		"assignee":       nil,
		"assignees":      []any{},
		"milestone":      nil,
		"comments":       len(issue.Comments),
		"created_at":     issue.Created,
		"updated_at":     issue.Updated,
		"closed_at":      closed,
		"closed_by":      nil,
		"html_url":       "http://github.local/" + repo.Owner + "/" + repo.Name + "/issues/" + strconv.Itoa(issue.Number),
		"url":            "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/issues/" + strconv.Itoa(issue.Number),
		"repository_url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name,
	}
}

func (s *Store) issueJSON(repo *Repo, issue *Issue) map[string]any {
	return s.issueJSONLocked(repo, issue)
}

// issueBodyLocked returns the marshaled issue, cached until the issue changes. Callers hold the
// repository lock; invalidateIssue drops the cache on every mutation.
func (s *Store) issueBodyLocked(repo *Repo, issue *Issue) []byte {
	if issue.cached != nil {
		return issue.cached
	}
	body, err := json.Marshal(s.issueJSONLocked(repo, issue))
	if err != nil {
		return []byte(`{"message":"Internal Server Error"}`)
	}
	issue.cached = body
	return body
}

func invalidateIssue(issue *Issue) {
	issue.cached = nil
}

func (s *Store) commentJSON(repo *Repo, issue *Issue, comment *Comment) map[string]any {
	return map[string]any{
		"id":         comment.ID,
		"node_id":    "C_" + strconv.FormatInt(comment.ID, 10),
		"body":       comment.Body,
		"user":       map[string]any{"login": repo.Owner, "id": 1000, "type": "User"},
		"created_at": comment.Created,
		"updated_at": comment.Created,
		"url":        "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/issues/" + strconv.Itoa(issue.Number) + "/comments/" + strconv.FormatInt(comment.ID, 10),
		"issue_url":  "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/issues/" + strconv.Itoa(issue.Number),
	}
}

func (s *Store) pullJSON(repo *Repo, pull *Pull) map[string]any {
	headSHA := pull.HeadSHA
	baseSHA := pull.BaseSHA
	if oid, ok := repo.Refs["refs/heads/"+pull.BaseRef]; ok {
		baseSHA = oid
	}
	var mergedAt any
	if pull.Merged {
		mergedAt = pull.MergedAt
	}
	mergeable := pull.Mergeable == "MERGEABLE"
	state := pull.State
	label := repo.Owner + ":" + pull.HeadRef
	return map[string]any{
		"id":                    pull.ID,
		"node_id":               "PR_" + strconv.FormatInt(pull.ID, 10),
		"number":                pull.Number,
		"state":                 state,
		"title":                 pull.Title,
		"body":                  pull.Body,
		"user":                  map[string]any{"login": repo.Owner, "id": 1000, "type": "User"},
		"created_at":            pull.Created,
		"updated_at":            pull.Updated,
		"closed_at":             nil,
		"merged_at":             mergedAt,
		"merge_commit_sha":      pull.MergeSHA,
		"merged":                pull.Merged,
		"draft":                 false,
		"mergeable":             mergeable,
		"mergeable_state":       pull.Mergeable,
		"merged_by":             nil,
		"comments":              0,
		"commits":               1,
		"additions":             0,
		"deletions":             0,
		"changed_files":         len(repo.pullFiles(pull)),
		"head":                  map[string]any{"ref": pull.HeadRef, "label": label, "sha": headSHA, "user": map[string]any{"login": repo.Owner, "id": 1000, "type": "User"}},
		"base":                  map[string]any{"ref": pull.BaseRef, "label": repo.Owner + ":" + pull.BaseRef, "sha": baseSHA, "user": map[string]any{"login": repo.Owner, "id": 1000, "type": "User"}},
		"html_url":              "http://github.local/" + repo.Owner + "/" + repo.Name + "/pull/" + strconv.Itoa(pull.Number),
		"url":                   "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/pulls/" + strconv.Itoa(pull.Number),
		"diff_url":              "http://github.local/" + repo.Owner + "/" + repo.Name + "/pull/" + strconv.Itoa(pull.Number) + ".diff",
		"patch_url":             "http://github.local/" + repo.Owner + "/" + repo.Name + "/pull/" + strconv.Itoa(pull.Number) + ".patch",
		"review_comments":       0,
		"maintainer_can_modify": true,
	}
}

// ------------------------------------------------------------------ routing

func (s *Store) handleREST(w http.ResponseWriter, r *http.Request, path string) {
	parts := strings.Split(strings.Trim(path, "/"), "/")
	if len(parts) == 0 || parts[0] == "" {
		w.Header().Set("X-OAuth-Scopes", "repo, read:org")
		s.writeJSON(w, 200, map[string]any{
			"current_user_url":      "/api/v3/user",
			"rate_limit_url":        "/api/v3/rate_limit",
			"repository_url":        "/api/v3/repos/{owner}/{repo}",
			"user_repositories_url": "/api/v3/users/{user}/repos{?type,page,per_page,sort}",
			"organization_url":      "/api/v3/orgs/{org}",
			"emojis_url":            "/api/v3/emojis",
		})
		return
	}
	switch parts[0] {
	case "meta":
		s.writeJSON(w, 200, map[string]any{"installed_version": "1.0.0", "verifiable_password_authentication": false})
	case "user":
		if len(parts) == 1 {
			s.writeJSON(w, 200, map[string]any{
				"login": s.user.Login, "id": s.user.ID, "node_id": "U_1000", "type": "User",
				"name": s.user.Name, "email": s.user.Email, "site_admin": false,
				"html_url": "http://github.local/" + s.user.Login, "url": "/api/v3/user",
				"repos_url": "/api/v3/user/repos", "public_repos": len(s.listRepos(s.user.Login)),
			})
			return
		}
		if parts[1] == "repos" && len(parts) == 2 {
			if r.Method == http.MethodPost {
				s.createRepoREST(w, r)
				return
			}
			s.listReposREST(w, r)
			return
		}
		s.fail(w, 404, "Not Found")
	case "repos":
		s.handleRepoREST(w, r, parts[1:])
	default:
		s.fail(w, 404, "Not Found")
	}
}

func (s *Store) createRepoREST(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name        string `json:"name"`
		Description string `json:"description"`
		Private     bool   `json:"private"`
	}
	if !s.readJSON(w, r, &body) {
		return
	}
	if strings.TrimSpace(body.Name) == "" {
		s.fail(w, 422, "Repository creation failed: name is required")
		return
	}
	repo, ok := s.createRepo(s.user.Login, body.Name, body.Description, body.Private)
	if !ok {
		s.fail(w, 422, "Repository creation failed: a repository with that name already exists")
		return
	}
	s.writeJSON(w, 201, s.repoJSON(repo))
}

func (s *Store) listReposREST(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query()
	page := atoiDefault(query.Get("page"), 1)
	perPage := atoiDefault(query.Get("per_page"), 30)
	if perPage <= 0 {
		perPage = 30
	}
	if perPage > 100 {
		perPage = 100
	}
	if page < 1 {
		page = 1
	}
	repos := s.listRepos(s.user.Login)
	total := len(repos)
	start := (page - 1) * perPage
	if start > total {
		start = total
	}
	end := start + perPage
	if end > total {
		end = total
	}
	items := make([]any, 0, end-start)
	for _, repo := range repos[start:end] {
		items = append(items, s.repoJSON(repo))
	}
	if end < total {
		base := "http://" + r.Host + "/api/v3/user/repos"
		values := url.Values{}
		values.Set("per_page", strconv.Itoa(perPage))
		values.Set("page", strconv.Itoa(page+1))
		w.Header().Set("Link", "<"+base+"?"+values.Encode()+`>; rel="next", <`+base+`?page=1>; rel="first"`)
	}
	s.writeJSON(w, 200, items)
}

func (s *Store) handleRepoREST(w http.ResponseWriter, r *http.Request, parts []string) {
	if len(parts) < 2 {
		s.fail(w, 404, "Not Found")
		return
	}
	owner, name := parts[0], parts[1]
	repo := s.findRepo(owner, name)
	if repo == nil {
		s.fail(w, 404, "Not Found")
		return
	}
	rest := parts[2:]
	if len(rest) == 0 {
		switch r.Method {
		case http.MethodGet:
			s.writeJSON(w, 200, s.repoJSON(repo))
		case http.MethodPatch:
			var body struct {
				Description *string `json:"description"`
				Private     *bool   `json:"private"`
			}
			if !s.readJSON(w, r, &body) {
				return
			}
			repo.mu.Lock()
			if body.Description != nil {
				repo.Description = *body.Description
			}
			if body.Private != nil {
				repo.Private = *body.Private
			}
			repo.mu.Unlock()
			s.writeJSON(w, 200, s.repoJSON(repo))
		case http.MethodDelete:
			s.deleteRepo(owner, name)
			w.Header().Set("X-GitHub-Request-Id", s.requestID())
			w.WriteHeader(204)
		default:
			s.fail(w, 404, "Not Found")
		}
		return
	}
	switch rest[0] {
	case "branches":
		s.branchesREST(w, r, repo, rest[1:])
	case "commits":
		s.commitsREST(w, r, repo)
	case "contents":
		s.contentsREST(w, r, repo, rest[1:])
	case "labels":
		s.labelsREST(w, r, repo)
	case "issues":
		s.issuesREST(w, r, repo, rest[1:])
	case "pulls":
		s.pullsREST(w, r, repo, rest[1:])
	case "git":
		s.gitDataREST(w, r, repo, rest[1:])
	default:
		s.fail(w, 404, "Not Found")
	}
}

func (s *Store) branchesREST(w http.ResponseWriter, r *http.Request, repo *Repo, rest []string) {
	repo.mu.RLock()
	refs := make([]string, 0, len(repo.Refs))
	for name := range repo.Refs {
		if strings.HasPrefix(name, "refs/heads/") {
			refs = append(refs, name)
		}
	}
	sort.Strings(refs)
	defer repo.mu.RUnlock()
	if len(rest) > 0 {
		name := strings.Join(rest, "/")
		oid, ok := repo.Refs["refs/heads/"+name]
		if !ok {
			s.fail(w, 404, "Branch not found")
			return
		}
		s.writeJSON(w, 200, map[string]any{"name": name, "commit": map[string]any{"sha": oid}, "protected": false})
		return
	}
	items := make([]any, 0, len(refs))
	for _, ref := range refs {
		items = append(items, map[string]any{
			"name":      strings.TrimPrefix(ref, "refs/heads/"),
			"commit":    map[string]any{"sha": repo.Refs[ref], "url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/commits/" + repo.Refs[ref]},
			"protected": false,
		})
	}
	s.writeJSON(w, 200, items)
}

func (s *Store) commitsREST(w http.ResponseWriter, r *http.Request, repo *Repo) {
	query := r.URL.Query()
	sha := query.Get("sha")
	perPage := atoiDefault(query.Get("per_page"), 30)
	if perPage <= 0 || perPage > 100 {
		perPage = 30
	}
	repo.mu.RLock()
	tip := ""
	if sha == "" {
		tip = repo.Refs["refs/heads/"+repo.DefaultBranch]
	} else if resolved, ok := repo.resolveRef(sha); ok {
		tip = resolved
	}
	commits := repo.walkCommits(tip, perPage)
	items := make([]any, 0, len(commits))
	for _, oid := range commits {
		object := repo.Objects[oid]
		if object == nil {
			continue
		}
		view := parseCommit(object.Data)
		view.Message = strings.TrimSuffix(view.Message, "\n")
		name, email := splitIdentity(view.Author)
		parents := make([]any, 0, len(view.Parents))
		for _, parent := range view.Parents {
			parents = append(parents, map[string]any{"sha": parent})
		}
		items = append(items, map[string]any{
			"sha":     oid,
			"node_id": "C_" + oid,
			"commit": map[string]any{
				"message":   view.Message,
				"author":    map[string]any{"name": name, "email": email, "date": baseTime},
				"committer": map[string]any{"name": name, "email": email, "date": baseTime},
				"tree":      map[string]any{"sha": view.Tree},
			},
			"parents": parents,
			"url":     "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/commits/" + oid,
		})
	}
	repo.mu.RUnlock()
	s.writeJSON(w, 200, items)
}

func splitIdentity(author string) (string, string) {
	author = strings.TrimSpace(author)
	open := strings.Index(author, "<")
	closeAt := strings.Index(author, ">")
	if open > 0 && closeAt > open {
		return strings.TrimSpace(author[:open]), strings.TrimSpace(author[open+1 : closeAt])
	}
	return author, ""
}

func (s *Store) labelsREST(w http.ResponseWriter, r *http.Request, repo *Repo) {
	if r.Method != http.MethodPost {
		repo.mu.RLock()
		items := make([]any, 0, len(repo.Labels))
		for _, label := range repo.Labels {
			items = append(items, map[string]any{"id": label.ID, "name": label.Name, "color": label.Color, "default": false})
		}
		repo.mu.RUnlock()
		s.writeJSON(w, 200, items)
		return
	}
	var body struct {
		Name  string `json:"name"`
		Color string `json:"color"`
	}
	if !s.readJSON(w, r, &body) {
		return
	}
	if body.Name == "" {
		s.fail(w, 422, "Validation Failed: name is required")
		return
	}
	repo.mu.Lock()
	label := repo.Labels[body.Name]
	if label == nil {
		repo.LabelSeq++
		label = &Label{ID: s.nextCommentID(), Name: body.Name, Color: body.Color}
		if label.Color == "" {
			label.Color = "ededed"
		}
		repo.Labels[body.Name] = label
	}
	repo.mu.Unlock()
	s.writeJSON(w, 201, map[string]any{"id": label.ID, "name": label.Name, "color": label.Color, "default": false})
}

func (s *Store) issuesREST(w http.ResponseWriter, r *http.Request, repo *Repo, rest []string) {
	if len(rest) > 0 {
		number, err := strconv.Atoi(rest[0])
		if err != nil || number <= 0 {
			s.fail(w, 404, "Not Found")
			return
		}
		if len(rest) > 1 && rest[1] == "comments" {
			s.commentsREST(w, r, repo, number)
			return
		}
		s.issueREST(w, r, repo, number)
		return
	}
	if r.Method == http.MethodPost {
		s.createIssueREST(w, r, repo)
		return
	}
	if r.Method != http.MethodGet {
		s.fail(w, 404, "Not Found")
		return
	}
	query := r.URL.Query()
	state := query.Get("state")
	if state == "" {
		state = "open"
	}
	perPage := atoiDefault(query.Get("per_page"), 30)
	if perPage <= 0 {
		perPage = 30
	}
	if perPage > 100 {
		perPage = 100
	}
	page := atoiDefault(query.Get("page"), 1)
	if page < 1 {
		page = 1
	}
	repo.mu.RLock()
	issues := make([]*Issue, 0, len(repo.Issues))
	for _, issue := range repo.Issues {
		if state == "all" || issue.State == state {
			issues = append(issues, issue)
		}
	}
	sort.Slice(issues, func(i, j int) bool { return issues[i].Number < issues[j].Number })
	if strings.EqualFold(query.Get("direction"), "desc") || query.Get("direction") == "" {
		for i, j := 0, len(issues)-1; i < j; i, j = i+1, j-1 {
			issues[i], issues[j] = issues[j], issues[i]
		}
	}
	start := (page - 1) * perPage
	if start > len(issues) {
		start = len(issues)
	}
	end := start + perPage
	if end > len(issues) {
		end = len(issues)
	}
	items := make([]any, 0, end-start)
	for _, issue := range issues[start:end] {
		items = append(items, s.issueJSONLocked(repo, issue))
	}
	repo.mu.RUnlock()
	s.writeJSON(w, 200, items)
}

func (s *Store) issueREST(w http.ResponseWriter, r *http.Request, repo *Repo, number int) {
	switch r.Method {
	case http.MethodGet:
		repo.mu.RLock()
		issue := repo.findIssue(number)
		if issue == nil {
			repo.mu.RUnlock()
			s.fail(w, 404, "Not Found")
			return
		}
		body := s.issueBodyLocked(repo, issue)
		repo.mu.RUnlock()
		s.writeRaw(w, 200, body)
	case http.MethodPatch:
		var body struct {
			Title  *string  `json:"title"`
			Body   *string  `json:"body"`
			State  *string  `json:"state"`
			Labels []string `json:"labels"`
		}
		if !s.readJSON(w, r, &body) {
			return
		}
		repo.mu.Lock()
		issue := repo.findIssue(number)
		if issue == nil {
			repo.mu.Unlock()
			s.fail(w, 404, "Not Found")
			return
		}
		if body.Title != nil {
			issue.Title = *body.Title
		}
		if body.Body != nil {
			issue.Body = *body.Body
		}
		if body.State != nil {
			if *body.State == "closed" {
				issue.State = "closed"
				issue.ClosedAt = baseTime
			} else if *body.State == "open" {
				issue.State = "open"
				issue.ClosedAt = ""
			}
		}
		if body.Labels != nil {
			issue.Labels = body.Labels
		}
		issue.Updated = baseTime
		invalidateIssue(issue)
		result := s.issueBodyLocked(repo, issue)
		repo.mu.Unlock()
		s.writeRaw(w, 200, result)
	default:
		s.fail(w, 404, "Not Found")
	}
}

func (s *Store) createIssueREST(w http.ResponseWriter, r *http.Request, repo *Repo) {
	var body struct {
		Title  string   `json:"title"`
		Body   string   `json:"body"`
		Labels []string `json:"labels"`
	}
	if !s.readJSON(w, r, &body) {
		return
	}
	if strings.TrimSpace(body.Title) == "" {
		s.fail(w, 422, "Validation Failed: title is required")
		return
	}
	repo.mu.Lock()
	for _, name := range body.Labels {
		if repo.Labels[name] == nil {
			repo.mu.Unlock()
			s.fail(w, 422, "Validation Failed: label "+name+" does not exist")
			return
		}
	}
	issue := repo.newIssue(s.nextIssueID(), body.Title, body.Body, body.Labels)
	result := s.issueBodyLocked(repo, issue)
	repo.mu.Unlock()
	s.writeRaw(w, 201, result)
}

func (s *Store) commentsREST(w http.ResponseWriter, r *http.Request, repo *Repo, number int) {
	switch r.Method {
	case http.MethodPost:
		var body struct {
			Body string `json:"body"`
		}
		if !s.readJSON(w, r, &body) {
			return
		}
		repo.mu.Lock()
		issue := repo.findIssue(number)
		if issue == nil {
			repo.mu.Unlock()
			s.fail(w, 404, "Not Found")
			return
		}
		repo.CommentSeq++
		comment := &Comment{ID: s.nextCommentID(), Body: body.Body, Login: repo.Owner, Created: baseTime}
		issue.Comments = append(issue.Comments, comment)
		issue.Updated = baseTime
		invalidateIssue(issue)
		result := s.commentJSON(repo, issue, comment)
		repo.mu.Unlock()
		s.writeJSON(w, 201, result)
	case http.MethodGet:
		repo.mu.RLock()
		issue := repo.findIssue(number)
		if issue == nil {
			repo.mu.RUnlock()
			s.fail(w, 404, "Not Found")
			return
		}
		items := make([]any, 0, len(issue.Comments))
		for _, comment := range issue.Comments {
			items = append(items, s.commentJSON(repo, issue, comment))
		}
		repo.mu.RUnlock()
		s.writeJSON(w, 200, items)
	default:
		s.fail(w, 404, "Not Found")
	}
}

// ------------------------------------------------------------------ contents

func (s *Store) contentsREST(w http.ResponseWriter, r *http.Request, repo *Repo, rest []string) {
	path := strings.Join(rest, "/")
	switch r.Method {
	case http.MethodGet:
		ref := r.URL.Query().Get("ref")
		repo.mu.RLock()
		files, _, ok := repo.treeFilesAt(ref)
		if !ok {
			repo.mu.RUnlock()
			s.fail(w, 404, "Not Found")
			return
		}
		if entry, exists := files[path]; exists {
			object := repo.Objects[entry.OID]
			if object == nil {
				repo.mu.RUnlock()
				s.fail(w, 404, "Not Found")
				return
			}
			item := map[string]any{
				"type": "file", "encoding": "base64", "size": len(object.Data),
				"name": lastSegment(path), "path": path, "sha": entry.OID,
				"content": b64(object.Data), "url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/contents/" + path,
				"html_url":     "http://github.local/" + repo.Owner + "/" + repo.Name + "/blob/" + repo.DefaultBranch + "/" + path,
				"git_url":      "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/git/blobs/" + entry.OID,
				"download_url": "http://github.local/" + repo.Owner + "/" + repo.Name + "/raw/" + repo.DefaultBranch + "/" + path,
			}
			repo.mu.RUnlock()
			s.writeJSON(w, 200, item)
			return
		}
		prefix := path
		if prefix != "" && !strings.HasSuffix(prefix, "/") {
			prefix += "/"
		}
		directories := map[string]bool{}
		names := make([]string, 0, 8)
		for filePath, entry := range files {
			if !strings.HasPrefix(filePath, prefix) {
				continue
			}
			remainder := filePath[len(prefix):]
			if remainder == "" {
				continue
			}
			if strings.Contains(remainder, "/") {
				dir := remainder[:strings.Index(remainder, "/")]
				if !directories[dir] {
					directories[dir] = true
					names = append(names, dir)
				}
				continue
			}
			names = append(names, remainder)
			_ = entry
		}
		if len(names) == 0 {
			repo.mu.RUnlock()
			s.fail(w, 404, "Not Found")
			return
		}
		sort.Strings(names)
		items := make([]any, 0, len(names))
		for _, name := range names {
			childPath := prefix + name
			if directories[name] {
				items = append(items, map[string]any{
					"type": "dir", "name": name, "path": childPath, "sha": entrySHA(files, repo, childPath),
					"url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/contents/" + childPath,
				})
				continue
			}
			entry := files[childPath]
			size := 0
			if object := repo.Objects[entry.OID]; object != nil {
				size = len(object.Data)
			}
			items = append(items, map[string]any{
				"type": "file", "name": name, "path": childPath, "sha": entry.OID, "size": size,
				"url":          "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/contents/" + childPath,
				"download_url": "http://github.local/" + repo.Owner + "/" + repo.Name + "/raw/" + repo.DefaultBranch + "/" + childPath,
			})
		}
		repo.mu.RUnlock()
		s.writeJSON(w, 200, items)
	case http.MethodPut:
		var body struct {
			Message string `json:"message"`
			Content string `json:"content"`
			Branch  string `json:"branch"`
			SHA     string `json:"sha"`
		}
		if !s.readJSON(w, r, &body) {
			return
		}
		data, err := decodeBase64(body.Content)
		if err != nil {
			s.fail(w, 422, "Validation Failed: content is not valid base64")
			return
		}
		if body.Message == "" {
			body.Message = "Update " + path
		}
		branch := body.Branch
		if branch == "" {
			branch = repo.DefaultBranch
		}
		repo.mu.Lock()
		refName := "refs/heads/" + branch
		parent, hasParent := repo.Refs[refName]
		files := map[string]TreeEntry{}
		if hasParent {
			files = repo.commitFiles(parent)
		}
		existing, existed := files[path]
		if body.SHA != "" {
			if !existed || existing.OID != body.SHA {
				repo.mu.Unlock()
				s.fail(w, 409, "sha does not match the current file (409 Conflict)")
				return
			}
		} else if existed {
			repo.mu.Unlock()
			s.fail(w, 422, "Invalid request: sha is required to update an existing file")
			return
		}
		blobOID := repo.putObject(ObjBlob, data)
		files[path] = TreeEntry{Mode: "100644", Name: lastSegment(path), OID: blobOID}
		tree := repo.writeTreeFromFiles(files)
		author := fmt.Sprintf("%s <%s>", s.user.Name, s.user.Email)
		message := body.Message
		if !strings.HasSuffix(message, "\n") {
			message += "\n"
		}
		var parents []string
		if hasParent {
			parents = []string{parent}
		}
		commit := repo.putObject(ObjCommit, buildCommit(CommitFields{
			Tree: tree, Parents: parents, Author: author, Committer: author,
			Timestamp: 1735689600 + int64(len(repo.Objects)), Message: message,
		}))
		repo.setRef(refName, commit)
		status := 201
		if existed {
			status = 200
		}
		result := map[string]any{
			"content": map[string]any{
				"type": "file", "encoding": "base64", "size": len(data), "name": lastSegment(path), "path": path, "sha": blobOID,
				"content": body.Content, "url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/contents/" + path,
			},
			"commit": map[string]any{
				"sha": commit, "node_id": "C_" + commit, "message": message,
				"author":    map[string]any{"name": s.user.Name, "email": s.user.Email, "date": baseTime},
				"committer": map[string]any{"name": s.user.Name, "email": s.user.Email, "date": baseTime},
				"tree":      map[string]any{"sha": tree},
				"parents":   parentList(parents),
			},
		}
		repo.mu.Unlock()
		s.writeJSON(w, status, result)
	case http.MethodDelete:
		var body struct {
			Message string `json:"message"`
			SHA     string `json:"sha"`
			Branch  string `json:"branch"`
		}
		if !s.readJSON(w, r, &body) {
			return
		}
		branch := body.Branch
		if branch == "" {
			branch = repo.DefaultBranch
		}
		repo.mu.Lock()
		refName := "refs/heads/" + branch
		parent, hasParent := repo.Refs[refName]
		if !hasParent {
			repo.mu.Unlock()
			s.fail(w, 404, "Not Found")
			return
		}
		files := repo.commitFiles(parent)
		existing, existed := files[path]
		if !existed {
			repo.mu.Unlock()
			s.fail(w, 404, "Not Found")
			return
		}
		if body.SHA != "" && existing.OID != body.SHA {
			repo.mu.Unlock()
			s.fail(w, 409, "sha does not match the current file (409 Conflict)")
			return
		}
		delete(files, path)
		tree := repo.writeTreeFromFiles(files)
		author := fmt.Sprintf("%s <%s>", s.user.Name, s.user.Email)
		message := body.Message
		if message == "" {
			message = "Delete " + path
		}
		if !strings.HasSuffix(message, "\n") {
			message += "\n"
		}
		commit := repo.putObject(ObjCommit, buildCommit(CommitFields{
			Tree: tree, Parents: []string{parent}, Author: author, Committer: author,
			Timestamp: 1735689600 + int64(len(repo.Objects)), Message: message,
		}))
		repo.setRef(refName, commit)
		repo.mu.Unlock()
		s.writeJSON(w, 200, map[string]any{
			"content": nil,
			"commit": map[string]any{
				"sha": commit, "node_id": "C_" + commit, "message": message,
				"author":    map[string]any{"name": s.user.Name, "email": s.user.Email, "date": baseTime},
				"committer": map[string]any{"name": s.user.Name, "email": s.user.Email, "date": baseTime},
				"tree":      map[string]any{"sha": tree},
				"parents":   parentList([]string{parent}),
			},
		})
	default:
		s.fail(w, 404, "Not Found")
	}
}

func entrySHA(files map[string]TreeEntry, repo *Repo, path string) string {
	// A directory entry's sha is opaque to the checks; derive it from the path so it is stable.
	return objectID(ObjTree, []byte(path))
}

func parentList(parents []string) []any {
	items := make([]any, 0, len(parents))
	for _, parent := range parents {
		items = append(items, map[string]any{"sha": parent})
	}
	return items
}

func decodeBase64(text string) ([]byte, error) {
	clean := strings.Map(func(r rune) rune {
		switch r {
		case '\r', '\n', '\t', ' ':
			return -1
		}
		return r
	}, text)
	return base64.StdEncoding.DecodeString(clean)
}

func lastSegment(path string) string {
	if index := strings.LastIndex(path, "/"); index >= 0 {
		return path[index+1:]
	}
	return path
}

func atoiDefault(text string, fallback int) int {
	if text == "" {
		return fallback
	}
	value, err := strconv.Atoi(text)
	if err != nil {
		return fallback
	}
	return value
}
