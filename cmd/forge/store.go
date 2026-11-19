package main

// The forge's in-memory store: users, repositories with their git objects and refs, labels,
// issues, comments and pull requests. One RWMutex guards the maps; each repository carries its
// own lock so a hot read path does not serialise on the global one.

import (
	"encoding/base64"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
)

const (
	DefaultBranch = "main"
	// A fixed timestamp keeps every reply deterministic: no clock enters a stored object.
	baseTime = "2025-01-01T00:00:00Z"
)

type User struct {
	ID    int64
	Login string
	Name  string
	Email string
}

type Label struct {
	ID    int64
	Name  string
	Color string
}

type Comment struct {
	ID      int64
	Body    string
	Login   string
	Created string
}

type Issue struct {
	ID       int64
	Number   int
	Title    string
	Body     string
	State    string // "open" | "closed"
	Labels   []string
	Created  string
	Updated  string
	ClosedAt string
	Comments []*Comment
	Locked   bool
	cached   []byte
}

type PRFile struct {
	Filename  string
	Status    string
	Additions int
	Deletions int
	Changes   int
	SHA       string
}

type Pull struct {
	ID         int64
	Number     int
	Title      string
	Body       string
	State      string // "open" | "closed"
	HeadRef    string
	BaseRef    string
	HeadSHA    string
	BaseSHA    string
	Merged     bool
	MergedAt   string
	MergeSHA   string
	Created    string
	Updated    string
	Mergeable  string
	mergedOnce bool
}

type Repo struct {
	mu             sync.RWMutex
	buildMu        sync.Mutex
	ID             int64
	Owner          string
	Name           string
	Description    string
	Private        bool
	DefaultBranch  string
	Created        string
	Updated        string
	Pushed         string
	Objects        map[string]*Object
	Refs           map[string]string
	RefsGen        int64
	Labels         map[string]*Label
	Issues         []*Issue
	IssueSeq       int
	Pulls          []*Pull
	PullSeq        int
	CommentSeq     int64
	LabelSeq       int64
	jsonCache      []byte
	packCache      map[string][]byte
	advUploadCache []byte
	advUploadGen   int64
	advRecvCache   []byte
	advRecvGen     int64
	refsCache      []byte
	refsCacheGen   int64
}

type Store struct {
	mu         sync.RWMutex
	login      string
	token      string
	repoSeq    int64
	issueIDSeq int64
	prIDSeq    int64
	commentSeq int64
	labelSeq   int64
	repos      map[string]*Repo // "owner/name"
	order      []*Repo          // insertion order, for stable listings
	requestSeq atomic.Int64
	user       User
}

func newStore(login, token string) *Store {
	store := &Store{
		login:   login,
		token:   token,
		repoSeq: 1000,
		repos:   make(map[string]*Repo),
	}
	store.user = User{ID: 1000, Login: login, Name: login, Email: login + "@example.invalid"}
	return store
}

func repoKey(owner, name string) string {
	return strings.ToLower(owner) + "/" + strings.ToLower(name)
}

func (s *Store) nextIssueID() int64 {
	return atomic.AddInt64(&s.issueIDSeq, 1) + 10_000
}

func (s *Store) nextPullID() int64 {
	return atomic.AddInt64(&s.prIDSeq, 1) + 20_000
}

func (s *Store) nextCommentID() int64 {
	return atomic.AddInt64(&s.commentSeq, 1) + 30_000
}

func (s *Store) findRepo(owner, name string) *Repo {
	s.mu.RLock()
	repo := s.repos[repoKey(owner, name)]
	s.mu.RUnlock()
	return repo
}

func (s *Store) createRepo(owner, name, description string, private bool) (*Repo, bool) {
	key := repoKey(owner, name)
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.repos[key]; exists {
		return nil, false
	}
	s.repoSeq++
	repo := &Repo{
		ID:            s.repoSeq,
		Owner:         owner,
		Name:          name,
		Description:   description,
		Private:       private,
		DefaultBranch: DefaultBranch,
		Created:       baseTime,
		Updated:       baseTime,
		Pushed:        baseTime,
		Objects:       make(map[string]*Object),
		Refs:          make(map[string]string),
		Labels:        make(map[string]*Label),
		packCache:     make(map[string][]byte),
	}
	s.repos[key] = repo
	s.order = append(s.order, repo)
	return repo, true
}

func (s *Store) deleteRepo(owner, name string) bool {
	key := repoKey(owner, name)
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.repos[key]; !exists {
		return false
	}
	delete(s.repos, key)
	for i, repo := range s.order {
		if repoKey(repo.Owner, repo.Name) == key {
			s.order = append(s.order[:i], s.order[i+1:]...)
			break
		}
	}
	return true
}

// listRepos returns the viewer's repositories in creation order.
func (s *Store) listRepos(owner string) []*Repo {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*Repo, 0, len(s.order))
	for _, repo := range s.order {
		if strings.EqualFold(repo.Owner, owner) {
			out = append(out, repo)
		}
	}
	return out
}

// ------------------------------------------------------------------ git plumbing

func (r *Repo) putObject(t ObjectType, data []byte) string {
	oid := objectID(t, data)
	if _, exists := r.Objects[oid]; !exists {
		r.Objects[oid] = &Object{Type: t, Data: data}
	}
	return oid
}

func (r *Repo) object(oid string) *Object {
	return r.Objects[oid]
}

func (r *Repo) ref(name string) (string, bool) {
	oid, ok := r.Refs[name]
	return oid, ok
}

func (r *Repo) setRef(name, oid string) {
	r.Refs[name] = oid
	r.RefsGen++
	r.Pushed = baseTime
}

func (r *Repo) deleteRef(name string) {
	delete(r.Refs, name)
	r.RefsGen++
}

// ancestor reports whether `want` is `tip` or reachable from it through commit parents.
func (r *Repo) ancestor(tip, want string) bool {
	if tip == want {
		return true
	}
	seen := map[string]bool{tip: true}
	queue := []string{tip}
	for len(queue) > 0 {
		oid := queue[0]
		queue = queue[1:]
		object := r.Objects[oid]
		if object == nil || object.Type != ObjCommit {
			continue
		}
		for _, parent := range parseCommit(object.Data).Parents {
			if parent == want {
				return true
			}
			if !seen[parent] {
				seen[parent] = true
				queue = append(queue, parent)
			}
		}
	}
	return false
}

// closure adds every object reachable from the roots to set.
func (r *Repo) closure(roots []string, set map[string]bool) {
	r.closureShallow(roots, nil, set)
}

// closureShallow is closure with the client's shallow boundaries: a boundary commit keeps its
// tree but not its parents, exactly what a shallow client actually holds.
func (r *Repo) closureShallow(roots []string, shallow map[string]bool, set map[string]bool) {
	queue := make([]string, 0, len(roots)+16)
	for _, root := range roots {
		if root == "" || root == zeroOID {
			continue
		}
		if !set[root] {
			set[root] = true
			queue = append(queue, root)
		}
	}
	for len(queue) > 0 {
		oid := queue[0]
		queue = queue[1:]
		object := r.Objects[oid]
		if object == nil {
			continue
		}
		switch object.Type {
		case ObjCommit:
			view := parseCommit(object.Data)
			if view.Tree != "" && !set[view.Tree] {
				set[view.Tree] = true
				queue = append(queue, view.Tree)
			}
			if shallow[oid] {
				continue
			}
			for _, parent := range view.Parents {
				if !set[parent] {
					set[parent] = true
					queue = append(queue, parent)
				}
			}
		case ObjTree:
			entries, err := parseTree(object.Data)
			if err != nil {
				continue
			}
			for _, entry := range entries {
				if !set[entry.OID] {
					set[entry.OID] = true
					queue = append(queue, entry.OID)
				}
			}
		case ObjTag:
			view := parseTag(object.Data)
			if view.Object != "" && !set[view.Object] {
				set[view.Object] = true
				queue = append(queue, view.Object)
			}
		}
	}
}

// depthClosure walks commits from each want to at most `depth` generations (0 = unlimited),
// collecting each included commit's tree closure. It returns the object set and the boundary
// commits: those at the last included generation that still name a parent.
func (r *Repo) depthClosure(wants []string, depth int, set map[string]bool) []string {
	type node struct {
		oid   string
		level int
	}
	seen := make(map[string]int, len(wants))
	queue := make([]node, 0, len(wants))
	for _, want := range wants {
		if want == "" || want == zeroOID {
			continue
		}
		if _, ok := seen[want]; ok {
			continue
		}
		seen[want] = 1
		queue = append(queue, node{want, 1})
	}
	boundaries := make([]string, 0, 4)
	for len(queue) > 0 {
		current := queue[0]
		queue = queue[1:]
		set[current.oid] = true
		object := r.Objects[current.oid]
		if object == nil {
			continue
		}
		switch object.Type {
		case ObjCommit:
			view := parseCommit(object.Data)
			if view.Tree != "" {
				r.closure([]string{view.Tree}, set)
			}
			if depth > 0 && current.level >= depth {
				if len(view.Parents) > 0 {
					boundaries = append(boundaries, current.oid)
				}
				continue
			}
			for _, parent := range view.Parents {
				if _, ok := seen[parent]; ok {
					continue
				}
				seen[parent] = current.level + 1
				queue = append(queue, node{parent, current.level + 1})
			}
		case ObjTag:
			view := parseTag(object.Data)
			if view.Object != "" {
				if _, ok := seen[view.Object]; !ok {
					seen[view.Object] = current.level
					queue = append(queue, node{view.Object, current.level})
				}
			}
		}
	}
	return boundaries
}

// flattenTree collects every file path under treeOID into out.
func (r *Repo) flattenTree(treeOID, prefix string, out map[string]TreeEntry) {
	object := r.Objects[treeOID]
	if object == nil || object.Type != ObjTree {
		return
	}
	entries, err := parseTree(object.Data)
	if err != nil {
		return
	}
	for _, entry := range entries {
		path := entry.Name
		if prefix != "" {
			path = prefix + "/" + entry.Name
		}
		if entry.Mode == "40000" {
			r.flattenTree(entry.OID, path, out)
			continue
		}
		out[path] = TreeEntry{Mode: entry.Mode, Name: entry.Name, OID: entry.OID}
	}
}

// writeTreeFromFiles stores the nested trees for a path->entry map and returns the root tree id.
func (r *Repo) writeTreeFromFiles(files map[string]TreeEntry) string {
	type dir struct {
		files map[string]TreeEntry
		dirs  map[string]*dir
	}
	newDir := func() *dir { return &dir{files: map[string]TreeEntry{}, dirs: map[string]*dir{}} }
	root := newDir()
	for path, entry := range files {
		segments := strings.Split(path, "/")
		node := root
		for _, segment := range segments[:len(segments)-1] {
			child, ok := node.dirs[segment]
			if !ok {
				child = newDir()
				node.dirs[segment] = child
			}
			node = child
		}
		name := segments[len(segments)-1]
		entry.Name = name
		node.files[name] = entry
	}
	var build func(node *dir) string
	build = func(node *dir) string {
		entries := make([]TreeEntry, 0, len(node.files)+len(node.dirs))
		for _, entry := range node.files {
			entries = append(entries, entry)
		}
		for name, child := range node.dirs {
			entries = append(entries, TreeEntry{Mode: "40000", Name: name, OID: build(child)})
		}
		return r.putObject(ObjTree, buildTree(entries))
	}
	return build(root)
}

// commitFiles resolves a commit's tree into a path->entry map.
func (r *Repo) commitFiles(commitOID string) map[string]TreeEntry {
	files := make(map[string]TreeEntry, 16)
	object := r.Objects[commitOID]
	if object == nil || object.Type != ObjCommit {
		return files
	}
	r.flattenTree(parseCommit(object.Data).Tree, "", files)
	return files
}

// commitTree resolves the tip commit of a ref (or a commit id) to its file map.
func (r *Repo) treeFilesAt(refOrOID string) (map[string]TreeEntry, string, bool) {
	oid := refOrOID
	if strings.HasPrefix(oid, "refs/") {
		resolved, ok := r.Refs[oid]
		if !ok {
			return nil, "", false
		}
		oid = resolved
	} else if resolved, ok := r.resolveRef(oid); ok {
		oid = resolved
	}
	object := r.Objects[oid]
	if object == nil {
		return nil, "", false
	}
	if object.Type == ObjTag {
		oid = parseTag(object.Data).Object
		object = r.Objects[oid]
		if object == nil {
			return nil, "", false
		}
	}
	if object.Type != ObjCommit {
		return nil, "", false
	}
	return r.commitFiles(oid), oid, true
}

// resolveRef resolves a branch, tag or commit id to an object id at HEAD of that name.
func (r *Repo) resolveRef(name string) (string, bool) {
	if name == "" {
		name = r.DefaultBranch
	}
	if oid, ok := r.Refs["refs/heads/"+name]; ok {
		return oid, true
	}
	if oid, ok := r.Refs["refs/tags/"+name]; ok {
		return oid, true
	}
	if oid, ok := r.Refs[name]; ok {
		return oid, true
	}
	if object := r.Objects[name]; object != nil && object.Type == ObjCommit {
		return name, true
	}
	return "", false
}

// walkCommits returns the commits reachable from tip, newest first (breadth-first by parents).
func (r *Repo) walkCommits(tip string, limit int) []string {
	out := make([]string, 0, 32)
	if tip == "" {
		return out
	}
	seen := map[string]bool{}
	queue := []string{tip}
	for len(queue) > 0 && (limit <= 0 || len(out) < limit) {
		oid := queue[0]
		queue = queue[1:]
		if seen[oid] {
			continue
		}
		seen[oid] = true
		object := r.Objects[oid]
		if object == nil || object.Type != ObjCommit {
			continue
		}
		out = append(out, oid)
		view := parseCommit(object.Data)
		for _, parent := range view.Parents {
			if !seen[parent] {
				queue = append(queue, parent)
			}
		}
	}
	return out
}

// mergeBase returns one common ancestor of a and b, or "".
func (r *Repo) mergeBase(a, b string) string {
	ancestors := map[string]bool{}
	queue := []string{a}
	for len(queue) > 0 {
		oid := queue[0]
		queue = queue[1:]
		if ancestors[oid] {
			continue
		}
		ancestors[oid] = true
		if object := r.Objects[oid]; object != nil && object.Type == ObjCommit {
			queue = append(queue, parseCommit(object.Data).Parents...)
		}
	}
	queue = []string{b}
	seen := map[string]bool{}
	for len(queue) > 0 {
		oid := queue[0]
		queue = queue[1:]
		if seen[oid] {
			continue
		}
		seen[oid] = true
		if ancestors[oid] {
			return oid
		}
		if object := r.Objects[oid]; object != nil && object.Type == ObjCommit {
			queue = append(queue, parseCommit(object.Data).Parents...)
		}
	}
	return ""
}

// newIssue appends an issue, returning it.
func (r *Repo) newIssue(id int64, title, body string, labels []string) *Issue {
	r.IssueSeq++
	issue := &Issue{
		ID:      id,
		Number:  r.IssueSeq,
		Title:   title,
		Body:    body,
		State:   "open",
		Labels:  labels,
		Created: baseTime,
		Updated: baseTime,
	}
	r.Issues = append(r.Issues, issue)
	return issue
}

func (r *Repo) findIssue(number int) *Issue {
	for _, issue := range r.Issues {
		if issue.Number == number {
			return issue
		}
	}
	return nil
}

func (r *Repo) findPull(number int) *Pull {
	for _, pull := range r.Pulls {
		if pull.Number == number {
			return pull
		}
	}
	return nil
}

func (r *Repo) newPull(id int64, title, body, head, base string) *Pull {
	r.PullSeq++
	pull := &Pull{
		ID:        id,
		Number:    r.PullSeq,
		Title:     title,
		Body:      body,
		State:     "open",
		HeadRef:   head,
		BaseRef:   base,
		Created:   baseTime,
		Updated:   baseTime,
		Mergeable: "MERGEABLE",
	}
	r.Pulls = append(r.Pulls, pull)
	return pull
}

// pullFiles diffs the base and head trees of a pull request.
func (r *Repo) pullFiles(pull *Pull) []PRFile {
	baseFiles := map[string]TreeEntry{}
	headFiles := map[string]TreeEntry{}
	if baseTip, ok := r.Refs["refs/heads/"+pull.BaseRef]; ok {
		baseFiles = r.commitFiles(baseTip)
	}
	if object := r.Objects[pull.HeadSHA]; object != nil && object.Type == ObjCommit {
		headFiles = r.commitFiles(pull.HeadSHA)
	}
	paths := make([]string, 0, len(headFiles))
	for path := range headFiles {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	files := make([]PRFile, 0, len(paths))
	for _, path := range paths {
		head := headFiles[path]
		base, existed := baseFiles[path]
		if existed && base.OID == head.OID {
			continue
		}
		file := PRFile{Filename: path, Status: "modified", SHA: head.OID}
		if !existed {
			file.Status = "added"
		}
		added, deleted := 0, 0
		if object := r.Objects[head.OID]; object != nil {
			added = lineCount(string(object.Data))
		}
		if existed {
			if object := r.Objects[base.OID]; object != nil {
				deleted = lineCount(string(object.Data))
			}
		}
		if added == 0 {
			added = 1
		}
		file.Additions = added
		file.Deletions = deleted
		file.Changes = added + deleted
		files = append(files, file)
	}
	for path := range baseFiles {
		if _, ok := headFiles[path]; !ok {
			files = append(files, PRFile{Filename: path, Status: "removed", Changes: 0})
		}
	}
	return files
}

// mergeConflict reports whether merging head into base would overwrite a base change.
func (r *Repo) mergeConflict(baseTip, headTip string) bool {
	base := r.mergeBase(baseTip, headTip)
	if base == "" {
		return false
	}
	baseFiles := r.commitFiles(base)
	headFiles := r.commitFiles(headTip)
	tipFiles := r.commitFiles(baseTip)
	for path, headEntry := range headFiles {
		baseEntry, inBase := baseFiles[path]
		if inBase && baseEntry.OID == headEntry.OID {
			continue
		}
		tipEntry, inTip := tipFiles[path]
		if !inTip {
			if inBase {
				return true // head changed a file the merge removed
			}
			continue
		}
		tipBaseEntry, tipInBase := baseFiles[path]
		if tipInBase && tipBaseEntry.OID == tipEntry.OID {
			continue // base did not change this path
		}
		if tipEntry.OID != headEntry.OID {
			return true
		}
	}
	return false
}

// mergeSquash writes a squash commit on the base branch carrying the head's changed paths.
func (r *Repo) mergeSquash(pull *Pull, message, author string, timestamp int64) (string, bool) {
	baseTip, ok := r.Refs["refs/heads/"+pull.BaseRef]
	if !ok {
		return "", false
	}
	if r.mergeConflict(baseTip, pull.HeadSHA) {
		return "", false
	}
	baseFiles := r.commitFiles(baseTip)
	headFiles := r.commitFiles(pull.HeadSHA)
	baseOfMerge := r.mergeBase(baseTip, pull.HeadSHA)
	mergeBaseFiles := r.commitFiles(baseOfMerge)
	for path, headEntry := range headFiles {
		baseEntry, inBase := mergeBaseFiles[path]
		if inBase && baseEntry.OID == headEntry.OID {
			continue
		}
		baseFiles[path] = headEntry
	}
	for path := range mergeBaseFiles {
		if _, ok := headFiles[path]; !ok {
			delete(baseFiles, path)
		}
	}
	tree := r.writeTreeFromFiles(baseFiles)
	commitData := buildCommit(CommitFields{
		Tree:      tree,
		Parents:   []string{baseTip},
		Author:    author,
		Committer: author,
		Timestamp: timestamp,
		Message:   message,
	})
	commit := r.putObject(ObjCommit, commitData)
	r.setRef("refs/heads/"+pull.BaseRef, commit)
	pull.Merged = true
	pull.State = "closed"
	pull.MergedAt = baseTime
	pull.MergeSHA = commit
	pull.Updated = baseTime
	pull.Mergeable = "UNKNOWN"
	return commit, true
}

func lineCount(text string) int {
	if text == "" {
		return 0
	}
	count := strings.Count(text, "\n")
	if !strings.HasSuffix(text, "\n") {
		count++
	}
	return count
}

func b64(data []byte) string {
	return base64.StdEncoding.EncodeToString(data)
}
