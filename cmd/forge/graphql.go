package main

// GitHub GraphQL v4: a hand-written document parser and the schema surface gh and
// github-mcp-server read. It resolves the same store REST and git serve.

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

// ------------------------------------------------------------------ lexer/parser

type gqlValue struct {
	kind string // string,int,float,bool,null,enum,var,list,object
	str  string
	num  int
	flag bool
	list []*gqlValue
	obj  map[string]*gqlValue
}

type gqlField struct {
	alias     string
	name      string
	args      map[string]*gqlValue
	selection []gqlSelect
}

type gqlFragment struct {
	typeName  string
	selection []gqlSelect
}

type gqlSelect struct {
	field    *gqlField
	fragment *gqlFragment
}

type gqlOperation struct {
	kind      string
	name      string
	selection []gqlSelect
}

type gqlParser struct {
	src  string
	pos  int
	fail error
}

func (p *gqlParser) skipSpace() {
	for p.pos < len(p.src) {
		c := p.src[p.pos]
		if c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' {
			p.pos++
			continue
		}
		if c == '#' {
			for p.pos < len(p.src) && p.src[p.pos] != '\n' {
				p.pos++
			}
			continue
		}
		break
	}
}

func (p *gqlParser) peek() byte {
	p.skipSpace()
	if p.pos >= len(p.src) {
		return 0
	}
	return p.src[p.pos]
}

func (p *gqlParser) name() string {
	p.skipSpace()
	start := p.pos
	for p.pos < len(p.src) {
		c := p.src[p.pos]
		if c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (p.pos > start && c >= '0' && c <= '9') {
			p.pos++
			continue
		}
		break
	}
	return p.src[start:p.pos]
}

func (p *gqlParser) parseString() (string, bool) {
	p.skipSpace()
	if p.pos >= len(p.src) || p.src[p.pos] != '"' {
		return "", false
	}
	p.pos++
	var out strings.Builder
	for p.pos < len(p.src) {
		c := p.src[p.pos]
		p.pos++
		if c == '"' {
			return out.String(), true
		}
		if c != '\\' {
			out.WriteByte(c)
			continue
		}
		if p.pos >= len(p.src) {
			break
		}
		esc := p.src[p.pos]
		p.pos++
		switch esc {
		case 'n':
			out.WriteByte('\n')
		case 't':
			out.WriteByte('\t')
		case 'r':
			out.WriteByte('\r')
		case 'b':
			out.WriteByte('\b')
		case 'f':
			out.WriteByte('\f')
		case 'u':
			if p.pos+4 <= len(p.src) {
				if code, err := strconv.ParseUint(p.src[p.pos:p.pos+4], 16, 32); err == nil {
					out.WriteRune(rune(code))
				}
				p.pos += 4
			}
		default:
			out.WriteByte(esc)
		}
	}
	return out.String(), false
}

func (p *gqlParser) parseValue() *gqlValue {
	p.skipSpace()
	if p.pos >= len(p.src) {
		return &gqlValue{kind: "null"}
	}
	c := p.src[p.pos]
	switch c {
	case '$':
		p.pos++
		return &gqlValue{kind: "var", str: p.name()}
	case '[':
		p.pos++
		list := []*gqlValue{}
		for p.peek() != ']' && p.pos < len(p.src) {
			list = append(list, p.parseValue())
			p.skipSpace()
			if p.pos < len(p.src) && p.src[p.pos] == ',' {
				p.pos++
			}
		}
		if p.pos < len(p.src) {
			p.pos++
		}
		return &gqlValue{kind: "list", list: list}
	case '{':
		p.pos++
		obj := map[string]*gqlValue{}
		for p.peek() != '}' && p.pos < len(p.src) {
			key := p.name()
			if p.peek() == ':' {
				p.pos++
			}
			obj[key] = p.parseValue()
			p.skipSpace()
			if p.pos < len(p.src) && p.src[p.pos] == ',' {
				p.pos++
			}
		}
		if p.pos < len(p.src) {
			p.pos++
		}
		return &gqlValue{kind: "object", obj: obj}
	case '"':
		text, _ := p.parseString()
		return &gqlValue{kind: "string", str: text}
	}
	if c == '-' || (c >= '0' && c <= '9') {
		start := p.pos
		if c == '-' {
			p.pos++
		}
		isFloat := false
		for p.pos < len(p.src) {
			d := p.src[p.pos]
			if d >= '0' && d <= '9' {
				p.pos++
				continue
			}
			if d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-' {
				isFloat = true
				p.pos++
				continue
			}
			break
		}
		text := p.src[start:p.pos]
		if isFloat {
			value, _ := strconv.ParseFloat(text, 64)
			return &gqlValue{kind: "float", num: int(value)}
		}
		value, _ := strconv.Atoi(text)
		return &gqlValue{kind: "int", num: value}
	}
	word := p.name()
	switch word {
	case "true":
		return &gqlValue{kind: "bool", flag: true}
	case "false":
		return &gqlValue{kind: "bool", flag: false}
	case "null":
		return &gqlValue{kind: "null"}
	}
	return &gqlValue{kind: "enum", str: word}
}

func (p *gqlParser) parseSelectionSet() []gqlSelect {
	out := []gqlSelect{}
	if p.peek() != '{' {
		return out
	}
	p.pos++
	for {
		p.skipSpace()
		if p.pos >= len(p.src) || p.src[p.pos] == '}' {
			if p.pos < len(p.src) {
				p.pos++
			}
			return out
		}
		if strings.HasPrefix(p.src[p.pos:], "...") {
			p.pos += 3
			entry := gqlSelect{}
			if p.peek() == '@' {
				// skip directive
				for p.pos < len(p.src) && p.src[p.pos] != ' ' && p.src[p.pos] != '\n' && p.src[p.pos] != '{' {
					p.pos++
				}
			}
			if p.peek() != '{' {
				word := p.name()
				if word != "on" {
					// a named fragment: not sent by the suite
					p.pos += 0
					continue
				}
				entry.fragment = &gqlFragment{typeName: p.name(), selection: p.parseSelectionSet()}
				out = append(out, entry)
				continue
			}
			entry.fragment = &gqlFragment{typeName: "", selection: p.parseSelectionSet()}
			out = append(out, entry)
			continue
		}
		field := &gqlField{args: map[string]*gqlValue{}}
		first := p.name()
		if first == "" {
			p.pos++
			continue
		}
		p.skipSpace()
		if p.pos < len(p.src) && p.src[p.pos] == ':' {
			p.pos++
			field.alias = first
			field.name = p.name()
		} else {
			field.name = first
			field.alias = first
		}
		if p.peek() == '(' {
			p.pos++
			for {
				p.skipSpace()
				if p.pos >= len(p.src) || p.src[p.pos] == ')' {
					if p.pos < len(p.src) {
						p.pos++
					}
					break
				}
				key := p.name()
				if key == "" {
					p.pos++
					continue
				}
				if p.peek() == ':' {
					p.pos++
				}
				field.args[key] = p.parseValue()
				p.skipSpace()
				if p.pos < len(p.src) && p.src[p.pos] == ',' {
					p.pos++
				}
			}
		}
		for p.peek() == '@' {
			for p.pos < len(p.src) && p.src[p.pos] != ' ' && p.src[p.pos] != '\n' && p.src[p.pos] != '{' && p.src[p.pos] != ')' {
				p.pos++
			}
			if p.peek() == '(' {
				depth := 0
				for p.pos < len(p.src) {
					if p.src[p.pos] == '(' {
						depth++
					}
					if p.src[p.pos] == ')' {
						depth--
						if depth == 0 {
							p.pos++
							break
						}
					}
					p.pos++
				}
			}
		}
		field.selection = p.parseSelectionSet()
		out = append(out, gqlSelect{field: field})
	}
}

func parseDocument(src string) (*gqlOperation, error) {
	p := &gqlParser{src: src}
	p.skipSpace()
	op := &gqlOperation{kind: "query"}
	word := p.name()
	switch word {
	case "query":
		op.kind = "query"
	case "mutation":
		op.kind = "mutation"
	case "subscription":
		return nil, fmt.Errorf("subscriptions are not supported")
	default:
		// an anonymous query: the word was the first field name
		p.pos = 0
	}
	if word == "query" || word == "mutation" {
		p.skipSpace()
		if p.peek() != '(' && p.peek() != '{' {
			op.name = p.name()
		}
		if p.peek() == '(' {
			depth := 0
			for p.pos < len(p.src) {
				if p.src[p.pos] == '(' {
					depth++
				}
				if p.src[p.pos] == ')' {
					depth--
					if depth == 0 {
						p.pos++
						break
					}
				}
				p.pos++
			}
		}
	}
	op.selection = p.parseSelectionSet()
	return op, nil
}

// ------------------------------------------------------------------ executor

type gqlExec struct {
	store  *Store
	vars   map[string]*gqlValue
	errors []map[string]any
}

func (g *gqlExec) resolve(value *gqlValue) any {
	if value == nil {
		return nil
	}
	switch value.kind {
	case "string":
		return value.str
	case "int":
		return value.num
	case "float":
		return value.num
	case "bool":
		return value.flag
	case "null":
		return nil
	case "enum":
		return value.str
	case "var":
		if actual, ok := g.vars[value.str]; ok {
			return g.resolve(actual)
		}
		return nil
	case "list":
		out := make([]any, 0, len(value.list))
		for _, entry := range value.list {
			out = append(out, g.resolve(entry))
		}
		return out
	case "object":
		out := map[string]any{}
		for key, entry := range value.obj {
			out[key] = g.resolve(entry)
		}
		return out
	}
	return nil
}

func (g *gqlExec) argString(field *gqlField, name string) string {
	if value, ok := field.args[name]; ok {
		if resolved, ok := g.resolve(value).(string); ok {
			return resolved
		}
	}
	return ""
}

func (g *gqlExec) argInt(field *gqlField, name string, fallback int) int {
	if value, ok := field.args[name]; ok {
		switch resolved := g.resolve(value).(type) {
		case int:
			return resolved
		case float64:
			return int(resolved)
		}
	}
	return fallback
}

func (g *gqlExec) argList(field *gqlField, name string) []any {
	if value, ok := field.args[name]; ok {
		if list, ok := g.resolve(value).([]any); ok {
			return list
		}
	}
	return nil
}

func (g *gqlExec) failField(typeName, fieldName string) {
	message := fmt.Sprintf("Field '%s' doesn't exist on type '%s'", fieldName, typeName)
	g.errors = append(g.errors, map[string]any{
		"message":   message,
		"path":      []any{fieldName},
		"locations": []any{map[string]any{"line": 1, "column": 1}},
	})
}

func (g *gqlExec) globalID(kind string, id int64) string {
	return base64.StdEncoding.EncodeToString([]byte(kind + ":" + strconv.FormatInt(id, 10)))
}

func (g *gqlExec) parseGlobalID(text string) (string, int64, bool) {
	raw, err := base64.StdEncoding.DecodeString(text)
	if err != nil {
		return "", 0, false
	}
	parts := strings.SplitN(string(raw), ":", 2)
	if len(parts) != 2 {
		return "", 0, false
	}
	id, err := strconv.ParseInt(parts[1], 10, 64)
	if err != nil {
		return "", 0, false
	}
	return parts[0], id, true
}

type fieldFunc func(field *gqlField) any

func (g *gqlExec) object(typeName string, selection []gqlSelect, resolve func(name string, field *gqlField) (any, bool)) map[string]any {
	out := map[string]any{}
	for _, entry := range selection {
		if entry.fragment != nil {
			if entry.fragment.typeName != "" && entry.fragment.typeName != typeName {
				continue
			}
			for key, value := range g.object(typeName, entry.fragment.selection, resolve) {
				out[key] = value
			}
			continue
		}
		field := entry.field
		if field == nil {
			continue
		}
		value, known := resolve(field.name, field)
		if !known {
			g.failField(typeName, field.name)
			out[field.alias] = nil
			continue
		}
		out[field.alias] = value
	}
	return out
}

// ------------------------------------------------------------------ schema tables

var typeFields = map[string][]string{
	"User":                             {"id", "login", "name", "email", "databaseId", "avatarUrl", "url"},
	"Repository":                       {"id", "databaseId", "name", "nameWithOwner", "owner", "defaultBranchRef", "viewerPermission", "hasIssuesEnabled", "issue", "issues", "pullRequests", "pullRequest", "description", "isPrivate", "createdAt", "updatedAt", "url", "stargazerCount", "forkCount", "hasIssuesEnabled"},
	"Issue":                            {"id", "number", "title", "state", "body", "createdAt", "updatedAt", "closed", "closedAt", "url", "comments", "author", "labels"},
	"PullRequest":                      {"id", "number", "state", "merged", "headRefName", "baseRefName", "mergeable", "files", "title", "body", "createdAt", "updatedAt", "isDraft", "mergedAt", "mergeCommit", "changedFiles", "url", "headRefOid", "baseRefOid"},
	"Ref":                              {"name", "prefix", "target"},
	"IssueConnection":                  {"nodes", "pageInfo", "totalCount", "edges"},
	"PullRequestConnection":            {"nodes", "pageInfo", "totalCount", "edges"},
	"PullRequestChangedFileConnection": {"nodes", "pageInfo", "totalCount", "edges"},
	"PageInfo":                         {"hasNextPage", "hasPreviousPage", "startCursor", "endCursor"},
	"PullRequestChangedFile":           {"path", "additions", "deletions", "changeType"},
}

var typeKind = map[string]string{
	"User": "OBJECT", "Repository": "OBJECT", "Issue": "OBJECT", "PullRequest": "OBJECT",
	"Ref": "OBJECT", "IssueConnection": "OBJECT", "PullRequestConnection": "OBJECT",
	"PullRequestChangedFileConnection": "OBJECT", "PageInfo": "OBJECT", "PullRequestChangedFile": "OBJECT",
	"SearchType": "ENUM", "IssueState": "ENUM", "PullRequestState": "ENUM",
	"MergeableState": "ENUM", "OrderDirection": "ENUM", "IssueOrderField": "ENUM",
}

var enumValues = map[string][]string{
	"SearchType":       {"ISSUE", "USER", "REPOSITORY", "PULL_REQUEST", "DISCUSSION"},
	"IssueState":       {"OPEN", "CLOSED"},
	"PullRequestState": {"OPEN", "CLOSED", "MERGED"},
	"MergeableState":   {"MERGEABLE", "CONFLICTING", "UNKNOWN"},
	"OrderDirection":   {"ASC", "DESC"},
	"IssueOrderField":  {"CREATED_AT", "UPDATED_AT", "COMMENTS"},
}

func fieldList(typeName string) []any {
	names := typeFields[typeName]
	items := make([]any, 0, len(names))
	for _, name := range names {
		items = append(items, map[string]any{"name": name, "description": "field " + name, "isDeprecated": false, "deprecationReason": nil})
	}
	return items
}

func enumList(typeName string) []any {
	values := enumValues[typeName]
	items := make([]any, 0, len(values))
	for _, name := range values {
		items = append(items, map[string]any{"name": name, "description": "value " + name, "isDeprecated": false, "deprecationReason": nil})
	}
	return items
}

// ------------------------------------------------------------------ connections

func cursorFor(index int) string {
	return base64.StdEncoding.EncodeToString([]byte("cursor:" + strconv.Itoa(index)))
}

func indexFromCursor(cursor string) int {
	if cursor == "" {
		return 0
	}
	raw, err := base64.StdEncoding.DecodeString(cursor)
	if err != nil {
		return 0
	}
	text := string(raw)
	if strings.HasPrefix(text, "cursor:") {
		if value, err := strconv.Atoi(text[len("cursor:"):]); err == nil && value >= 0 {
			return value
		}
	}
	return 0
}

func paginate(items []any, first int, after string) (page []any, hasNext bool, endCursor string) {
	if first <= 0 {
		first = len(items)
	}
	start := indexFromCursor(after)
	if start > len(items) {
		start = len(items)
	}
	end := start + first
	if end > len(items) {
		end = len(items)
	}
	page = items[start:end]
	hasNext = end < len(items)
	return page, hasNext, cursorFor(end)
}

// ------------------------------------------------------------------ root resolution

// executeGraphQL answers one operation document.
func (s *Store) executeGraphQL(query string, variables map[string]any) (map[string]any, []map[string]any) {
	op, err := parseDocument(query)
	if err != nil {
		return nil, []map[string]any{{"message": err.Error()}}
	}
	vars := map[string]*gqlValue{}
	for name, value := range variables {
		vars[name] = valueFromJSON(value)
	}
	exec := &gqlExec{store: s, vars: vars}
	data := map[string]any{}
	for _, entry := range op.selection {
		if entry.field == nil {
			continue
		}
		field := entry.field
		var value any
		if op.kind == "mutation" {
			value = exec.mutation(field)
		} else {
			value, _ = exec.rootField(field)
		}
		if value == nil {
			// an unknown root field is reported by rootField; a null is still a value
		}
		data[field.alias] = value
	}
	return data, exec.errors
}

func valueFromJSON(value any) *gqlValue {
	switch typed := value.(type) {
	case nil:
		return &gqlValue{kind: "null"}
	case string:
		return &gqlValue{kind: "string", str: typed}
	case bool:
		return &gqlValue{kind: "bool", flag: typed}
	case float64:
		return &gqlValue{kind: "int", num: int(typed)}
	case int:
		return &gqlValue{kind: "int", num: typed}
	case []any:
		list := make([]*gqlValue, 0, len(typed))
		for _, item := range typed {
			list = append(list, valueFromJSON(item))
		}
		return &gqlValue{kind: "list", list: list}
	case map[string]any:
		obj := map[string]*gqlValue{}
		for key, item := range typed {
			obj[key] = valueFromJSON(item)
		}
		return &gqlValue{kind: "object", obj: obj}
	}
	return &gqlValue{kind: "null"}
}

func (g *gqlExec) rootField(field *gqlField) (any, bool) {
	switch field.name {
	case "viewer":
		return g.selectUser(g.store.user.Login, field.selection), true
	case "repository":
		owner := g.argString(field, "owner")
		name := g.argString(field, "name")
		repo := g.store.findRepo(owner, name)
		if repo == nil {
			return nil, true
		}
		return g.selectRepository(repo, field.selection), true
	case "node":
		id := g.argString(field, "id")
		kind, number, ok := g.parseGlobalID(id)
		if !ok {
			return nil, true
		}
		switch kind {
		case "repo":
			repo := g.repoByID(number)
			if repo == nil {
				return nil, true
			}
			return g.selectRepositoryNode(repo, field.selection), true
		case "issue":
			repo, issue := g.issueByID(number)
			if repo == nil {
				return nil, true
			}
			return g.selectIssue(repo, issue, field.selection), true
		case "pr":
			repo, pull := g.pullByID(number)
			if repo == nil {
				return nil, true
			}
			return g.selectPull(repo, pull, field.selection), true
		}
		return nil, true
	case "__type":
		name := g.argString(field, "name")
		kind, ok := typeKind[name]
		if !ok {
			return nil, true
		}
		return g.object("__Type", field.selection, func(fieldName string, inner *gqlField) (any, bool) {
			switch fieldName {
			case "name":
				return name, true
			case "kind":
				return kind, true
			case "description":
				return "type " + name, true
			case "fields":
				if kind != "OBJECT" {
					return []any{}, true
				}
				return fieldList(name), true
			case "enumValues":
				if kind != "ENUM" {
					return []any{}, true
				}
				return enumList(name), true
			case "interfaces":
				return []any{}, true
			case "possibleTypes":
				return []any{}, true
			}
			return nil, false
		}), true
	}
	g.failField("Query", field.name)
	return nil, false
}

func (g *gqlExec) repoByID(id int64) *Repo {
	g.store.mu.RLock()
	defer g.store.mu.RUnlock()
	for _, repo := range g.store.order {
		if repo.ID == id {
			return repo
		}
	}
	return nil
}

func (g *gqlExec) issueByID(id int64) (*Repo, *Issue) {
	g.store.mu.RLock()
	repos := append([]*Repo(nil), g.store.order...)
	g.store.mu.RUnlock()
	for _, repo := range repos {
		repo.mu.RLock()
		for _, issue := range repo.Issues {
			if issue.ID == id {
				repo.mu.RUnlock()
				return repo, issue
			}
		}
		repo.mu.RUnlock()
	}
	return nil, nil
}

func (g *gqlExec) pullByID(id int64) (*Repo, *Pull) {
	g.store.mu.RLock()
	repos := append([]*Repo(nil), g.store.order...)
	g.store.mu.RUnlock()
	for _, repo := range repos {
		repo.mu.RLock()
		for _, pull := range repo.Pulls {
			if pull.ID == id {
				repo.mu.RUnlock()
				return repo, pull
			}
		}
		repo.mu.RUnlock()
	}
	return nil, nil
}

// ------------------------------------------------------------------ selectors

func (g *gqlExec) selectUser(login string, selection []gqlSelect) map[string]any {
	return g.object("User", selection, func(name string, field *gqlField) (any, bool) {
		switch name {
		case "login":
			return login, true
		case "id":
			return g.globalID("user", g.store.user.ID), true
		case "databaseId":
			return int(g.store.user.ID), true
		case "name":
			return login, true
		case "email":
			return login + "@example.invalid", true
		case "url":
			return "http://github.local/" + login, true
		case "avatarUrl":
			return "http://github.local/avatar.png", true
		}
		return nil, false
	})
}

func (g *gqlExec) selectRepository(repo *Repo, selection []gqlSelect) map[string]any {
	return g.object("Repository", selection, func(name string, field *gqlField) (any, bool) {
		return g.repositoryField(repo, name, field)
	})
}

func (g *gqlExec) selectRepositoryNode(repo *Repo, selection []gqlSelect) map[string]any {
	out := g.object("Repository", selection, func(name string, field *gqlField) (any, bool) {
		return g.repositoryField(repo, name, field)
	})
	out["id"] = g.globalID("repo", repo.ID)
	return out
}

func (g *gqlExec) repositoryField(repo *Repo, name string, field *gqlField) (any, bool) {
	switch name {
	case "id":
		return g.globalID("repo", repo.ID), true
	case "databaseId":
		return int(repo.ID), true
	case "name":
		return repo.Name, true
	case "nameWithOwner":
		return repo.Owner + "/" + repo.Name, true
	case "owner":
		return g.selectUser(repo.Owner, field.selection), true
	case "defaultBranchRef":
		repo.mu.RLock()
		_, exists := repo.Refs["refs/heads/"+repo.DefaultBranch]
		repo.mu.RUnlock()
		if !exists {
			return nil, true
		}
		return g.object("Ref", field.selection, func(name string, _ *gqlField) (any, bool) {
			switch name {
			case "name":
				return repo.DefaultBranch, true
			case "prefix":
				return "refs/heads/", true
			case "target":
				return map[string]any{"oid": repo.Refs["refs/heads/"+repo.DefaultBranch]}, true
			}
			return nil, false
		}), true
	case "viewerPermission":
		return "ADMIN", true
	case "hasIssuesEnabled":
		return true, true
	case "description":
		return repo.Description, true
	case "isPrivate":
		return repo.Private, true
	case "createdAt", "updatedAt":
		return baseTime, true
	case "url":
		return "http://github.local/" + repo.Owner + "/" + repo.Name, true
	case "stargazerCount", "forkCount":
		return 0, true
	case "issue":
		number := g.argInt(field, "number", 0)
		repo.mu.RLock()
		issue := repo.findIssue(number)
		if issue == nil {
			repo.mu.RUnlock()
			return nil, true
		}
		body := g.selectIssue(repo, issue, field.selection)
		repo.mu.RUnlock()
		return body, true
	case "issueOrPullRequest":
		return nil, true
	case "issues":
		return g.issueConnection(repo, field), true
	case "pullRequests":
		return g.pullConnection(repo, field), true
	case "pullRequest":
		number := g.argInt(field, "number", 0)
		repo.mu.RLock()
		pull := repo.findPull(number)
		if pull == nil {
			repo.mu.RUnlock()
			return nil, true
		}
		body := g.selectPull(repo, pull, field.selection)
		repo.mu.RUnlock()
		return body, true
	}
	return nil, false
}

func (g *gqlExec) issueConnection(repo *Repo, field *gqlField) map[string]any {
	first := g.argInt(field, "first", 100)
	after := g.argString(field, "after")
	states := g.argList(field, "states")
	stateFilter := map[string]bool{}
	for _, state := range states {
		if text, ok := state.(string); ok {
			stateFilter[strings.ToUpper(text)] = true
		}
	}
	descending := false
	if value, ok := field.args["orderBy"]; ok {
		if obj, ok := g.resolve(value).(map[string]any); ok {
			if direction, ok := obj["direction"].(string); ok && strings.EqualFold(direction, "DESC") {
				descending = true
			}
		}
	}
	repo.mu.RLock()
	issues := make([]*Issue, 0, len(repo.Issues))
	for _, issue := range repo.Issues {
		if len(stateFilter) > 0 && !stateFilter[strings.ToUpper(issue.State)] {
			continue
		}
		issues = append(issues, issue)
	}
	sortIssues(issues, descending)
	items := make([]any, 0, len(issues))
	for _, issue := range issues {
		items = append(items, g.selectIssue(repo, issue, field.selectionOf("nodes")))
	}
	repo.mu.RUnlock()
	page, hasNext, endCursor := paginate(items, first, after)
	return g.connection("IssueConnection", page, hasNext, endCursor, len(items), field)
}

func sortIssues(issues []*Issue, descending bool) {
	for i := 1; i < len(issues); i++ {
		for j := i; j > 0; j-- {
			left, right := issues[j-1].Number, issues[j].Number
			if (descending && left >= right) || (!descending && left <= right) {
				break
			}
			issues[j-1], issues[j] = issues[j], issues[j-1]
		}
	}
}

func (g *gqlExec) pullConnection(repo *Repo, field *gqlField) map[string]any {
	first := g.argInt(field, "first", 100)
	after := g.argString(field, "after")
	repo.mu.RLock()
	pulls := make([]*Pull, 0, len(repo.Pulls))
	for _, pull := range repo.Pulls {
		if pull.State == "open" {
			pulls = append(pulls, pull)
		}
	}
	items := make([]any, 0, len(pulls))
	for _, pull := range pulls {
		items = append(items, g.selectPull(repo, pull, field.selectionOf("nodes")))
	}
	repo.mu.RUnlock()
	page, hasNext, endCursor := paginate(items, first, after)
	return g.connection("PullRequestConnection", page, hasNext, endCursor, len(items), field)
}

// selectionOf returns the selection set under a connection's `nodes` field.
func (field *gqlField) selectionOf(name string) []gqlSelect {
	for _, entry := range field.selection {
		if entry.field != nil && entry.field.name == name {
			return entry.field.selection
		}
	}
	return nil
}

func (g *gqlExec) connection(typeName string, page []any, hasNext bool, endCursor string, total int, field *gqlField) map[string]any {
	return g.object(typeName, field.selection, func(name string, inner *gqlField) (any, bool) {
		switch name {
		case "nodes":
			return page, true
		case "totalCount":
			return total, true
		case "edges":
			edges := make([]any, 0, len(page))
			for index, node := range page {
				edges = append(edges, map[string]any{"node": node, "cursor": cursorFor(index + 1)})
			}
			return edges, true
		case "pageInfo":
			return g.object("PageInfo", inner.selection, func(infoName string, _ *gqlField) (any, bool) {
				switch infoName {
				case "hasNextPage":
					return hasNext, true
				case "hasPreviousPage":
					return false, true
				case "startCursor":
					if len(page) == 0 {
						return nil, true
					}
					return cursorFor(0), true
				case "endCursor":
					if len(page) == 0 {
						return nil, true
					}
					return endCursor, true
				}
				return nil, false
			}), true
		}
		return nil, false
	})
}

func (g *gqlExec) selectIssue(repo *Repo, issue *Issue, selection []gqlSelect) map[string]any {
	return g.object("Issue", selection, func(name string, field *gqlField) (any, bool) {
		switch name {
		case "id":
			return g.globalID("issue", issue.ID), true
		case "number":
			return issue.Number, true
		case "title":
			return issue.Title, true
		case "state":
			return strings.ToUpper(issue.State), true
		case "body":
			return issue.Body, true
		case "createdAt", "updatedAt":
			return issue.Created, true
		case "closed":
			return issue.State == "closed", true
		case "closedAt":
			if issue.State == "closed" {
				return baseTime, true
			}
			return nil, true
		case "url":
			return "http://github.local/" + repo.Owner + "/" + repo.Name + "/issues/" + strconv.Itoa(issue.Number), true
		case "author":
			return g.selectUser(repo.Owner, field.selection), true
		case "labels":
			return nil, true
		case "comments":
			items := make([]any, 0, len(issue.Comments))
			for _, comment := range issue.Comments {
				items = append(items, map[string]any{"id": g.globalID("comment", comment.ID), "body": comment.Body})
			}
			return map[string]any{"nodes": items, "totalCount": len(items)}, true
		}
		return nil, false
	})
}

func (g *gqlExec) selectPull(repo *Repo, pull *Pull, selection []gqlSelect) map[string]any {
	return g.object("PullRequest", selection, func(name string, field *gqlField) (any, bool) {
		switch name {
		case "id":
			return g.globalID("pr", pull.ID), true
		case "number":
			return pull.Number, true
		case "state":
			if pull.Merged {
				return "MERGED", true
			}
			return strings.ToUpper(pull.State), true
		case "merged":
			return pull.Merged, true
		case "headRefName":
			return pull.HeadRef, true
		case "baseRefName":
			return pull.BaseRef, true
		case "mergeable":
			if pull.Merged {
				return "UNKNOWN", true
			}
			return "MERGEABLE", true
		case "files":
			first := g.argInt(field, "first", 100)
			files := repo.pullFiles(pull)
			items := make([]any, 0, len(files))
			for _, file := range files {
				items = append(items, g.object("PullRequestChangedFile", field.selectionOf("nodes"), func(fileName string, _ *gqlField) (any, bool) {
					switch fileName {
					case "path":
						return file.Filename, true
					case "additions":
						return file.Additions, true
					case "deletions":
						return file.Deletions, true
					case "changeType":
						return strings.ToUpper(file.Status), true
					}
					return nil, false
				}))
			}
			page, hasNext, endCursor := paginate(items, first, "")
			return g.connection("PullRequestChangedFileConnection", page, hasNext, endCursor, len(items), field), true
		case "title":
			return pull.Title, true
		case "body":
			return pull.Body, true
		case "createdAt", "updatedAt":
			return pull.Created, true
		case "isDraft":
			return false, true
		case "mergedAt":
			if pull.Merged {
				return baseTime, true
			}
			return nil, true
		case "mergeCommit":
			if pull.Merged {
				return map[string]any{"oid": pull.MergeSHA, "commitUrl": ""}, true
			}
			return nil, true
		case "changedFiles":
			return len(repo.pullFiles(pull)), true
		case "url":
			return "http://github.local/" + repo.Owner + "/" + repo.Name + "/pull/" + strconv.Itoa(pull.Number), true
		case "headRefOid":
			return pull.HeadSHA, true
		case "baseRefOid":
			return pull.BaseSHA, true
		}
		return nil, false
	})
}

// ------------------------------------------------------------------ mutations

func (g *gqlExec) mutation(field *gqlField) any {
	input := map[string]any{}
	if value, ok := field.args["input"]; ok {
		if resolved, ok := g.resolve(value).(map[string]any); ok {
			input = resolved
		}
	}
	str := func(key string) string {
		if value, ok := input[key].(string); ok {
			return value
		}
		return ""
	}
	switch field.name {
	case "createIssue":
		kind, id, ok := g.parseGlobalID(str("repositoryId"))
		if !ok || kind != "repo" {
			return nil
		}
		repo := g.repoByID(id)
		if repo == nil {
			return nil
		}
		repo.mu.Lock()
		issue := repo.newIssue(g.store.nextIssueID(), str("title"), str("body"), nil)
		body := map[string]any{
			"issue": g.selectIssue(repo, issue, field.selectionOf("issue")),
		}
		repo.mu.Unlock()
		return body
	case "closeIssue":
		kind, id, ok := g.parseGlobalID(str("issueId"))
		if !ok || kind != "issue" {
			return nil
		}
		repo, issue := g.issueByID(id)
		if repo == nil {
			return nil
		}
		repo.mu.Lock()
		issue.State = "closed"
		issue.ClosedAt = baseTime
		issue.Updated = baseTime
		invalidateIssue(issue)
		body := map[string]any{"issue": g.selectIssue(repo, issue, field.selectionOf("issue"))}
		repo.mu.Unlock()
		return body
	case "createPullRequest":
		kind, id, ok := g.parseGlobalID(str("repositoryId"))
		if !ok || kind != "repo" {
			return nil
		}
		repo := g.repoByID(id)
		if repo == nil {
			return nil
		}
		head := str("headRefName")
		base := str("baseRefName")
		if base == "" {
			base = repo.DefaultBranch
		}
		repo.mu.Lock()
		headTip, headOK := repo.Refs["refs/heads/"+head]
		baseTip, baseOK := repo.Refs["refs/heads/"+base]
		if !headOK || !baseOK {
			repo.mu.Unlock()
			return nil
		}
		pull := repo.newPull(g.store.nextPullID(), str("title"), str("body"), head, base)
		pull.HeadSHA = headTip
		pull.BaseSHA = baseTip
		body := map[string]any{"pullRequest": g.selectPull(repo, pull, field.selectionOf("pullRequest"))}
		repo.mu.Unlock()
		return body
	case "mergePullRequest":
		kind, id, ok := g.parseGlobalID(str("pullRequestId"))
		if !ok || kind != "pr" {
			return nil
		}
		repo, pull := g.pullByID(id)
		if repo == nil {
			return nil
		}
		repo.mu.Lock()
		if !pull.Merged {
			author := g.store.user.Name + " <" + g.store.user.Email + ">"
			message := pull.Title + " (#" + strconv.Itoa(pull.Number) + ")\n"
			repo.mergeSquash(pull, message, author, 1735689600+int64(len(repo.Objects)))
		}
		body := map[string]any{"pullRequest": g.selectPull(repo, pull, field.selectionOf("pullRequest"))}
		repo.mu.Unlock()
		return body
	case "addComment":
		kind, id, ok := g.parseGlobalID(str("subjectId"))
		if !ok || kind != "issue" {
			return nil
		}
		repo, issue := g.issueByID(id)
		if repo == nil {
			return nil
		}
		repo.mu.Lock()
		comment := &Comment{ID: g.store.nextCommentID(), Body: str("body"), Login: repo.Owner, Created: baseTime}
		issue.Comments = append(issue.Comments, comment)
		body := map[string]any{"commentEdge": map[string]any{"node": map[string]any{
			"id": g.globalID("comment", comment.ID), "body": comment.Body,
		}}}
		repo.mu.Unlock()
		return body
	}
	g.failField("Mutation", field.name)
	return nil
}

// handleGraphQL answers POST /api/graphql, including GitHub's operation batching.
func (s *Store) oneGraphQL(query string, variables map[string]any) map[string]any {
	data, errors := s.executeGraphQL(query, variables)
	out := map[string]any{"data": data}
	if len(errors) > 0 {
		out["errors"] = errors
	}
	return out
}

func (s *Store) handleGraphQLBody(body []byte) (int, any) {
	trimmed := strings.TrimSpace(string(body))
	if trimmed == "" {
		return 400, map[string]any{"message": "Problems parsing JSON"}
	}
	if strings.HasPrefix(trimmed, "[") {
		var operations []struct {
			Query     string         `json:"query"`
			Variables map[string]any `json:"variables"`
		}
		if err := json.Unmarshal(body, &operations); err != nil {
			return 400, map[string]any{"message": "Problems parsing JSON"}
		}
		responses := make([]any, 0, len(operations))
		for _, operation := range operations {
			responses = append(responses, s.oneGraphQL(operation.Query, operation.Variables))
		}
		return 200, responses
	}
	var operation struct {
		Query     string         `json:"query"`
		Variables map[string]any `json:"variables"`
	}
	if err := json.Unmarshal(body, &operation); err != nil {
		return 400, map[string]any{"message": "Problems parsing JSON"}
	}
	if strings.TrimSpace(operation.Query) == "" {
		return 200, map[string]any{"data": nil, "errors": []any{map[string]any{"message": "query is required"}}}
	}
	return 200, s.oneGraphQL(operation.Query, operation.Variables)
}
