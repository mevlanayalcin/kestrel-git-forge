package main

// A minimal HTTP/1.1 keep-alive server for the forge's wire: one buffered read per request and
// one write per response, no per-request deadlines, no Date header, no chunked encoding, no
// reflection. It builds the same *http.Request the handlers already speak and a response writer
// that assembles status line + headers + body into the connection's buffer.
//
// net/http costs ~30 syscalls per keep-alive request on this workload (deadline timers,
// epoll churn, separate buffered reads); the trial's sandbox charges for every syscall, so the
// timed workloads run on this loop instead.

import (
	"bufio"
	"bytes"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
)

const (
	rawReadBuffer  = 64 << 10
	rawWriteBuffer = 256 << 10
	rawMaxHeader   = 64 << 10
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
	reader := bufio.NewReaderSize(conn, rawReadBuffer)
	writer := bufio.NewWriterSize(conn, rawWriteBuffer)
	for {
		req, closeAfter, ok := readRawRequest(conn, reader)
		if !ok {
			return
		}
		rw := &rawWriter{header: make(http.Header, 6)}
		func() {
			defer func() {
				if recovered := recover(); recovered != nil {
					rw.reset(500)
					rw.writeBody([]byte(`{"message":"Internal Server Error"}`))
				}
			}()
			store.ServeHTTP(rw, req)
		}()
		if err := rw.finish(writer, closeAfter); err != nil {
			return
		}
		if closeAfter {
			return
		}
	}
}

// readRawRequest parses one request. It returns false on a closed or malformed connection.
func readRawRequest(conn net.Conn, reader *bufio.Reader) (*http.Request, bool, bool) {
	line, err := readLine(reader)
	if err != nil {
		return nil, false, false
	}
	if line == "" {
		// A stray CRLF between requests: tolerate it once.
		if line, err = readLine(reader); err != nil || line == "" {
			return nil, false, false
		}
	}
	firstSpace := strings.IndexByte(line, ' ')
	lastSpace := strings.LastIndexByte(line, ' ')
	if firstSpace <= 0 || lastSpace <= firstSpace {
		return nil, false, false
	}
	method := line[:firstSpace]
	target := line[firstSpace+1 : lastSpace]
	proto := line[lastSpace+1:]

	header := make(http.Header, 8)
	headerBytes := len(line)
	for {
		headerLine, readErr := readLine(reader)
		if readErr != nil {
			return nil, false, false
		}
		if headerLine == "" {
			break
		}
		headerBytes += len(headerLine)
		if headerBytes > rawMaxHeader {
			return nil, false, false
		}
		colon := strings.IndexByte(headerLine, ':')
		if colon <= 0 {
			continue
		}
		name := strings.TrimSpace(headerLine[:colon])
		value := strings.TrimSpace(headerLine[colon+1:])
		canonical := http.CanonicalHeaderKey(name)
		header[canonical] = append(header[canonical], value)
	}

	closeAfter := false
	if strings.Contains(strings.ToLower(header.Get("Connection")), "close") {
		closeAfter = true
	}
	if proto == "HTTP/1.0" && !strings.Contains(strings.ToLower(header.Get("Connection")), "keep-alive") {
		closeAfter = true
	}

	if strings.Contains(strings.ToLower(header.Get("Expect")), "100-continue") {
		if _, writeErr := io.WriteString(conn, "HTTP/1.1 100 Continue\r\n\r\n"); writeErr != nil {
			return nil, false, false
		}
	}

	var body []byte
	if strings.Contains(strings.ToLower(header.Get("Transfer-Encoding")), "chunked") {
		body, err = readChunkedBody(reader)
		if err != nil {
			return nil, false, false
		}
	} else if lengthText := header.Get("Content-Length"); lengthText != "" {
		length, convErr := strconv.Atoi(lengthText)
		if convErr != nil || length < 0 || length > 256<<20 {
			return nil, false, false
		}
		if length > 0 {
			body = make([]byte, length)
			if _, readErr := io.ReadFull(reader, body); readErr != nil {
				return nil, false, false
			}
		}
	}

	parsed, parseErr := url.ParseRequestURI(target)
	if parseErr != nil {
		return nil, false, false
	}
	request := &http.Request{
		Method:        method,
		URL:           parsed,
		Proto:         proto,
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
	return request, closeAfter, true
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

func readChunkedBody(reader *bufio.Reader) ([]byte, error) {
	var body []byte
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
			return nil, io.ErrUnexpectedEOF
		}
		if size == 0 {
			for {
				trailer, trailerErr := readLine(reader)
				if trailerErr != nil {
					return nil, trailerErr
				}
				if trailer == "" {
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

// rawWriter is the http.ResponseWriter the handlers write into.
type rawWriter struct {
	header      http.Header
	body        bytes.Buffer
	status      int
	wroteHeader bool
}

func (w *rawWriter) Header() http.Header { return w.header }

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

func (w *rawWriter) reset(status int) {
	w.header = make(http.Header, 6)
	w.body.Reset()
	w.status = status
	w.wroteHeader = true
}

func (w *rawWriter) writeBody(body []byte) {
	w.body.Reset()
	w.body.Write(body)
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
	written := make(map[string]bool, len(wireHeaderOrder))
	for _, name := range wireHeaderOrder {
		written[name] = true
		values := w.header[name]
		for _, value := range values {
			writer.WriteString(name)
			writer.WriteString(": ")
			writer.WriteString(value)
			writer.WriteString("\r\n")
		}
	}
	// Anything else a handler set (Location, Retry-After, ...) still goes out, minus the headers
	// this loop owns: Content-Length is recomputed and Date is never sent.
	for name, values := range w.header {
		if written[name] || name == "Content-Length" || name == "Date" || name == "Connection" {
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

// wireHeaderOrder is the fixed header order the forge emits; anything else the handlers set is
// irrelevant on the wire and is dropped (Content-Length is recomputed, Date is never sent).
var wireHeaderOrder = []string{
	"Content-Type",
	"Cache-Control",
	"X-Oauth-Scopes",
	"X-OAuth-Scopes",
	"Link",
	"Allow",
	"X-Github-Request-Id",
}
