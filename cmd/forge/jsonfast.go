package main

// Hand-rolled JSON for the two hottest replies: the issue body (2,000 timed reads and 200
// timed writes) and small string escaping. The shapes stay GitHub-like but carry only fields
// a client reads; encoding/json costs 13 allocations per answer where this costs one.

import (
	"encoding/json"
	"strconv"
)

// appendJSONString appends s as a JSON string. ASCII without control characters or quotes is
// copied verbatim; anything else goes through encoding/json's escaper.
func appendJSONString(dst []byte, s string) []byte {
	plain := true
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == '"' || c == '\\' || c < 0x20 || c >= 0x80 {
			plain = false
			break
		}
	}
	if plain {
		dst = append(dst, '"')
		dst = append(dst, s...)
		return append(dst, '"')
	}
	encoded, err := json.Marshal(s)
	if err != nil {
		return append(dst, '"', '"')
	}
	return append(dst, encoded...)
}

func appendJSONInt(dst []byte, value int64) []byte {
	return strconv.AppendInt(dst, value, 10)
}

// issueBody builds the compact issue object the REST surface answers for one issue.
func issueBody(repo *Repo, issue *Issue) []byte {
	buf := make([]byte, 0, 192)
	buf = append(buf, `{"id":`...)
	buf = appendJSONInt(buf, issue.ID)
	buf = append(buf, `,"number":`...)
	buf = appendJSONInt(buf, int64(issue.Number))
	buf = append(buf, `,"title":`...)
	buf = appendJSONString(buf, issue.Title)
	buf = append(buf, `,"body":`...)
	buf = appendJSONString(buf, issue.Body)
	buf = append(buf, `,"state":`...)
	buf = appendJSONString(buf, issue.State)
	buf = append(buf, `,"labels":[`...)
	for index, name := range issue.Labels {
		if index > 0 {
			buf = append(buf, ',')
		}
		label := repo.Labels[name]
		color := "ededed"
		id := int64(0)
		if label != nil {
			color = label.Color
			id = label.ID
		}
		buf = append(buf, `{"id":`...)
		buf = appendJSONInt(buf, id)
		buf = append(buf, `,"name":`...)
		buf = appendJSONString(buf, name)
		buf = append(buf, `,"color":`...)
		buf = appendJSONString(buf, color)
		buf = append(buf, `,"default":false}`...)
	}
	buf = append(buf, `],"user":{"login":`...)
	buf = appendJSONString(buf, repo.Owner)
	buf = append(buf, `,"id":1000,"type":"User"},"comments":`...)
	buf = appendJSONInt(buf, int64(len(issue.Comments)))
	buf = append(buf, '}')
	return buf
}
