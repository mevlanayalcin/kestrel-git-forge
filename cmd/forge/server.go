package main

// The HTTP router: health, REST, GraphQL, MCP and the git smart-HTTP endpoints.

import (
	"io"
	"net/http"
	"strconv"
	"strings"
)

func (s *Store) authorized(r *http.Request) bool {
	if s.token == "" {
		return true
	}
	header := r.Header.Get("Authorization")
	if header == "" {
		return false
	}
	value := strings.TrimSpace(strings.TrimPrefix(header, "Bearer "))
	return value == s.token
}

func (s *Store) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	switch {
	case path == "/healthz":
		s.writeJSON(w, 200, map[string]any{"ok": true})
	case path == "/api/graphql":
		s.serveGraphQL(w, r)
	case path == "/mcp":
		if !s.authorized(r) {
			w.Header().Set("Content-Type", jsonType)
			w.WriteHeader(401)
			w.Write([]byte(`{"message":"Requires authentication"}`))
			return
		}
		s.handleMCP(w, r)
	case path == "/api/v3" || strings.HasPrefix(path, "/api/v3/"):
		if !s.authorized(r) {
			s.fail(w, 401, "Requires authentication")
			return
		}
		s.handleREST(w, r, strings.TrimPrefix(path, "/api/v3"))
	case strings.Contains(path, ".git/"):
		s.handleGit(w, r, path)
	default:
		s.fail(w, 404, "Not Found")
	}
}

func (s *Store) serveGraphQL(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		s.fail(w, 405, "Method Not Allowed")
		return
	}
	if !s.authorized(r) {
		s.fail(w, 401, "Requires authentication")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 8<<20))
	if err != nil {
		s.fail(w, 400, "Problems parsing JSON")
		return
	}
	status, response := s.handleGraphQLBody(body)
	s.writeJSON(w, status, response)
}

// ------------------------------------------------------------------ git smart HTTP

func (s *Store) handleGit(w http.ResponseWriter, r *http.Request, path string) {
	index := strings.Index(path, ".git/")
	if index < 0 {
		s.fail(w, 404, "Not Found")
		return
	}
	name := strings.TrimPrefix(path[:index], "/")
	segments := strings.SplitN(name, "/", 2)
	if len(segments) != 2 || segments[0] == "" || segments[1] == "" {
		s.fail(w, 404, "Not Found")
		return
	}
	repo := s.findRepo(segments[0], segments[1])
	if repo == nil {
		s.fail(w, 404, "Not Found")
		return
	}
	suffix := path[index+5:]
	switch suffix {
	case "info/refs":
		s.gitInfoRefs(w, r, repo)
	case "git-upload-pack":
		s.gitUploadPack(w, r, repo)
	case "git-receive-pack":
		s.gitReceivePack(w, r, repo)
	default:
		s.fail(w, 404, "Not Found")
	}
}

func writeGitBody(w http.ResponseWriter, contentType string, body []byte) {
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(200)
	w.Write(body)
}

func (s *Store) gitInfoRefs(w http.ResponseWriter, r *http.Request, repo *Repo) {
	if r.Method != http.MethodGet {
		s.fail(w, 404, "Not Found")
		return
	}
	service := r.URL.Query().Get("service")
	switch service {
	case "git-upload-pack":
		if strings.Contains(r.Header.Get("Git-Protocol"), "version=2") {
			writeGitBody(w, "application/x-git-upload-pack-advertisement", advertisementV2())
			return
		}
		writeGitBody(w, "application/x-git-upload-pack-advertisement", cachedAdvertisement(repo, service))
	case "git-receive-pack":
		writeGitBody(w, "application/x-git-receive-pack-advertisement", cachedAdvertisement(repo, service))
	default:
		s.fail(w, 404, "Not Found")
	}
}

func (s *Store) gitUploadPack(w http.ResponseWriter, r *http.Request, repo *Repo) {
	if r.Method != http.MethodPost {
		s.fail(w, 404, "Not Found")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 64<<20))
	if err != nil {
		s.fail(w, 400, "Bad Request")
		return
	}
	v2 := strings.Contains(r.Header.Get("Git-Protocol"), "version=2")
	if !v2 {
		wants, haves, shallow, depth, parseErr := parseFetchArgs(body)
		if parseErr != nil {
			s.fail(w, 400, "Bad Request")
			return
		}
		writeGitBody(w, "application/x-git-upload-pack-result", buildFetchV0(repo, wants, haves, shallow, depth))
		return
	}
	lines, _, parseErr := splitPktLines(body)
	if parseErr != nil || len(lines) == 0 {
		s.fail(w, 400, "Bad Request")
		return
	}
	command := strings.TrimSpace(firstLine(lines))
	switch {
	case strings.HasPrefix(command, "command=ls-refs"):
		args := make([]string, 0, len(lines))
		for _, line := range lines[1:] {
			args = append(args, strings.TrimSpace(string(line)))
		}
		if isDefaultLsRefs(args) {
			writeGitBody(w, "application/x-git-upload-pack-result", cachedLsRefs(repo, args))
			return
		}
		writeGitBody(w, "application/x-git-upload-pack-result", lsRefs(repo, args))
	case strings.HasPrefix(command, "command=fetch"):
		wants := []string{}
		haves := []string{}
		clientShallow := []string{}
		depth := 0
		for _, line := range lines[1:] {
			text := strings.TrimSpace(string(line))
			switch {
			case strings.HasPrefix(text, "want "):
				fields := strings.Fields(text)
				if len(fields) >= 2 {
					wants = append(wants, fields[1])
				}
			case strings.HasPrefix(text, "have "):
				fields := strings.Fields(text)
				if len(fields) >= 2 {
					haves = append(haves, fields[1])
				}
			case strings.HasPrefix(text, "shallow "):
				fields := strings.Fields(text)
				if len(fields) >= 2 {
					clientShallow = append(clientShallow, fields[1])
				}
			case strings.HasPrefix(text, "deepen "):
				if value, err := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(text, "deepen "))); err == nil {
					depth = value
				}
			}
		}
		if len(wants) == 0 {
			writeGitBody(w, "application/x-git-upload-pack-result", pktFlush)
			return
		}
		if depth == 1 && len(wants) == 1 && len(haves) == 0 {
			writeGitBody(w, "application/x-git-upload-pack-result", repo.cachedFetchV2(wants[0], depth))
			return
		}
		result := buildFetchV2Shallow(repo, wants, haves, clientShallow, depth)
		writeGitBody(w, "application/x-git-upload-pack-result", result.body)
	default:
		s.fail(w, 400, "Bad Request")
	}
}

func parseFetchArgs(body []byte) (wants, haves, shallow []string, depth int, err error) {
	lines, _, err := splitPktLines(body)
	if err != nil {
		return nil, nil, nil, 0, err
	}
	for _, line := range lines {
		text := strings.TrimSpace(string(line))
		switch {
		case strings.HasPrefix(text, "want "):
			fields := strings.Fields(text)
			if len(fields) >= 2 {
				wants = append(wants, fields[1])
			}
		case strings.HasPrefix(text, "have "):
			fields := strings.Fields(text)
			if len(fields) >= 2 {
				haves = append(haves, fields[1])
			}
		case strings.HasPrefix(text, "shallow "):
			fields := strings.Fields(text)
			if len(fields) >= 2 {
				shallow = append(shallow, fields[1])
			}
		case strings.HasPrefix(text, "deepen "):
			if value, convErr := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(text, "deepen "))); convErr == nil {
				depth = value
			}
		}
	}
	return wants, haves, shallow, depth, nil
}

func (s *Store) gitReceivePack(w http.ResponseWriter, r *http.Request, repo *Repo) {
	if r.Method != http.MethodPost {
		s.fail(w, 404, "Not Found")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 128<<20))
	if err != nil {
		s.fail(w, 400, "Bad Request")
		return
	}
	report, err := unpackPush(repo, body)
	if err != nil {
		s.fail(w, 400, "Bad Request")
		return
	}
	if pushWantsSideband(body) {
		report = sidebandReport(report)
	}
	writeGitBody(w, "application/x-git-receive-pack-result", report)
}
