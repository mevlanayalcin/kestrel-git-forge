package main

// Git object model and packfile codec: SHA-1 object ids, the loose-object id recipe, a
// non-delta pack writer and a pack reader that also resolves OFS/REF deltas.

import (
	"bytes"
	"compress/zlib"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"sort"
	"sync"
)

type ObjectType int

const (
	ObjCommit ObjectType = 1
	ObjTree   ObjectType = 2
	ObjBlob   ObjectType = 3
	ObjTag    ObjectType = 4
)

func (t ObjectType) String() string {
	switch t {
	case ObjCommit:
		return "commit"
	case ObjTree:
		return "tree"
	case ObjBlob:
		return "blob"
	case ObjTag:
		return "tag"
	}
	return "unknown"
}

func typeFromString(s string) ObjectType {
	switch s {
	case "commit":
		return ObjCommit
	case "tree":
		return ObjTree
	case "blob":
		return ObjBlob
	case "tag":
		return ObjTag
	}
	return 0
}

// Object is one stored git object. Data is the payload without the loose-object header.
type Object struct {
	Type ObjectType
	Data []byte
}

func objectID(t ObjectType, data []byte) string {
	h := sha1.New()
	fmt.Fprintf(h, "%s %d\x00", t.String(), len(data))
	h.Write(data)
	return hex.EncodeToString(h.Sum(nil))
}

// TreeEntry is one entry of a parsed tree object.
type TreeEntry struct {
	Mode string
	Name string
	OID  string
}

func parseTree(data []byte) ([]TreeEntry, error) {
	entries := make([]TreeEntry, 0, 8)
	for len(data) > 0 {
		sp := bytes.IndexByte(data, ' ')
		if sp <= 0 {
			return nil, errors.New("tree entry has no mode")
		}
		mode := string(data[:sp])
		data = data[sp+1:]
		nul := bytes.IndexByte(data, 0)
		if nul < 0 {
			return nil, errors.New("tree entry has no name")
		}
		name := string(data[:nul])
		data = data[nul+1:]
		if len(data) < 20 {
			return nil, errors.New("tree entry has no object id")
		}
		oid := hex.EncodeToString(data[:20])
		data = data[20:]
		entries = append(entries, TreeEntry{Mode: mode, Name: name, OID: oid})
	}
	return entries, nil
}

// buildTree encodes entries in git's tree order. Callers usually pass them sorted; sorting again
// here keeps malformed input out of the store.
func buildTree(entries []TreeEntry) []byte {
	sorted := append([]TreeEntry(nil), entries...)
	sort.Slice(sorted, func(i, j int) bool { return treeNameLess(sorted[i], sorted[j]) })
	var buf bytes.Buffer
	for _, entry := range sorted {
		buf.WriteString(entry.Mode)
		buf.WriteByte(' ')
		buf.WriteString(entry.Name)
		buf.WriteByte(0)
		raw, err := hex.DecodeString(entry.OID)
		if err != nil || len(raw) != 20 {
			continue
		}
		buf.Write(raw)
	}
	return buf.Bytes()
}

func treeNameLess(a, b TreeEntry) bool {
	// git sorts trees as if directory names ended in '/'.
	an, bn := a.Name, b.Name
	if a.Mode == "40000" {
		an += "/"
	}
	if b.Mode == "40000" {
		bn += "/"
	}
	return an < bn
}

// CommitFields carries what the forge writes into a commit object.
type CommitFields struct {
	Tree      string
	Parents   []string
	Author    string
	Committer string
	Timestamp int64
	Message   string
	Encoding  string
}

func buildCommit(fields CommitFields) []byte {
	var buf bytes.Buffer
	buf.WriteString("tree " + fields.Tree + "\n")
	for _, parent := range fields.Parents {
		buf.WriteString("parent " + parent + "\n")
	}
	if fields.Encoding != "" {
		buf.WriteString("encoding " + fields.Encoding + "\n")
	}
	stamp := fmt.Sprintf("%d +0000", fields.Timestamp)
	buf.WriteString("author " + fields.Author + " " + stamp + "\n")
	buf.WriteString("committer " + fields.Committer + " " + stamp + "\n")
	buf.WriteString("\n")
	buf.WriteString(fields.Message)
	return buf.Bytes()
}

// CommitView is the parsed subset of a commit the forge reasons about.
type CommitView struct {
	Tree      string
	Parents   []string
	Author    string
	Committer string
	Message   string
}

func parseCommit(data []byte) CommitView {
	var view CommitView
	header := data
	if idx := bytes.Index(data, []byte("\n\n")); idx >= 0 {
		header = data[:idx]
		view.Message = string(data[idx+2:])
	}
	for _, line := range bytes.Split(header, []byte("\n")) {
		switch {
		case bytes.HasPrefix(line, []byte("tree ")):
			view.Tree = string(line[5:])
		case bytes.HasPrefix(line, []byte("parent ")):
			view.Parents = append(view.Parents, string(line[7:]))
		case bytes.HasPrefix(line, []byte("author ")):
			view.Author = string(line[7:])
		case bytes.HasPrefix(line, []byte("committer ")):
			view.Committer = string(line[10:])
		}
	}
	return view
}

// TagView is the parsed subset of an annotated tag.
type TagView struct {
	Object string
	Type   string
	Tag    string
}

func parseTag(data []byte) TagView {
	var view TagView
	header := data
	if idx := bytes.Index(data, []byte("\n\n")); idx >= 0 {
		header = data[:idx]
	}
	for _, line := range bytes.Split(header, []byte("\n")) {
		switch {
		case bytes.HasPrefix(line, []byte("object ")):
			view.Object = string(line[7:])
		case bytes.HasPrefix(line, []byte("type ")):
			view.Type = string(line[5:])
		case bytes.HasPrefix(line, []byte("tag ")):
			view.Tag = string(line[4:])
		}
	}
	return view
}

func packObjectHeader(t ObjectType, size int) []byte {
	first := byte(t)<<4 | byte(size&0x0f)
	size >>= 4
	out := []byte{first}
	for size > 0 {
		out[len(out)-1] |= 0x80
		out = append(out, byte(size&0x7f))
		size >>= 7
	}
	return out
}

// writePack encodes objects as a version 2 pack with no deltas and appends the trailing SHA-1.
func writePack(objects []*Object) []byte {
	var buf bytes.Buffer
	buf.WriteString("PACK")
	buf.Write([]byte{0, 0, 0, 2})
	writeUint32(&buf, uint32(len(objects)))
	zw, _ := zlib.NewWriterLevel(&buf, 1)
	for _, object := range objects {
		buf.Write(packObjectHeader(object.Type, len(object.Data)))
		zw.Reset(&buf)
		zw.Write(object.Data)
		zw.Close()
	}
	sum := sha1.Sum(buf.Bytes())
	buf.Write(sum[:])
	return buf.Bytes()
}

func writeUint32(buf *bytes.Buffer, v uint32) {
	buf.Write([]byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)})
}

// countReader counts the compressed bytes the zlib reader actually consumed. bytes.Reader
// implements io.ByteReader, so flate reads the stream byte by byte and stops at the adler32.
type countReader struct {
	r *bytes.Reader
	n int
}

func (c *countReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.n += n
	return n, err
}

func (c *countReader) ReadByte() (byte, error) {
	b, err := c.r.ReadByte()
	if err == nil {
		c.n++
	}
	return b, err
}

// zlib readers hold ~40 KiB of inflate tables; pushes inflate three objects each, so a pool
// keeps the timed push path allocation-free.
type zlibHolder struct {
	rc io.ReadCloser
}

var zlibPool = sync.Pool{New: func() any { return &zlibHolder{} }}

var inflateBufferPool = sync.Pool{New: func() any {
	buffer := make([]byte, 64<<10)
	return &buffer
}}

func inflateAt(data []byte) ([]byte, int, error) {
	return inflateAtSize(data, 0)
}

// inflateAtSize inflates one zlib stream, sizing the output from the pack header when known.
func inflateAtSize(data []byte, size int) ([]byte, int, error) {
	src := &countReader{r: bytes.NewReader(data)}
	holder := zlibPool.Get().(*zlibHolder)
	var zr io.ReadCloser
	if holder.rc == nil {
		created, err := zlib.NewReader(src)
		if err != nil {
			zlibPool.Put(holder)
			return nil, 0, err
		}
		holder.rc = created
		zr = created
	} else {
		if resetErr := holder.rc.(zlib.Resetter).Reset(src, nil); resetErr != nil {
			holder.rc.Close()
			holder.rc = nil
			zlibPool.Put(holder)
			return nil, 0, resetErr
		}
		zr = holder.rc
	}
	if size < 0 || size > 64<<20 {
		size = 0
	}
	out := make([]byte, 0, size)
	bufferPtr := inflateBufferPool.Get().(*[]byte)
	buffer := *bufferPtr
	for {
		n, readErr := zr.Read(buffer)
		if n > 0 {
			out = append(out, buffer[:n]...)
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			inflateBufferPool.Put(bufferPtr)
			zr.Close()
			zlibPool.Put(holder)
			return nil, src.n, readErr
		}
	}
	inflateBufferPool.Put(bufferPtr)
	if closeErr := zr.Close(); closeErr != nil {
		zlibPool.Put(holder)
		return nil, src.n, closeErr
	}
	zlibPool.Put(holder)
	return out, src.n, nil
}

func applyDelta(base, delta []byte) ([]byte, error) {
	pos := 0
	readVarint := func() (int, error) {
		value, shift := 0, 0
		for {
			if pos >= len(delta) {
				return 0, errors.New("delta ended inside a size")
			}
			b := delta[pos]
			pos++
			value |= int(b&0x7f) << shift
			if b&0x80 == 0 {
				return value, nil
			}
			shift += 7
			if shift > 56 {
				return 0, errors.New("delta size is too large")
			}
		}
	}
	baseSize, err := readVarint()
	if err != nil {
		return nil, err
	}
	if baseSize != len(base) {
		return nil, fmt.Errorf("delta base size %d does not match %d", baseSize, len(base))
	}
	resultSize, err := readVarint()
	if err != nil {
		return nil, err
	}
	out := make([]byte, 0, resultSize)
	for pos < len(delta) {
		op := delta[pos]
		pos++
		if op&0x80 != 0 {
			var offset, size int
			if op&0x01 != 0 {
				offset |= int(delta[pos])
				pos++
			}
			if op&0x02 != 0 {
				offset |= int(delta[pos]) << 8
				pos++
			}
			if op&0x04 != 0 {
				offset |= int(delta[pos]) << 16
				pos++
			}
			if op&0x08 != 0 {
				offset |= int(delta[pos]) << 24
				pos++
			}
			if op&0x10 != 0 {
				size |= int(delta[pos])
				pos++
			}
			if op&0x20 != 0 {
				size |= int(delta[pos]) << 8
				pos++
			}
			if op&0x40 != 0 {
				size |= int(delta[pos]) << 16
				pos++
			}
			if size == 0 {
				size = 0x10000
			}
			if offset < 0 || size < 0 || offset+size > len(base) {
				return nil, errors.New("delta copy runs past its base")
			}
			out = append(out, base[offset:offset+size]...)
			continue
		}
		if op == 0 {
			return nil, errors.New("delta opcode 0 is reserved")
		}
		if pos+int(op) > len(delta) {
			return nil, errors.New("delta insert runs past its data")
		}
		out = append(out, delta[pos:pos+int(op)]...)
		pos += int(op)
	}
	if len(out) != resultSize {
		return nil, fmt.Errorf("delta produced %d bytes, expected %d", len(out), resultSize)
	}
	return out, nil
}

// readPack parses a packfile (with or without deltas) and returns its objects keyed by id.
// resolve looks a base object up outside the pack; it may be nil.
func readPack(data []byte, resolve func(oid string) *Object) (map[string]*Object, error) {
	if len(data) < 32 || string(data[:4]) != "PACK" {
		return nil, errors.New("not a packfile")
	}
	version := int(data[4])<<24 | int(data[5])<<16 | int(data[6])<<8 | int(data[7])
	if version != 2 && version != 3 {
		return nil, fmt.Errorf("pack version %d", version)
	}
	count := int(data[8])<<24 | int(data[9])<<16 | int(data[10])<<8 | int(data[11])
	if count < 0 || count > 1_000_000 {
		return nil, errors.New("pack object count is implausible")
	}
	body := data[:len(data)-20]
	want := sha1.Sum(body)
	if !bytes.Equal(want[:], data[len(data)-20:]) {
		return nil, errors.New("pack checksum mismatch")
	}
	objects := make(map[string]*Object, count)
	byOffset := make(map[int]*Object, count)
	pos := 12
	for i := 0; i < count; i++ {
		start := pos
		if pos >= len(data)-20 {
			return nil, errors.New("pack ended early")
		}
		b := data[pos]
		pos++
		typ := (b >> 4) & 7
		size := int(b & 0x0f)
		shift := 4
		for b&0x80 != 0 {
			if pos >= len(data)-20 {
				return nil, errors.New("pack header ended early")
			}
			b = data[pos]
			pos++
			size |= int(b&0x7f) << shift
			shift += 7
			if shift > 35 {
				return nil, errors.New("pack object size is too large")
			}
		}
		var baseOID string
		baseOffset := -1
		switch ObjectType(typ) {
		case 6: // OFS_DELTA
			if pos >= len(data)-20 {
				return nil, errors.New("delta offset ended early")
			}
			c := data[pos]
			pos++
			distance := int(c & 0x7f)
			for c&0x80 != 0 {
				if pos >= len(data)-20 {
					return nil, errors.New("delta offset ended early")
				}
				c = data[pos]
				pos++
				distance = ((distance + 1) << 7) | int(c&0x7f)
			}
			baseOffset = start - distance
		case 7: // REF_DELTA
			if pos+20 > len(data)-20 {
				return nil, errors.New("delta base ended early")
			}
			baseOID = hex.EncodeToString(data[pos : pos+20])
			pos += 20
		}
		if pos >= len(data)-20 {
			return nil, errors.New("pack data ended early")
		}
		payload, used, err := inflateAtSize(data[pos:len(data)-20], size)
		if err != nil {
			return nil, err
		}
		pos += used
		object := &Object{Type: ObjectType(typ), Data: payload}
		switch ObjectType(typ) {
		case ObjCommit, ObjTree, ObjBlob, ObjTag:
			// complete object
		case 6: // OFS_DELTA
			base := byOffset[baseOffset]
			if base == nil {
				return nil, errors.New("delta base offset resolves to no object")
			}
			merged, err := applyDelta(base.Data, payload)
			if err != nil {
				return nil, err
			}
			object = &Object{Type: base.Type, Data: merged}
		case 7: // REF_DELTA
			var base *Object
			if fromPack := objects[baseOID]; fromPack != nil {
				base = fromPack
			} else if resolve != nil {
				base = resolve(baseOID)
			}
			if base == nil {
				return nil, errors.New("delta base id resolves to no object")
			}
			merged, err := applyDelta(base.Data, payload)
			if err != nil {
				return nil, err
			}
			object = &Object{Type: base.Type, Data: merged}
		default:
			return nil, fmt.Errorf("invalid pack object type %d", typ)
		}
		if object.Type == 0 || len(object.Data) != size && ObjectType(typ) != 6 && ObjectType(typ) != 7 {
			// The declared size is the inflated size for complete objects.
			return nil, errors.New("pack object size mismatch")
		}
		oid := objectID(object.Type, object.Data)
		objects[oid] = object
		byOffset[start] = object
	}
	return objects, nil
}
