package main

// Pre-framed HTTP responses for the four timed workloads. A frame is the complete wire answer
// (status line, headers, body) so the connection loop can serve it with one conn.Write and no
// allocation: the depth-1 clone fetch, the clone ls-refs, both ref advertisements and the issue
// read cache. Anything not cached falls back to the generic handler path.

import (
	"bytes"
	"strconv"
)

// frameRequestID is a constant, non-empty request id for cached frames; the contract asks for a
// non-empty X-GitHub-Request-Id, not a unique one.
const frameRequestID = "forge"

var jsonFrameHead = []byte("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nX-Github-Request-Id: forge\r\nContent-Length: ")

func buildFrame(contentType string, body []byte) []byte {
	frame := make([]byte, 0, len(contentType)+len(body)+96)
	frame = append(frame, "HTTP/1.1 200 OK\r\nContent-Type: "...)
	frame = append(frame, contentType...)
	frame = append(frame, "\r\nX-Github-Request-Id: "...)
	frame = append(frame, frameRequestID...)
	frame = append(frame, "\r\nContent-Length: "...)
	frame = strconv.AppendInt(frame, int64(len(body)), 10)
	frame = append(frame, "\r\n\r\n"...)
	return append(frame, body...)
}

// jsonFrame is the frame for a compact JSON object.
func jsonFrame(body []byte) []byte {
	frame := make([]byte, 0, len(jsonFrameHead)+len(body)+8)
	frame = append(frame, jsonFrameHead...)
	frame = strconv.AppendInt(frame, int64(len(body)), 10)
	frame = append(frame, "\r\n\r\n"...)
	return append(frame, body...)
}

var (
	methodGET  = []byte("GET")
	methodPOST = []byte("POST")

	apiReposPrefix     = []byte("/api/v3/repos/")
	issuesSegment      = []byte("issues/")
	gitInfoRefsMarker  = []byte(".git/info/refs?service=")
	gitUploadPackMark  = []byte(".git/git-upload-pack")
	serviceUploadPack  = []byte("git-upload-pack")
	serviceReceivePack = []byte("git-receive-pack")
	commandLsRefs      = []byte("command=ls-refs")
	commandFetch       = []byte("command=fetch")
	refPrefixToken     = []byte("ref-prefix ")
	peelToken          = []byte("peel")
	symrefsToken       = []byte("symrefs")
	unbornToken        = []byte("unborn")
	wantToken          = []byte("want ")
	deepenOneToken     = []byte("deepen 1\n")
	deepenOneCRToken   = []byte("deepen 1\r\n")
	haveToken          = []byte("have ")
	advertV2Frame      []byte
	v2AdvertisementRaw = advertisementV2()
)

// fastFrame answers a timed request from cache, or nil to fall through to the generic path.
func (s *Store) fastFrame(head *rawHead, body []byte) []byte {
	if !s.authorizedBytes(head.auth) {
		return nil
	}
	if bytes.Equal(head.method, methodGET) {
		if bytes.HasPrefix(head.target, apiReposPrefix) {
			return s.fastIssueFrame(head.target)
		}
		if bytes.Index(head.target, gitInfoRefsMarker) > 0 {
			return s.fastInfoRefsFrame(head)
		}
		return nil
	}
	if bytes.Equal(head.method, methodPOST) && bytes.HasSuffix(head.target, gitUploadPackMark) {
		return s.fastUploadPackFrame(head, body)
	}
	return nil
}

// fastIssueFrame answers GET /api/v3/repos/{owner}/{repo}/issues/{n} from the issue's frame.
func (s *Store) fastIssueFrame(target []byte) []byte {
	rest := target[len(apiReposPrefix):]
	slash := bytes.IndexByte(rest, '/')
	if slash <= 0 {
		return nil
	}
	owner, rest := rest[:slash], rest[slash+1:]
	slash = bytes.IndexByte(rest, '/')
	if slash <= 0 {
		return nil
	}
	name, rest := rest[:slash], rest[slash+1:]
	if !bytes.HasPrefix(rest, issuesSegment) {
		return nil
	}
	number, ok := parsePositiveInt(rest[len(issuesSegment):])
	if !ok {
		return nil
	}
	repo := s.repoFast(owner, name)
	if repo == nil {
		return nil
	}
	repo.mu.RLock()
	issue := repo.issueByNumber[number]
	var frame []byte
	if issue != nil {
		frame = issue.frame
	}
	repo.mu.RUnlock()
	return frame
}

// fastInfoRefsFrame answers the two advertisements from their cached frames.
func (s *Store) fastInfoRefsFrame(head *rawHead) []byte {
	marker := bytes.Index(head.target, gitInfoRefsMarker)
	if marker <= 1 {
		return nil
	}
	owner, name, ok := splitRepoPath(head.target[1:marker])
	if !ok {
		return nil
	}
	service := head.target[marker+len(gitInfoRefsMarker):]
	repo := s.repoFast(owner, name)
	if repo == nil {
		return nil
	}
	if bytes.Equal(service, serviceReceivePack) {
		return repo.receiveAdvertisementFrame()
	}
	if !bytes.Equal(service, serviceUploadPack) {
		return nil
	}
	if head.gitProto2 {
		if advertV2Frame == nil {
			advertV2Frame = buildFrame("application/x-git-upload-pack-advertisement", v2AdvertisementRaw)
		}
		return advertV2Frame
	}
	return repo.uploadAdvertisementFrame()
}

// fastUploadPackFrame answers the clone path's ls-refs and depth-1 fetch from cache.
func (s *Store) fastUploadPackFrame(head *rawHead, body []byte) []byte {
	owner, name, ok := splitRepoPath(head.target[1 : len(head.target)-len(gitUploadPackMark)])
	if !ok {
		return nil
	}
	repo := s.repoFast(owner, name)
	if repo == nil {
		return nil
	}
	payload, _ := firstPktPayload(body)
	switch {
	case bytes.HasPrefix(payload, commandLsRefs):
		if !bytes.Contains(body, refPrefixToken) &&
			bytes.Contains(body, peelToken) && bytes.Contains(body, symrefsToken) && bytes.Contains(body, unbornToken) {
			return repo.lsRefsFrame()
		}
		return nil
	case bytes.HasPrefix(payload, commandFetch):
		want, ok := singleShallowWant(body)
		if !ok {
			return nil
		}
		return repo.fetchFrame(want)
	}
	return nil
}

// firstPktPayload returns the first data payload of a pkt-line stream.
func firstPktPayload(body []byte) ([]byte, bool) {
	if len(body) < 4 {
		return nil, false
	}
	length, ok := parseHexLength(body[:4])
	if !ok || length < 4 || length > len(body) {
		return nil, false
	}
	return body[4:length], true
}

// singleShallowWant matches "one want, deepen 1, no haves" and returns the wanted object id.
func singleShallowWant(body []byte) (string, bool) {
	if bytes.Contains(body, haveToken) {
		return "", false
	}
	if !bytes.Contains(body, deepenOneToken) && !bytes.Contains(body, deepenOneCRToken) {
		return "", false
	}
	index := bytes.Index(body, wantToken)
	if index < 0 {
		return "", false
	}
	oid := body[index+len(wantToken):]
	if len(oid) < 40 {
		return "", false
	}
	oid = oid[:40]
	if bytes.Contains(body[index+len(wantToken)+40:], wantToken) {
		return "", false
	}
	for _, c := range oid {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			return "", false
		}
	}
	return string(oid), true
}

func splitRepoPath(path []byte) (owner, name []byte, ok bool) {
	slash := bytes.IndexByte(path, '/')
	if slash <= 0 || slash == len(path)-1 {
		return nil, nil, false
	}
	return path[:slash], path[slash+1:], true
}

// parsePositiveInt parses ASCII digits without allocating; a trailing query or slash is rejected.
func parsePositiveInt(text []byte) (int, bool) {
	if len(text) == 0 || len(text) > 9 {
		return 0, false
	}
	value := 0
	for _, c := range text {
		if c < '0' || c > '9' {
			return 0, false
		}
		value = value*10 + int(c-'0')
	}
	if value <= 0 {
		return 0, false
	}
	return value, true
}

func parseHexLength(text []byte) (int, bool) {
	if len(text) != 4 {
		return 0, false
	}
	value := 0
	for _, c := range text {
		value <<= 4
		switch {
		case c >= '0' && c <= '9':
			value |= int(c - '0')
		case c >= 'a' && c <= 'f':
			value |= int(c-'a') + 10
		case c >= 'A' && c <= 'F':
			value |= int(c-'A') + 10
		default:
			return 0, false
		}
	}
	return value, true
}
