package main

// Pull requests and the Git Data API, the two REST surfaces the fixtures build on.

import (
	"net/http"
	"sort"
	"strconv"
	"strings"
)

func (s *Store) pullsREST(w http.ResponseWriter, r *http.Request, repo *Repo, rest []string) {
	if len(rest) == 0 {
		switch r.Method {
		case http.MethodPost:
			s.createPullREST(w, r, repo)
		case http.MethodGet:
			query := r.URL.Query()
			state := query.Get("state")
			if state == "" {
				state = "open"
			}
			perPage := atoiDefault(query.Get("per_page"), 30)
			if perPage <= 0 || perPage > 100 {
				perPage = 30
			}
			repo.mu.RLock()
			pulls := make([]*Pull, 0, len(repo.Pulls))
			for _, pull := range repo.Pulls {
				if state == "all" || pull.State == state {
					pulls = append(pulls, pull)
				}
			}
			sort.Slice(pulls, func(i, j int) bool { return pulls[i].Number < pulls[j].Number })
			if len(pulls) > perPage {
				pulls = pulls[len(pulls)-perPage:]
			}
			items := make([]any, 0, len(pulls))
			for _, pull := range pulls {
				items = append(items, s.pullJSON(repo, pull))
			}
			repo.mu.RUnlock()
			s.writeJSON(w, 200, items)
		default:
			s.fail(w, 404, "Not Found")
		}
		return
	}
	number, err := strconv.Atoi(rest[0])
	if err != nil || number <= 0 {
		s.fail(w, 404, "Not Found")
		return
	}
	if len(rest) == 1 {
		if r.Method != http.MethodGet {
			s.fail(w, 404, "Not Found")
			return
		}
		repo.mu.RLock()
		pull := repo.findPull(number)
		if pull == nil {
			repo.mu.RUnlock()
			s.fail(w, 404, "Not Found")
			return
		}
		body := s.pullJSON(repo, pull)
		repo.mu.RUnlock()
		s.writeJSON(w, 200, body)
		return
	}
	switch rest[1] {
	case "files":
		repo.mu.RLock()
		pull := repo.findPull(number)
		if pull == nil {
			repo.mu.RUnlock()
			s.fail(w, 404, "Not Found")
			return
		}
		files := repo.pullFiles(pull)
		items := make([]any, 0, len(files))
		for _, file := range files {
			items = append(items, map[string]any{
				"filename": file.Filename, "status": file.Status, "additions": file.Additions,
				"deletions": file.Deletions, "changes": file.Changes, "sha": file.SHA,
				"blob_url": "http://github.local/" + repo.Owner + "/" + repo.Name + "/blob/" + pull.HeadSHA + "/" + file.Filename,
				"raw_url":  "http://github.local/" + repo.Owner + "/" + repo.Name + "/raw/" + pull.HeadSHA + "/" + file.Filename,
			})
		}
		repo.mu.RUnlock()
		s.writeJSON(w, 200, items)
	case "merge":
		s.mergePullREST(w, r, repo, number)
	default:
		s.fail(w, 404, "Not Found")
	}
}

func (s *Store) createPullREST(w http.ResponseWriter, r *http.Request, repo *Repo) {
	var body struct {
		Title string `json:"title"`
		Body  string `json:"body"`
		Head  string `json:"head"`
		Base  string `json:"base"`
	}
	if !s.readJSON(w, r, &body) {
		return
	}
	head := body.Head
	if index := strings.Index(head, ":"); index >= 0 {
		head = head[index+1:]
	}
	if head == "" || body.Title == "" {
		s.fail(w, 422, "Validation Failed: head and title are required")
		return
	}
	repo.mu.Lock()
	if _, ok := repo.Refs["refs/heads/"+head]; !ok {
		repo.mu.Unlock()
		s.fail(w, 422, "Validation Failed: head branch does not exist")
		return
	}
	base := body.Base
	if base == "" {
		base = repo.DefaultBranch
	}
	baseTip, ok := repo.Refs["refs/heads/"+base]
	if !ok {
		repo.mu.Unlock()
		s.fail(w, 422, "Validation Failed: base branch does not exist")
		return
	}
	for _, existing := range repo.Pulls {
		if existing.HeadRef == head && existing.State == "open" && existing.BaseRef == base {
			repo.mu.Unlock()
			s.fail(w, 422, "Validation Failed: A pull request already exists for that head branch")
			return
		}
	}
	pull := repo.newPull(s.nextPullID(), body.Title, body.Body, head, base)
	pull.HeadSHA = repo.Refs["refs/heads/"+head]
	pull.BaseSHA = baseTip
	bodyJSON := s.pullJSON(repo, pull)
	repo.mu.Unlock()
	s.writeJSON(w, 201, bodyJSON)
}

func (s *Store) mergePullREST(w http.ResponseWriter, r *http.Request, repo *Repo, number int) {
	if r.Method != http.MethodPut && r.Method != http.MethodPost {
		s.fail(w, 404, "Not Found")
		return
	}
	var body struct {
		MergeMethod string `json:"merge_method"`
		CommitTitle string `json:"commit_title"`
	}
	if r.ContentLength > 0 && !s.readJSON(w, r, &body) {
		return
	}
	repo.mu.Lock()
	pull := repo.findPull(number)
	if pull == nil {
		repo.mu.Unlock()
		s.fail(w, 404, "Not Found")
		return
	}
	if pull.Merged {
		repo.mu.Unlock()
		s.fail(w, 405, "Pull Request is not mergeable")
		return
	}
	if _, ok := repo.Refs["refs/heads/"+pull.BaseRef]; !ok {
		repo.mu.Unlock()
		s.fail(w, 405, "Pull Request is not mergeable")
		return
	}
	if pull.HeadSHA == "" {
		pull.HeadSHA = repo.Refs["refs/heads/"+pull.HeadRef]
	}
	message := body.CommitTitle
	if message == "" {
		message = pull.Title + " (#" + strconv.Itoa(pull.Number) + ")\n"
	}
	if !strings.HasSuffix(message, "\n") {
		message += "\n"
	}
	author := s.user.Name + " <" + s.user.Email + ">"
	commit, ok := repo.mergeSquash(pull, message, author, 1735689600+int64(len(repo.Objects)))
	repo.mu.Unlock()
	if !ok {
		s.fail(w, 405, "Pull Request is not mergeable")
		return
	}
	s.writeJSON(w, 200, map[string]any{"sha": commit, "merged": true, "message": "Pull Request successfully merged"})
}

// ------------------------------------------------------------------ git data

func (s *Store) gitDataREST(w http.ResponseWriter, r *http.Request, repo *Repo, rest []string) {
	if len(rest) >= 1 && rest[0] == "refs" {
		if r.Method == http.MethodPost && len(rest) == 1 {
			var body struct {
				Ref string `json:"ref"`
				SHA string `json:"sha"`
			}
			if !s.readJSON(w, r, &body) {
				return
			}
			if !strings.HasPrefix(body.Ref, "refs/") || body.SHA == "" {
				s.fail(w, 422, "Validation Failed: ref and sha are required")
				return
			}
			repo.mu.Lock()
			if _, exists := repo.Refs[body.Ref]; exists {
				repo.mu.Unlock()
				s.fail(w, 422, "Reference already exists")
				return
			}
			if repo.Objects[body.SHA] == nil {
				repo.mu.Unlock()
				s.fail(w, 422, "Object does not exist")
				return
			}
			repo.setRef(body.Ref, body.SHA)
			repo.mu.Unlock()
			s.writeJSON(w, 201, map[string]any{
				"ref": body.Ref, "node_id": "REF_" + body.SHA,
				"url":    "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/git/" + body.Ref,
				"object": map[string]any{"sha": body.SHA, "type": "commit", "url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/git/commits/" + body.SHA},
			})
			return
		}
		if r.Method == http.MethodGet && len(rest) >= 2 {
			name := strings.Join(rest[1:], "/")
			repo.mu.RLock()
			oid, ok := repo.Refs[name]
			repo.mu.RUnlock()
			if !ok {
				s.fail(w, 404, "Not Found")
				return
			}
			s.writeJSON(w, 200, map[string]any{
				"ref": name, "node_id": "REF_" + oid, "url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/git/" + name,
				"object": map[string]any{"sha": oid, "type": "commit", "url": "/api/v3/repos/" + repo.Owner + "/" + repo.Name + "/git/commits/" + oid},
			})
			return
		}
	}
	if len(rest) >= 2 && rest[0] == "commits" && r.Method == http.MethodGet {
		oid := rest[1]
		repo.mu.RLock()
		object := repo.Objects[oid]
		if object == nil || object.Type != ObjCommit {
			repo.mu.RUnlock()
			s.fail(w, 404, "Not Found")
			return
		}
		view := parseCommit(object.Data)
		repo.mu.RUnlock()
		name, email := splitIdentity(view.Author)
		parents := make([]any, 0, len(view.Parents))
		for _, parent := range view.Parents {
			parents = append(parents, map[string]any{"sha": parent})
		}
		s.writeJSON(w, 200, map[string]any{
			"sha": oid, "node_id": "C_" + oid,
			"commit": map[string]any{
				"message":   view.Message,
				"author":    map[string]any{"name": name, "email": email, "date": baseTime},
				"committer": map[string]any{"name": name, "email": email, "date": baseTime},
				"tree":      map[string]any{"sha": view.Tree},
			},
			"parents": parents,
		})
		return
	}
	if len(rest) >= 2 && rest[0] == "blobs" && r.Method == http.MethodGet {
		repo.mu.RLock()
		object := repo.Objects[rest[1]]
		repo.mu.RUnlock()
		if object == nil {
			s.fail(w, 404, "Not Found")
			return
		}
		s.writeJSON(w, 200, map[string]any{"sha": rest[1], "size": len(object.Data), "encoding": "base64", "content": b64(object.Data)})
		return
	}
	s.fail(w, 404, "Not Found")
}
