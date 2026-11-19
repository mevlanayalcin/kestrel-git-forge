package main

// Git smart HTTP: the v2 capability advertisement, ls-refs, fetch (v2 and a v0 fallback) and
// receive-pack v0 with per-ref fast-forward checks. Everything is served from the in-memory store.

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"sort"
	"strconv"
	"strings"
)

const zeroOID = "0000000000000000000000000000000000000000"

const agentName = "kestrel-forge/1.0"

var hexDigits = "0123456789abcdef"

func pktLine(payload []byte) []byte {
	length := len(payload) + 4
	if length > 65520 {
		length = 65520
		payload = payload[:length-4]
	}
	out := make([]byte, 4, length)
	out[0] = hexDigits[(length>>12)&0xf]
	out[1] = hexDigits[(length>>8)&0xf]
	out[2] = hexDigits[(length>>4)&0xf]
	out[3] = hexDigits[length&0xf]
	return append(out, payload...)
}

func pktStr(text string) []byte { return pktLine([]byte(text)) }

var pktFlush = []byte("0000")
var pktDelim = []byte("0001")

// splitPktLines decodes leading pkt-lines; rest is what follows the last control packet.
func splitPktLines(body []byte) (lines [][]byte, rest []byte, err error) {
	pos := 0
	for {
		if pos+4 <= len(body) && string(body[pos:pos+4]) == "PACK" {
			return lines, body[pos:], nil
		}
		if pos+4 > len(body) {
			if pos == len(body) {
				return lines, body[pos:], nil
			}
			return nil, nil, fmt.Errorf("truncated pkt-line header")
		}
		size, convErr := strconv.ParseUint(string(body[pos:pos+4]), 16, 32)
		if convErr != nil {
			return nil, nil, fmt.Errorf("bad pkt-line length")
		}
		switch size {
		case 0, 1, 2:
			pos += 4
			continue
		}
		if size < 4 || pos+int(size) > len(body) {
			return nil, nil, fmt.Errorf("bad pkt-line size %d", size)
		}
		lines = append(lines, body[pos+4:pos+int(size)])
		pos += int(size)
	}
}

func firstLine(lines [][]byte) string {
	for _, line := range lines {
		if len(line) > 0 {
			return strings.TrimRight(string(line), "\n")
		}
	}
	return ""
}

// advertisementV2 is the capability advertisement for Git-Protocol: version=2.
func advertisementV2() []byte {
	var buf bytes.Buffer
	buf.Write(pktStr("version 2\n"))
	buf.Write(pktStr("ls-refs=unborn\n"))
	buf.Write(pktStr("fetch=shallow\n"))
	buf.Write(pktStr("object-format=sha1\n"))
	buf.Write(pktFlush)
	return buf.Bytes()
}

// cachedAdvertisement answers the v0 ref advertisement from the per-generation cache.
func cachedAdvertisement(repo *Repo, service string) []byte {
	repo.mu.RLock()
	gen := repo.RefsGen
	cached := repo.advUploadCache
	cachedGen := repo.advUploadGen
	if service == "git-receive-pack" {
		cached = repo.advRecvCache
		cachedGen = repo.advRecvGen
	}
	repo.mu.RUnlock()
	if cached != nil && cachedGen == gen {
		return cached
	}
	body := advertisementV0(repo, service)
	repo.mu.Lock()
	if service == "git-receive-pack" {
		repo.advRecvCache = body
		repo.advRecvGen = gen
	} else {
		repo.advUploadCache = body
		repo.advUploadGen = gen
	}
	repo.mu.Unlock()
	return body
}

// isDefaultLsRefs reports the clone path's argument set: no ref-prefix, peel+symrefs+unborn.
func isDefaultLsRefs(args []string) bool {
	peel, symrefs, unborn := false, false, false
	for _, arg := range args {
		switch {
		case arg == "peel":
			peel = true
		case arg == "symrefs":
			symrefs = true
		case arg == "unborn":
			unborn = true
		case strings.HasPrefix(arg, "agent="), strings.HasPrefix(arg, "object-format="), arg == "":
			// protocol noise the cache key does not carry
		default:
			return false
		}
	}
	return peel && symrefs && unborn
}

// cachedLsRefs answers the clone path's ls-refs (no prefixes, peel+symrefs+unborn) from cache.
func cachedLsRefs(repo *Repo, args []string) []byte {
	repo.mu.RLock()
	gen := repo.RefsGen
	cached := repo.refsCache
	cachedGen := repo.refsCacheGen
	repo.mu.RUnlock()
	if cached != nil && cachedGen == gen {
		return cached
	}
	body := lsRefs(repo, args)
	repo.mu.Lock()
	repo.refsCache = body
	repo.refsCacheGen = gen
	repo.mu.Unlock()
	return body
}

// uploadAdvertisementFrame answers the v0 upload-pack advertisement as one wire frame.
func (r *Repo) uploadAdvertisementFrame() []byte {
	r.mu.RLock()
	gen, frame, frameGen := r.RefsGen, r.frameAdvUpload, r.frameAdvUploadGen
	r.mu.RUnlock()
	if frame != nil && frameGen == gen {
		return frame
	}
	body := cachedAdvertisement(r, "git-upload-pack")
	frame = buildFrame("application/x-git-upload-pack-advertisement", body)
	r.mu.Lock()
	r.frameAdvUpload, r.frameAdvUploadGen = frame, gen
	r.mu.Unlock()
	return frame
}

// receiveAdvertisementFrame answers the v0 receive-pack advertisement as one wire frame.
func (r *Repo) receiveAdvertisementFrame() []byte {
	r.mu.RLock()
	gen, frame, frameGen := r.RefsGen, r.frameAdvRecv, r.frameAdvRecvGen
	r.mu.RUnlock()
	if frame != nil && frameGen == gen {
		return frame
	}
	body := cachedAdvertisement(r, "git-receive-pack")
	frame = buildFrame("application/x-git-receive-pack-advertisement", body)
	r.mu.Lock()
	r.frameAdvRecv, r.frameAdvRecvGen = frame, gen
	r.mu.Unlock()
	return frame
}

// lsRefsFrame answers the clone path's ls-refs as one wire frame.
func (r *Repo) lsRefsFrame() []byte {
	r.mu.RLock()
	gen, frame, frameGen := r.RefsGen, r.frameLsRefs, r.frameLsRefsGen
	r.mu.RUnlock()
	if frame != nil && frameGen == gen {
		return frame
	}
	body := cachedLsRefs(r, []string{"peel", "symrefs", "unborn"})
	frame = buildFrame("application/x-git-upload-pack-result", body)
	r.mu.Lock()
	r.frameLsRefs, r.frameLsRefsGen = frame, gen
	r.mu.Unlock()
	return frame
}

// fetchFrame answers a single-want depth-1 fetch as one wire frame.
func (r *Repo) fetchFrame(want string) []byte {
	key := "d1:" + want
	r.mu.RLock()
	frame := r.frameFetch[key]
	r.mu.RUnlock()
	if frame != nil {
		return frame
	}
	body := r.cachedFetchV2(want, 1)
	frame = buildFrame("application/x-git-upload-pack-result", body)
	r.mu.Lock()
	if r.frameFetch == nil {
		r.frameFetch = make(map[string][]byte)
	}
	r.frameFetch[key] = frame
	r.mu.Unlock()
	return frame
}

// advertisementV0 is the ref advertisement for upload-pack or receive-pack.
func advertisementV0(repo *Repo, service string) []byte {
	caps := "agent=" + agentName
	if service == "git-upload-pack" {
		caps += " multi_ack_detailed side-band-64k thin-pack ofs-delta shallow no-progress include-tag object-format=sha1"
	} else {
		caps += " report-status delete-refs side-band-64k quiet ofs-delta"
	}
	var buf bytes.Buffer
	buf.Write(pktStr("# service=" + service + "\n"))
	buf.Write(pktFlush)

	repo.mu.RLock()
	refs := make([]string, 0, len(repo.Refs))
	for name := range repo.Refs {
		refs = append(refs, name)
	}
	sort.Strings(refs)
	head := ""
	if tip, ok := repo.Refs["refs/heads/"+repo.DefaultBranch]; ok {
		head = tip
	}
	type line struct{ oid, name string }
	out := make([]line, 0, len(refs)+1)
	if head != "" {
		out = append(out, line{head, "HEAD"})
		caps += " symref=HEAD:refs/heads/" + repo.DefaultBranch
	}
	for _, name := range refs {
		out = append(out, line{repo.Refs[name], name})
	}
	peels := make(map[string]string, 2)
	for _, name := range refs {
		if object := repo.Objects[repo.Refs[name]]; object != nil && object.Type == ObjTag {
			peels[name] = parseTag(object.Data).Object
		}
	}
	repo.mu.RUnlock()

	if len(out) == 0 {
		buf.Write(pktStr(zeroOID + " capabilities^{}\x00" + caps + "\n"))
		buf.Write(pktFlush)
		return buf.Bytes()
	}
	for index, entry := range out {
		suffix := ""
		if index == 0 {
			suffix = "\x00" + caps
		}
		buf.Write(pktStr(entry.oid + " " + entry.name + suffix + "\n"))
		if peeled, ok := peels[entry.name]; ok {
			buf.Write(pktStr(peeled + " " + entry.name + "^{}\n"))
		}
	}
	buf.Write(pktFlush)
	return buf.Bytes()
}

// lsRefs answers a v2 ls-refs command.
func lsRefs(repo *Repo, args []string) []byte {
	peel, symrefs, unborn := false, false, false
	prefixes := make([]string, 0, 2)
	for _, arg := range args {
		switch {
		case arg == "peel":
			peel = true
		case arg == "symrefs":
			symrefs = true
		case arg == "unborn":
			unborn = true
		case strings.HasPrefix(arg, "ref-prefix "):
			prefixes = append(prefixes, strings.TrimPrefix(arg, "ref-prefix "))
		}
	}
	matches := func(name string) bool {
		if len(prefixes) == 0 {
			return true
		}
		for _, prefix := range prefixes {
			if strings.HasPrefix(name, prefix) {
				return true
			}
		}
		return false
	}
	var buf bytes.Buffer
	repo.mu.RLock()
	names := make([]string, 0, len(repo.Refs))
	for name := range repo.Refs {
		if matches(name) {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	headTip, headOK := repo.Refs["refs/heads/"+repo.DefaultBranch]
	if len(prefixes) == 0 && symrefs {
		if headOK {
			line := headTip + " HEAD"
			line += " symref-target:refs/heads/" + repo.DefaultBranch
			buf.Write(pktStr(line + "\n"))
		} else if unborn {
			buf.Write(pktStr("unborn HEAD symref-target:refs/heads/" + repo.DefaultBranch + "\n"))
		}
	}
	for _, name := range names {
		oid := repo.Refs[name]
		line := oid + " " + name
		if peel {
			if object := repo.Objects[oid]; object != nil && object.Type == ObjTag {
				line += " peeled:" + parseTag(object.Data).Object
			}
		}
		buf.Write(pktStr(line + "\n"))
	}
	repo.mu.RUnlock()
	buf.Write(pktFlush)
	return buf.Bytes()
}

// fetchResult is a built v2 fetch answer.
type fetchResult struct {
	body      []byte
	shallow   []string
	objectIDs []string
}

// buildFetchV2 computes the objects a v2 fetch must send and frames the whole response body.
func buildFetchV2(repo *Repo, wants, haves []string, depth int) fetchResult {
	return buildFetchV2Shallow(repo, wants, haves, nil, depth)
}

func buildFetchV2Shallow(repo *Repo, wants, haves, clientShallow []string, depth int) fetchResult {
	repo.mu.RLock()
	set := make(map[string]bool, 64)
	boundaries := repo.depthClosure(wants, depth, set)
	if len(haves) > 0 {
		subtract := make(map[string]bool, 64)
		shallowSet := make(map[string]bool, len(clientShallow))
		for _, oid := range clientShallow {
			shallowSet[oid] = true
		}
		repo.closureShallow(haves, shallowSet, subtract)
		for oid := range set {
			if subtract[oid] {
				delete(set, oid)
			}
		}
	}
	oids := make([]string, 0, len(set))
	for oid := range set {
		oids = append(oids, oid)
	}
	sort.Strings(oids)
	objects := make([]*Object, 0, len(oids))
	for _, oid := range oids {
		if object := repo.Objects[oid]; object != nil {
			objects = append(objects, object)
		}
	}
	repo.mu.RUnlock()

	pack := writePack(objects)
	var buf bytes.Buffer
	if len(boundaries) > 0 {
		buf.Write(pktStr("shallow-info\n"))
		for _, oid := range boundaries {
			buf.Write(pktStr("shallow " + oid + "\n"))
		}
		buf.Write(pktFlush)
	}
	buf.Write(pktStr("packfile\n"))
	const maxBand = 65000
	for len(pack) > 0 {
		chunk := pack
		if len(chunk) > maxBand {
			chunk = chunk[:maxBand]
		}
		payload := make([]byte, 1+len(chunk))
		payload[0] = 1
		copy(payload[1:], chunk)
		buf.Write(pktLine(payload))
		pack = pack[len(chunk):]
	}
	buf.Write(pktFlush)
	return fetchResult{body: buf.Bytes(), shallow: boundaries, objectIDs: oids}
}

// buildFetchV0 frames a v0 upload-pack fetch answer: NAK, then the sideband pack.
func buildFetchV0(repo *Repo, wants, haves, clientShallow []string, depth int) []byte {
	result := buildFetchV2Shallow(repo, wants, haves, clientShallow, depth)
	var buf bytes.Buffer
	buf.Write(pktStr("NAK\n"))
	// Re-frame the packfile portion: buildFetchV2's body starts with optional shallow-info, then
	// the "packfile" header and band-1 packets; a v0 client wants the band packets directly.
	lines, _, err := splitPktLines(result.body)
	if err == nil {
		inPack := false
		for _, line := range lines {
			text := string(line)
			if text == "packfile\n" {
				inPack = true
				continue
			}
			if text == "shallow-info\n" {
				continue
			}
			if !inPack {
				if strings.HasPrefix(text, "shallow ") {
					buf.Write(pktStr(text))
				}
				continue
			}
			buf.Write(pktLine(line))
		}
	}
	buf.Write(pktFlush)
	return buf.Bytes()
}

// cachedFetchV2 answers a single-want depth-1 fetch from the per-commit cache. The first
// concurrent caller builds the pack while the others wait, then every clone is a memcpy.
func (r *Repo) cachedFetchV2(want string, depth int) []byte {
	if depth != 1 {
		return nil
	}
	key := "d1:" + want
	r.mu.RLock()
	cached := r.packCache[key]
	r.mu.RUnlock()
	if cached != nil {
		return cached
	}
	r.buildMu.Lock()
	defer r.buildMu.Unlock()
	r.mu.RLock()
	cached = r.packCache[key]
	r.mu.RUnlock()
	if cached != nil {
		return cached
	}
	result := buildFetchV2(r, []string{want}, nil, depth)
	r.mu.Lock()
	if r.packCache == nil {
		r.packCache = make(map[string][]byte)
	}
	r.packCache[key] = result.body
	r.mu.Unlock()
	return result.body
}

// ------------------------------------------------------------------ receive-pack

type pushCommand struct {
	oldOID string
	newOID string
	ref    string
}

// unpackPush applies a command list and an optional pack, returning the report-status lines.
func unpackPush(repo *Repo, body []byte) ([]byte, error) {
	lines, packData, err := splitPktLines(body)
	if err != nil {
		return nil, err
	}
	if len(lines) == 0 {
		return nil, fmt.Errorf("empty command list")
	}
	commands := make([]pushCommand, 0, len(lines))
	for index, line := range lines {
		text := string(line)
		text = strings.TrimRight(text, "\n")
		if index == 0 {
			if nul := strings.IndexByte(text, 0); nul >= 0 {
				text = text[:nul]
			}
		}
		fields := strings.SplitN(text, " ", 3)
		if len(fields) != 3 {
			return nil, fmt.Errorf("bad command line")
		}
		commands = append(commands, pushCommand{oldOID: fields[0], newOID: fields[1], ref: fields[2]})
	}

	repo.mu.Lock()
	defer repo.mu.Unlock()

	unpackLine := "unpack ok"
	if len(packData) > 0 {
		objects, packErr := readPack(packData, func(oid string) *Object { return repo.Objects[oid] })
		if packErr != nil {
			unpackLine = "unpack " + packErr.Error()
		} else {
			for oid, object := range objects {
				if _, exists := repo.Objects[oid]; !exists {
					repo.Objects[oid] = object
				}
			}
		}
	}

	var report bytes.Buffer
	report.Write(pktStr(unpackLine + "\n"))
	for _, command := range commands {
		if unpackLine != "unpack ok" {
			report.Write(pktStr("ng " + command.ref + " unpacker error\n"))
			continue
		}
		switch {
		case command.newOID == zeroOID:
			repo.deleteRef(command.ref)
			report.Write(pktStr("ok " + command.ref + "\n"))
		case !validOID(command.newOID):
			report.Write(pktStr("ng " + command.ref + " bad object\n"))
		case repo.Objects[command.newOID] == nil:
			report.Write(pktStr("ng " + command.ref + " missing object\n"))
		default:
			current, exists := repo.Refs[command.ref]
			if !exists {
				repo.setRef(command.ref, command.newOID)
				report.Write(pktStr("ok " + command.ref + "\n"))
				continue
			}
			if command.oldOID != current || !repo.ancestor(command.newOID, current) {
				report.Write(pktStr("ng " + command.ref + " non-fast-forward\n"))
				continue
			}
			if command.newOID != current {
				repo.setRef(command.ref, command.newOID)
			}
			report.Write(pktStr("ok " + command.ref + "\n"))
		}
	}
	report.Write(pktFlush)
	return report.Bytes(), nil
}

func validOID(oid string) bool {
	if len(oid) != 40 {
		return false
	}
	_, err := hex.DecodeString(oid)
	return err == nil
}

// sidebandReport wraps a report-status stream in side-band-64k band 1 packets.
func sidebandReport(inner []byte) []byte {
	var buf bytes.Buffer
	const maxBand = 65000
	for len(inner) > 0 {
		chunk := inner
		if len(chunk) > maxBand {
			chunk = chunk[:maxBand]
		}
		payload := make([]byte, 1+len(chunk))
		payload[0] = 1
		copy(payload[1:], chunk)
		buf.Write(pktLine(payload))
		inner = inner[len(chunk):]
	}
	buf.Write(pktFlush)
	return buf.Bytes()
}

// pushWantsSideband reports whether the client's first command asked for side-band-64k.
func pushWantsSideband(body []byte) bool {
	lines, _, err := splitPktLines(body)
	if err != nil || len(lines) == 0 {
		return false
	}
	first := string(lines[0])
	nul := strings.IndexByte(first, 0)
	if nul < 0 {
		return false
	}
	return strings.Contains(first[nul+1:], "side-band-64k")
}
