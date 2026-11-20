// Git object codec: ids, trees, commits, tags and the packfile reader/writer.
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "forge.hpp"

namespace kestrel {

const char* kZeroOid = "0000000000000000000000000000000000000000";
const std::string kPktFlush = "0000";

std::string object_id(int type, const std::string& data) {
  Sha1 h;
  std::string header = std::string(obj_type_name(type)) + " " + itoa10((long long)data.size());
  header.push_back('\0');
  h.update(header.data(), header.size());
  h.update(data.data(), data.size());
  unsigned char out[20];
  h.finish(out);
  return hex_encode(out, 20);
}

bool tree_name_less(const TreeEntry& a, const TreeEntry& b) {
  std::string an = a.name, bn = b.name;
  if (a.mode == "40000") an.push_back('/');
  if (b.mode == "40000") bn.push_back('/');
  return an < bn;
}

bool parse_tree(const std::string& data, std::vector<TreeEntry>& out) {
  out.clear();
  size_t pos = 0;
  while (pos < data.size()) {
    size_t sp = data.find(' ', pos);
    if (sp == std::string::npos || sp == pos) return false;
    std::string mode = data.substr(pos, sp - pos);
    pos = sp + 1;
    size_t nul = data.find('\0', pos);
    if (nul == std::string::npos) return false;
    std::string name = data.substr(pos, nul - pos);
    pos = nul + 1;
    if (data.size() - pos < 20) return false;
    std::string oid = hex_encode((const unsigned char*)data.data() + pos, 20);
    pos += 20;
    out.push_back(TreeEntry{std::move(mode), std::move(name), std::move(oid)});
  }
  return true;
}

std::string build_tree(std::vector<TreeEntry> entries) {
  std::sort(entries.begin(), entries.end(), tree_name_less);
  std::string out;
  out.reserve(entries.size() * 40);
  for (const auto& e : entries) {
    out += e.mode;
    out.push_back(' ');
    out += e.name;
    out.push_back('\0');
    for (int i = 0; i < 20; i++) {
      int hi = i * 2 < (int)e.oid.size() ? hexval(e.oid[i * 2]) : -1;
      int lo = i * 2 + 1 < (int)e.oid.size() ? hexval(e.oid[i * 2 + 1]) : -1;
      if (hi < 0 || lo < 0) return out;
      out.push_back(char((hi << 4) | lo));
    }
  }
  return out;
}

CommitView parse_commit(const std::string& data) {
  CommitView view;
  size_t split = data.find("\n\n");
  std::string header = split == std::string::npos ? data : data.substr(0, split);
  if (split != std::string::npos) view.message = data.substr(split + 2);
  size_t pos = 0;
  while (pos <= header.size()) {
    size_t nl = header.find('\n', pos);
    if (nl == std::string::npos) nl = header.size();
    std::string line = header.substr(pos, nl - pos);
    if (starts_with(line, "tree "))
      view.tree = line.substr(5);
    else if (starts_with(line, "parent "))
      view.parents.push_back(line.substr(7));
    else if (starts_with(line, "author "))
      view.author = line.substr(7);
    else if (starts_with(line, "committer "))
      view.committer = line.substr(10);
    if (nl == header.size()) break;
    pos = nl + 1;
  }
  return view;
}

TagView parse_tag(const std::string& data) {
  TagView view;
  size_t split = data.find("\n\n");
  std::string header = split == std::string::npos ? data : data.substr(0, split);
  size_t pos = 0;
  while (pos <= header.size()) {
    size_t nl = header.find('\n', pos);
    if (nl == std::string::npos) nl = header.size();
    std::string line = header.substr(pos, nl - pos);
    if (starts_with(line, "object "))
      view.object = line.substr(7);
    else if (starts_with(line, "type "))
      view.type = line.substr(5);
    else if (starts_with(line, "tag "))
      view.tag = line.substr(4);
    if (nl == header.size()) break;
    pos = nl + 1;
  }
  return view;
}

std::string build_commit(const std::string& tree, const std::vector<std::string>& parents,
                         const std::string& author, const std::string& committer, long long timestamp,
                         const std::string& message) {
  std::string out;
  out.reserve(message.size() + 256);
  out += "tree ";
  out += tree;
  out.push_back('\n');
  for (const auto& p : parents) {
    out += "parent ";
    out += p;
    out.push_back('\n');
  }
  std::string stamp = itoa10(timestamp) + " +0000";
  out += "author ";
  out += author;
  out.push_back(' ');
  out += stamp;
  out.push_back('\n');
  out += "committer ";
  out += committer;
  out.push_back(' ');
  out += stamp;
  out.push_back('\n');
  out.push_back('\n');
  out += message;
  return out;
}

// ---------------------------------------------------------------- pack writer

struct Deflater {
  z_stream zs;
  Deflater() {
    std::memset(&zs, 0, sizeof(zs));
    deflateInit(&zs, 1);
  }
};

struct Inflater {
  z_stream zs;
  Inflater() {
    std::memset(&zs, 0, sizeof(zs));
    inflateInit(&zs);
  }
};

static void append_pack_header(std::string& out, int type, size_t size) {
  unsigned char first = (unsigned char)((type << 4) | (int)(size & 0x0f));
  size >>= 4;
  std::vector<unsigned char> bytes;
  bytes.push_back(first);
  while (size > 0) {
    bytes.back() |= 0x80;
    bytes.push_back((unsigned char)(size & 0x7f));
    size >>= 7;
  }
  out.append((const char*)bytes.data(), bytes.size());
}

std::string write_pack(const std::vector<std::shared_ptr<const Object>>& objects) {
  thread_local Deflater deflater;
  std::string out;
  out.reserve(64 + objects.size() * 64);
  out += "PACK";
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  out.push_back(2);
  uint32_t count = (uint32_t)objects.size();
  out.push_back((char)(count >> 24));
  out.push_back((char)(count >> 16));
  out.push_back((char)(count >> 8));
  out.push_back((char)count);
  for (const auto& obj : objects) {
    append_pack_header(out, obj->type, obj->data.size());
    if (!obj->raw_zlib.empty()) {
      out += obj->raw_zlib;
      continue;
    }
    z_stream& zs = deflater.zs;
    deflateReset(&zs);
    uLong bound = deflateBound(&zs, (uLong)obj->data.size());
    size_t base = out.size();
    out.resize(base + bound);
    zs.next_in = (Bytef*)obj->data.data();
    zs.avail_in = (uInt)obj->data.size();
    zs.next_out = (Bytef*)&out[base];
    zs.avail_out = (uInt)bound;
    deflate(&zs, Z_FINISH);
    size_t produced = bound - zs.avail_out;
    out.resize(base + produced);
  }
  std::string sum = sha1_raw(out.data(), out.size());
  out += sum;
  return out;
}

// ---------------------------------------------------------------- pack reader

static bool apply_delta(const std::string& base, const std::string& delta, std::string& out) {
  size_t pos = 0;
  auto read_varint = [&](size_t& value) -> bool {
    value = 0;
    int shift = 0;
    for (;;) {
      if (pos >= delta.size()) return false;
      unsigned char b = (unsigned char)delta[pos++];
      value |= (size_t)(b & 0x7f) << shift;
      if (!(b & 0x80)) return true;
      shift += 7;
      if (shift > 56) return false;
    }
  };
  size_t base_size = 0, result_size = 0;
  if (!read_varint(base_size) || !read_varint(result_size)) return false;
  if (base_size != base.size()) return false;
  out.clear();
  out.reserve(result_size);
  while (pos < delta.size()) {
    unsigned char op = (unsigned char)delta[pos++];
    if (op & 0x80) {
      size_t offset = 0, size = 0;
      for (int i = 0; i < 4; i++) {
        if (op & (1u << i)) {
          if (pos >= delta.size()) return false;
          offset |= (size_t)(unsigned char)delta[pos++] << (i * 8);
        }
      }
      for (int i = 0; i < 3; i++) {
        if (op & (0x10u << i)) {
          if (pos >= delta.size()) return false;
          size |= (size_t)(unsigned char)delta[pos++] << (i * 8);
        }
      }
      if (size == 0) size = 0x10000;
      if (offset + size > base.size()) return false;
      out.append(base.data() + offset, size);
      continue;
    }
    if (op == 0) return false;
    if (pos + op > delta.size()) return false;
    out.append(delta.data() + pos, op);
    pos += op;
  }
  return out.size() == result_size;
}

static bool inflate_at(const std::string& data, size_t offset, size_t size_hint, std::string& out,
                       size_t& consumed) {
  thread_local Inflater inflater;
  z_stream& zs = inflater.zs;
  if (inflateReset(&zs) != Z_OK) return false;
  size_t avail = data.size() - offset;
  if (avail > 0x7fffffffu) avail = 0x7fffffffu;
  zs.next_in = (Bytef*)(data.data() + offset);
  zs.avail_in = (uInt)avail;
  size_t cap = size_hint > 0 && size_hint < (64u << 20) ? size_hint + 64 : 4096;
  out.resize(cap);
  size_t produced = 0;
  for (;;) {
    if (produced == out.size()) out.resize(out.size() * 2 + 64);
    zs.next_out = (Bytef*)&out[produced];
    zs.avail_out = (uInt)(out.size() - produced);
    int rc = inflate(&zs, Z_NO_FLUSH);
    produced = out.size() - zs.avail_out;
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK && rc != Z_BUF_ERROR) return false;
    if (rc == Z_BUF_ERROR && zs.avail_in == 0) return false;
    if (zs.avail_in == 0 && zs.avail_out != 0) return false;
  }
  out.resize(produced);
  consumed = avail - zs.avail_in;
  return true;
}

bool read_pack(const std::string& data,
               const std::function<std::shared_ptr<const Object>(const std::string&)>& resolve,
               std::vector<std::pair<std::string, std::shared_ptr<const Object>>>& out, std::string& err) {
  out.clear();
  if (data.size() < 32 || std::memcmp(data.data(), "PACK", 4) != 0) {
    err = "not a packfile";
    return false;
  }
  int version = ((unsigned char)data[4] << 24) | ((unsigned char)data[5] << 16) |
                ((unsigned char)data[6] << 8) | (unsigned char)data[7];
  if (version != 2 && version != 3) {
    err = "pack version " + itoa10(version);
    return false;
  }
  size_t count = ((unsigned char)data[8] << 24) | ((unsigned char)data[9] << 16) |
                 ((unsigned char)data[10] << 8) | (unsigned char)data[11];
  if (count > 1000000) {
    err = "pack object count is implausible";
    return false;
  }
  size_t body_len = data.size() - 20;
  std::string want = sha1_raw(data.data(), body_len);
  if (std::memcmp(want.data(), data.data() + body_len, 20) != 0) {
    err = "pack checksum mismatch";
    return false;
  }
  std::unordered_map<std::string, size_t> by_oid;
  std::unordered_map<size_t, size_t> by_offset;
  out.reserve(count);
  size_t pos = 12;
  for (size_t i = 0; i < count; i++) {
    size_t start = pos;
    if (pos >= body_len) {
      err = "pack ended early";
      return false;
    }
    unsigned char b = (unsigned char)data[pos++];
    int type = (b >> 4) & 7;
    size_t size = b & 0x0f;
    int shift = 4;
    while (b & 0x80) {
      if (pos >= body_len) {
        err = "pack header ended early";
        return false;
      }
      b = (unsigned char)data[pos++];
      size |= (size_t)(b & 0x7f) << shift;
      shift += 7;
      if (shift > 35) {
        err = "pack object size is too large";
        return false;
      }
    }
    std::string base_oid;
    size_t base_offset = (size_t)-1;
    if (type == 6) {
      if (pos >= body_len) {
        err = "delta offset ended early";
        return false;
      }
      unsigned char c = (unsigned char)data[pos++];
      size_t distance = c & 0x7f;
      while (c & 0x80) {
        if (pos >= body_len) {
          err = "delta offset ended early";
          return false;
        }
        c = (unsigned char)data[pos++];
        distance = ((distance + 1) << 7) | (c & 0x7f);
      }
      base_offset = start >= distance ? start - distance : (size_t)-1;
    } else if (type == 7) {
      if (pos + 20 > body_len) {
        err = "delta base ended early";
        return false;
      }
      base_oid = hex_encode((const unsigned char*)data.data() + pos, 20);
      pos += 20;
    }
    if (pos >= body_len) {
      err = "pack data ended early";
      return false;
    }
    std::string payload;
    size_t used = 0;
    if (!inflate_at(data, pos, size, payload, used)) {
      err = "inflate failed";
      return false;
    }
    size_t raw_pos = pos;
    pos += used;
    std::shared_ptr<const Object> object;
    if (type >= 1 && type <= 4) {
      if (payload.size() != size) {
        err = "pack object size mismatch";
        return false;
      }
      object = std::make_shared<const Object>(Object{type, std::move(payload), data.substr(raw_pos, used)});
    } else if (type == 6) {
      auto it = by_offset.find(base_offset);
      if (it == by_offset.end()) {
        err = "delta base offset resolves to no object";
        return false;
      }
      const auto& base = out[it->second].second;
      std::string merged;
      if (!apply_delta(base->data, payload, merged)) {
        err = "bad ofs-delta";
        return false;
      }
      object = std::make_shared<const Object>(Object{base->type, std::move(merged)});
    } else if (type == 7) {
      std::shared_ptr<const Object> base;
      auto it = by_oid.find(base_oid);
      if (it != by_oid.end())
        base = out[it->second].second;
      else if (resolve)
        base = resolve(base_oid);
      if (!base) {
        err = "delta base id resolves to no object";
        return false;
      }
      std::string merged;
      if (!apply_delta(base->data, payload, merged)) {
        err = "bad ref-delta";
        return false;
      }
      object = std::make_shared<const Object>(Object{base->type, std::move(merged)});
    } else {
      err = "invalid pack object type " + itoa10(type);
      return false;
    }
    std::string oid = object_id(object->type, object->data);
    by_oid[oid] = out.size();
    by_offset[start] = out.size();
    out.emplace_back(std::move(oid), std::move(object));
  }
  return true;
}

std::string pkt_line(const std::string& payload) {
  size_t length = payload.size() + 4;
  const std::string* use = &payload;
  std::string truncated;
  if (length > 65520) {
    length = 65520;
    truncated = payload.substr(0, length - 4);
    use = &truncated;
  }
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(length);
  out.push_back(hex[(length >> 12) & 0xf]);
  out.push_back(hex[(length >> 8) & 0xf]);
  out.push_back(hex[(length >> 4) & 0xf]);
  out.push_back(hex[length & 0xf]);
  out += *use;
  return out;
}

}  // namespace kestrel
