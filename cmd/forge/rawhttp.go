package main

// A minimal HTTP/1.1 keep-alive server for the forge's wire: one buffered read per request and
// one write per response, no per-request deadlines, no Date header, no chunked encoding, no
// reflection. The timed endpoints (issue read, both advertisements, clone ls-refs and depth-1
// fetch) are answered from pre-framed byte slices with zero allocations; everything else takes
// the generic path through the same handlers net/http served.
//
// net/http costs ~7.4 syscalls per keep-alive request on this workload (deadline timers, epoll
// churn, separate buffered reads); this loop costs ~2.3, and the trial's sandbox charges for
// every syscall, so the timed workloads run here.

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
)

const (
	rawReadBuffer  = 8 << 10
	rawWriteBuffer = 16 << 10
	rawMaxHeader   = 64 << 10
)

var (
	rawReaderPool = sync.Pool{New: func() any { return bufio.NewReaderSize(strings.NewReader(""), rawReadBuffer) }}
	rawWriterPool = sync.Pool{New: func() any { return bufio.NewWriterSize(io.Discard, rawWriteBuffer) }}
	rawSinkPool   = sync.Pool{New: func() any {
		return &rawWriter{header: make(http.Header, 8), body: bytes.NewBuffer(make([]byte, 0, 512))}
	}}
	continueBytes = []byte("HTTP/1.1 100 Continue\r\n\r\n")
)

// serveRaw accepts connections until the listener closes.
func serveRaw(store *Store, listener net.Listener) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		if tcp, ok := conn.(*net.TCPConn); ok {
			_ = tcp.SetNoDelay(true)
		}
		go serveRawConn(store, conn)
	}
}

func serveRawConn(store *Store, conn net.Conn) {
	defer conn.Close()
	reader := rawReaderPool.Get().(*bufio.Reader)
	reader.Reset(conn)
	writer := rawWriterPool.Get().(*bufio.Writer)
	writer.Reset(conn)
	defer func() {
		reader.Reset(strings.NewReader(""))
		writer.Reset(io.Discard)
		rawReaderPool.Put(reader)
		rawWriterPool.Put(writer)
	}()

	headBuf := make([]byte, 0, 4096)
	bodyBuf := make([]byte, 0, 4096)
	for {
		head, err := readHead(reader, &headBuf)
		if err != nil {
			return
		}
		if head.expectContinue {
			if _, err := conn.Write(continueBytes); err != nil {
				return
			}
		}
		body, err := readBody(reader, head, &bodyBuf)
		if err != nil {
			return
		}
		if frame := store.fastFrame(head, body); frame != nil {
			if _, err := conn.Write(frame); err != nil {
				return
			}
			if head.closeAfter {
				return
			}
			continue
		}
		req := buildRequest(head, body)
		sink := rawSinkPool.Get().(*rawWriter)
		sink.resetForRequest()
		func() {
			defer func() {
				if recovered := recover(); recovered != nil {
					sink.resetError(500, `{"message":"Internal Server Error"}`)
				}
			}()
			store.ServeHTTP(sink, req)
		}()
		err = sink.finish(writer, head.closeAfter)
		rawSinkPool.Put(sink)
		if err != nil || head.closeAfter {
			return
		}
	}
}

// rawHead is one parsed request line and the headers the forge looks at. Its slices point into
// the connection's head buffer and stay valid until the next readHead.
type rawHead struct {
	method         []byte
	target         []byte
	proto          []byte
	auth           []byte
	contentLength  int
	chunked        bool
	closeAfter     bool
	expectContinue bool
	gitProto2      bool
	keepAliveAsked bool
	headerBlock    []byte
}

var errBadRequest = errors.New("bad request")

// readHead reads and parses one request head into buf (reused per connection).
func readHead(reader *bufio.Reader, buf *[]byte) (*rawHead, error) {
	*buf = (*buf)[:0]
	for {
		line, err := reader.ReadSlice('\n')
		if err != nil {
			return nil, err
		}
		*buf = append(*buf, line...)
		if len(*buf) > rawMaxHeader {
			return nil, errBadRequest
		}
		if len(line) <= 2 {
			break
		}
	}
	head := &rawHead{}
	lines := *buf
	lineEnd := bytes.IndexByte(lines, '\n')
	if lineEnd < 0 {
		return nil, errBadRequest
	}
	first := trimCR(lines[:lineEnd])
	firstSpace := bytes.IndexByte(first, ' ')
	lastSpace := bytes.LastIndexByte(first, ' ')
	if firstSpace <= 0 || lastSpace <= firstSpace {
		return nil, errBadRequest
	}
	head.method = first[:firstSpace]
	head.target = first[firstSpace+1 : lastSpace]
	head.proto = first[lastSpace+1:]
	for rest := lines[lineEnd+1:]; len(rest) > 0; {
		end := bytes.IndexByte(rest, '\n')
		if end < 0 {
			break
		}
		headerLine := trimCR(rest[:end])
		rest = rest[end+1:]
		if len(headerLine) == 0 {
			break
		}
		colon := bytes.IndexByte(headerLine, ':')
		if colon <= 0 {
			continue
		}
		name := headerLine[:colon]
		value := trimSpaces(headerLine[colon+1:])
		switch {
		case headerNameIs(name, "authorization"):
			head.auth = value
		case headerNameIs(name, "content-length"):
			length, ok := parsePositiveInt(value)
			if !ok && len(value) > 0 && value[0] == '0' {
				length = 0
			} else if !ok {
				return nil, errBadRequest
			}
			head.contentLength = length
		case headerNameIs(name, "transfer-encoding"):
			head.chunked = containsFoldASCII(value, "chunked")
		case headerNameIs(name, "connection"):
			head.closeAfter = containsFoldASCII(value, "close")
			head.keepAliveAsked = containsFoldASCII(value, "keep-alive")
		case headerNameIs(name, "expect"):
			head.expectContinue = containsFoldASCII(value, "100-continue")
		case headerNameIs(name, "git-protocol"):
			head.gitProto2 = containsFoldASCII(value, "version=2")
		}
	}
	if bytes.Equal(head.proto, []byte("HTTP/1.0")) && !head.keepAliveAsked {
		// HTTP/1.0 closes unless the client asked otherwise; the suite always speaks 1.1.
		head.closeAfter = true
	}
	head.headerBlock = lines[lineEnd+1:]
	return head, nil
}

func trimCR(line []byte) []byte {
	if len(line) > 0 && line[len(line)-1] == '\r' {
		return line[:len(line)-1]
	}
	return line
}

func trimSpaces(value []byte) []byte {
	for len(value) > 0 && (value[0] == ' ' || value[0] == '\t') {
		value = value[1:]
	}
	for len(value) > 0 && (value[len(value)-1] == ' ' || value[len(value)-1] == '\t') {
		value = value[:len(value)-1]
	}
	return value
}

func headerNameIs(name []byte, lower string) bool {
	if len(name) != len(lower) {
		return false
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		if c != lower[i] {
			return false
		}
	}
	return true
}

func containsFoldASCII(haystack []byte, lower string) bool {
	if len(lower) == 0 || len(haystack) < len(lower) {
		return false
	}
	for i := 0; i+len(lower) <= len(haystack); i++ {
		matched := true
		for j := 0; j < len(lower); j++ {
			c := haystack[i+j]
			if c >= 'A' && c <= 'Z' {
				c += 'a' - 'A'
			}
			if c != lower[j] {
				matched = false
				break
			}
		}
		if matched {
			return true
		}
	}
	return false
}

// readBody reads the request body into buf (reused per connection).
func readBody(reader *bufio.Reader, head *rawHead, buf *[]byte) ([]byte, error) {
	*buf = (*buf)[:0]
	if head.chunked {
		return readChunkedBody(reader, buf)
	}
	if head.contentLength <= 0 {
		return nil, nil
	}
	if cap(*buf) < head.contentLength {
		*buf = make([]byte, head.contentLength)
	} else {
		*buf = (*buf)[:head.contentLength]
	}
	if _, err := io.ReadFull(reader, *buf); err != nil {
		return nil, err
	}
	return *buf, nil
}

func readChunkedBody(reader *bufio.Reader, buf *[]byte) ([]byte, error) {
	body := (*buf)[:0]
	for {
		sizeLine, err := readLine(reader)
		if err != nil {
			return nil, err
		}
		if semicolon := strings.IndexByte(sizeLine, ';'); semicolon >= 0 {
			sizeLine = sizeLine[:semicolon]
		}
		size, convErr := strconv.ParseInt(strings.TrimSpace(sizeLine), 16, 64)
		if convErr != nil || size < 0 || size > 256<<20 {
			return nil, errBadRequest
		}
		if size == 0 {
			for {
				trailer, trailerErr := readLine(reader)
				if trailerErr != nil {
					return nil, trailerErr
				}
				if trailer == "" {
					*buf = body
					return body, nil
				}
			}
		}
		start := len(body)
		body = append(body, make([]byte, int(size))...)
		if _, err := io.ReadFull(reader, body[start:]); err != nil {
			return nil, err
		}
		if _, err := readLine(reader); err != nil {
			return nil, err
		}
	}
}

// readLine returns one CRLF-terminated line without its terminator.
func readLine(reader *bufio.Reader) (string, error) {
	slice, err := reader.ReadSlice('\n')
	if err != nil {
		return "", err
	}
	length := len(slice)
	if length >= 2 && slice[length-2] == '\r' {
		return string(slice[:length-2]), nil
	}
	return string(slice[:length-1]), nil
}

// buildRequest turns the parsed head into the *http.Request the handlers speak.
func buildRequest(head *rawHead, body []byte) *http.Request {
	header := make(http.Header, 8)
	// The head buffer's slices are still valid here; recover the header block by re-reading the
	// request line's remainder is not possible, so headers are re-parsed from the raw head bytes.
	// readHead keeps them in headBuf and the caller owns it, so store them on the head.
	for block := head.headerBlock; len(block) > 0; {
		end := bytes.IndexByte(block, '\n')
		if end < 0 {
			break
		}
		line := trimCR(block[:end])
		block = block[end+1:]
		if len(line) == 0 {
			break
		}
		colon := bytes.IndexByte(line, ':')
		if colon <= 0 {
			continue
		}
		name := http.CanonicalHeaderKey(string(trimSpaces(line[:colon])))
		header[name] = append(header[name], string(trimSpaces(line[colon+1:])))
	}
	target := string(head.target)
	parsed, err := url.ParseRequestURI(target)
	if err != nil {
		parsed = &url.URL{Path: target}
	}
	request := &http.Request{
		Method:        string(head.method),
		URL:           parsed,
		Proto:         string(head.proto),
		ProtoMajor:    1,
		ProtoMinor:    1,
		Header:        header,
		Host:          header.Get("Host"),
		RequestURI:    target,
		Body:          io.NopCloser(bytes.NewReader(body)),
		ContentLength: int64(len(body)),
	}
	if parsed.Host != "" {
		request.Host = parsed.Host
	}
	return request
}

// rawWriter is the http.ResponseWriter the handlers write into, pooled per connection burst.
type rawWriter struct {
	header      http.Header
	body        *bytes.Buffer
	status      int
	wroteHeader bool
}

func (w *rawWriter) Header() http.Header { return w.header }

func (w *rawWriter) resetForRequest() {
	clear(w.header)
	w.body.Reset()
	w.status = 0
	w.wroteHeader = false
}

func (w *rawWriter) resetError(status int, message string) {
	clear(w.header)
	w.body.Reset()
	w.status = status
	w.wroteHeader = true
	w.header.Set("Content-Type", "application/json; charset=utf-8")
	w.body.WriteString(message)
}

func (w *rawWriter) WriteHeader(status int) {
	if w.wroteHeader {
		return
	}
	w.status = status
	w.wroteHeader = true
}

func (w *rawWriter) Write(chunk []byte) (int, error) {
	if !w.wroteHeader {
		w.WriteHeader(http.StatusOK)
	}
	return w.body.Write(chunk)
}

var statusTexts = map[int]string{
	200: "OK", 201: "Created", 202: "Accepted", 204: "No Content", 301: "Moved Permanently",
	304: "Not Modified", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
	404: "Not Found", 405: "Method Not Allowed", 409: "Conflict", 413: "Payload Too Large",
	415: "Unsupported Media Type", 422: "Unprocessable Entity", 429: "Too Many Requests",
	500: "Internal Server Error", 501: "Not Implemented", 503: "Service Unavailable",
}

// finish writes the status line, a fixed header order and the body, then flushes once.
func (w *rawWriter) finish(writer *bufio.Writer, closeAfter bool) error {
	status := w.status
	if status == 0 {
		status = http.StatusOK
	}
	writer.WriteString("HTTP/1.1 ")
	writer.WriteString(strconv.Itoa(status))
	writer.WriteByte(' ')
	if text, ok := statusTexts[status]; ok {
		writer.WriteString(text)
	} else {
		writer.WriteString("Status")
	}
	writer.WriteString("\r\n")
	for _, name := range wireHeaderOrder {
		values := w.header[name]
		for _, value := range values {
			writer.WriteString(name)
			writer.WriteString(": ")
			writer.WriteString(value)
			writer.WriteString("\r\n")
		}
	}
	for name, values := range w.header {
		if isWireHeader(name) || name == "Content-Length" || name == "Date" || name == "Connection" {
			continue
		}
		for _, value := range values {
			writer.WriteString(name)
			writer.WriteString(": ")
			writer.WriteString(value)
			writer.WriteString("\r\n")
		}
	}
	bodyAllowed := status != http.StatusNoContent && status != http.StatusNotModified
	if bodyAllowed {
		writer.WriteString("Content-Length: ")
		writer.WriteString(strconv.Itoa(w.body.Len()))
		writer.WriteString("\r\n")
	}
	if closeAfter {
		writer.WriteString("Connection: close\r\n")
	}
	writer.WriteString("\r\n")
	if bodyAllowed && w.body.Len() > 0 {
		writer.Write(w.body.Bytes())
	}
	return writer.Flush()
}

func isWireHeader(name string) bool {
	for _, known := range wireHeaderOrder {
		if name == known {
			return true
		}
	}
	return false
}

// wireHeaderOrder is the fixed header order the forge emits (Content-Length is recomputed and
// Date is never sent).
var wireHeaderOrder = []string{
	"Content-Type",
	"Cache-Control",
	"X-Oauth-Scopes",
	"X-OAuth-Scopes",
	"Link",
	"Allow",
	"X-Github-Request-Id",
}
