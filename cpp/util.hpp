// Small string helpers and a compact JSON reader/writer.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kestrel {

inline std::string to_lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return s;
}

inline bool ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x + 32);
    if (y >= 'A' && y <= 'Z') y = char(y + 32);
    if (x != y) return false;
  }
  return true;
}

inline bool starts_with(const std::string& s, const char* pre) {
  size_t n = std::strlen(pre);
  return s.size() >= n && std::memcmp(s.data(), pre, n) == 0;
}
inline bool starts_with(const std::string& s, const std::string& pre) {
  return s.size() >= pre.size() && std::memcmp(s.data(), pre.data(), pre.size()) == 0;
}
inline bool ends_with(const std::string& s, const char* suf) {
  size_t n = std::strlen(suf);
  return s.size() >= n && std::memcmp(s.data() + s.size() - n, suf, n) == 0;
}
inline bool ends_with(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && std::memcmp(s.data() + s.size() - suf.size(), suf.data(), suf.size()) == 0;
}

inline std::string trim_ws(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}

inline std::vector<std::string> split_str(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t at = s.find(sep, start);
    if (at == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, at - start));
    start = at + 1;
  }
  return out;
}

inline std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    size_t j = i;
    while (j < s.size() && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) j++;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

inline int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::string url_decode(const std::string& s, bool plus_as_space) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '%' && i + 2 < s.size()) {
      int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(char((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (c == '+' && plus_as_space)
      out.push_back(' ');
    else
      out.push_back(c);
  }
  return out;
}

inline std::string b64_encode(const std::string& in) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < in.size()) {
    uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back(tbl[v & 63]);
    i += 3;
  }
  if (i + 1 == in.size()) {
    uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == in.size()) {
    uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

inline bool b64_decode(const std::string& text, std::string& out) {
  out.clear();
  int vals[4];
  int n = 0;
  for (char c : text) {
    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') continue;
    int v;
    if (c >= 'A' && c <= 'Z')
      v = c - 'A';
    else if (c >= 'a' && c <= 'z')
      v = c - 'a' + 26;
    else if (c >= '0' && c <= '9')
      v = c - '0' + 52;
    else if (c == '+')
      v = 62;
    else if (c == '/')
      v = 63;
    else if (c == '=')
      v = -2;
    else
      return false;
    vals[n++] = v;
    if (n == 4) {
      if (vals[0] < 0 || vals[1] < 0) return false;
      out.push_back(char((vals[0] << 2) | (vals[1] >> 4)));
      if (vals[2] == -2) {
        if (vals[3] != -2) return false;
      } else {
        if (vals[2] < 0) return false;
        out.push_back(char(((vals[1] & 15) << 4) | (vals[2] >> 2)));
        if (vals[3] == -2) {
          // done
        } else {
          if (vals[3] < 0) return false;
          out.push_back(char(((vals[2] & 3) << 6) | vals[3]));
        }
      }
      n = 0;
    }
  }
  return n == 0;
}

inline std::string hex_encode(const unsigned char* p, size_t n) {
  static const char* hex = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hex[p[i] >> 4];
    out[i * 2 + 1] = hex[p[i] & 15];
  }
  return out;
}

inline void append_uint(std::string& out, unsigned long long v) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = char('0' + (v % 10));
    v /= 10;
  } while (v);
  while (n) out.push_back(tmp[--n]);
}

inline std::string itoa10(long long v) {
  std::string out;
  if (v < 0) {
    out.push_back('-');
    append_uint(out, (unsigned long long)(-(v + 1)) + 1);
  } else {
    append_uint(out, (unsigned long long)v);
  }
  return out;
}

inline std::string request_id_hex(uint64_t n) {
  static const char* hex = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; i--) {
    out[i] = hex[n & 15];
    n >>= 4;
  }
  return out;
}

inline void append_json_string(std::string& out, const std::string& s) {
  static const char* hex = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 15]);
        } else {
          out.push_back(char(c));
        }
    }
  }
  out.push_back('"');
}

// ---------------------------------------------------------------- JSON value

struct JVal {
  enum Type { NUL = 0, BOOL, INT, DBL, STR, ARR, OBJ } type = NUL;
  bool b = false;
  long long i = 0;
  double d = 0;
  std::string s;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;

  JVal() = default;
  static JVal null() { return JVal(); }
  static JVal boolean(bool v) {
    JVal j;
    j.type = BOOL;
    j.b = v;
    return j;
  }
  static JVal integer(long long v) {
    JVal j;
    j.type = INT;
    j.i = v;
    return j;
  }
  static JVal number(double v) {
    JVal j;
    j.type = DBL;
    j.d = v;
    return j;
  }
  static JVal string(std::string v) {
    JVal j;
    j.type = STR;
    j.s = std::move(v);
    return j;
  }
  static JVal array() {
    JVal j;
    j.type = ARR;
    return j;
  }
  static JVal object() {
    JVal j;
    j.type = OBJ;
    return j;
  }
  JVal& set(const std::string& k, JVal v) {
    for (auto& kv : obj)
      if (kv.first == k) {
        kv.second = std::move(v);
        return *this;
      }
    obj.emplace_back(k, std::move(v));
    return *this;
  }
  JVal& push(JVal v) {
    arr.push_back(std::move(v));
    return *this;
  }
};

inline const JVal* jget(const JVal& o, const std::string& k) {
  if (o.type != JVal::OBJ) return nullptr;
  for (const auto& kv : o.obj)
    if (kv.first == k) return &kv.second;
  return nullptr;
}

inline std::string jstr(const JVal& o, const std::string& k, const std::string& fallback = "") {
  const JVal* v = jget(o, k);
  return v && v->type == JVal::STR ? v->s : fallback;
}

inline bool jbool(const JVal& o, const std::string& k, bool fallback = false) {
  const JVal* v = jget(o, k);
  return v && v->type == JVal::BOOL ? v->b : fallback;
}

inline long long jint(const JVal& o, const std::string& k, long long fallback = 0) {
  const JVal* v = jget(o, k);
  if (!v) return fallback;
  if (v->type == JVal::INT) return v->i;
  if (v->type == JVal::DBL) return (long long)v->d;
  return fallback;
}

struct JsonParser {
  const char* p;
  const char* end;
  bool ok = true;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }
  bool lit(const char* text) {
    size_t n = std::strlen(text);
    if ((size_t)(end - p) < n || std::memcmp(p, text, n) != 0) return false;
    p += n;
    return true;
  }
  bool parse_string(std::string& out) {
    if (p >= end || *p != '"') return false;
    p++;
    out.clear();
    while (p < end) {
      unsigned char c = (unsigned char)*p++;
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back((char)c);
        continue;
      }
      if (p >= end) return false;
      char esc = *p++;
      switch (esc) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (end - p < 4) return false;
          int cp = 0;
          for (int k = 0; k < 4; k++) {
            int v = hexval(p[k]);
            if (v < 0) return false;
            cp = cp * 16 + v;
          }
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            int lo = 0;
            bool good = true;
            for (int k = 0; k < 4; k++) {
              int v = hexval(p[2 + k]);
              if (v < 0) {
                good = false;
                break;
              }
              lo = lo * 16 + v;
            }
            if (good && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            }
          }
          if (cp < 0x80) {
            out.push_back((char)cp);
          } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
          } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
          } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }
  bool parse_value(JVal& out, int depth) {
    if (depth > 200) return false;
    ws();
    if (p >= end) return false;
    char c = *p;
    if (c == '"') {
      out.type = JVal::STR;
      return parse_string(out.s);
    }
    if (c == '{') {
      p++;
      out.type = JVal::OBJ;
      ws();
      if (p < end && *p == '}') {
        p++;
        return true;
      }
      for (;;) {
        ws();
        std::string key;
        if (!parse_string(key)) return false;
        ws();
        if (p >= end || *p != ':') return false;
        p++;
        JVal v;
        if (!parse_value(v, depth + 1)) return false;
        out.obj.emplace_back(std::move(key), std::move(v));
        ws();
        if (p < end && *p == ',') {
          p++;
          continue;
        }
        if (p < end && *p == '}') {
          p++;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      p++;
      out.type = JVal::ARR;
      ws();
      if (p < end && *p == ']') {
        p++;
        return true;
      }
      for (;;) {
        JVal v;
        if (!parse_value(v, depth + 1)) return false;
        out.arr.push_back(std::move(v));
        ws();
        if (p < end && *p == ',') {
          p++;
          continue;
        }
        if (p < end && *p == ']') {
          p++;
          return true;
        }
        return false;
      }
    }
    if (c == 't') {
      if (!lit("true")) return false;
      out.type = JVal::BOOL;
      out.b = true;
      return true;
    }
    if (c == 'f') {
      if (!lit("false")) return false;
      out.type = JVal::BOOL;
      out.b = false;
      return true;
    }
    if (c == 'n') {
      if (!lit("null")) return false;
      out.type = JVal::NUL;
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const char* start = p;
      bool isDouble = false;
      if (*p == '-') p++;
      while (p < end && *p >= '0' && *p <= '9') p++;
      if (p < end && *p == '.') {
        isDouble = true;
        p++;
        while (p < end && *p >= '0' && *p <= '9') p++;
      }
      if (p < end && (*p == 'e' || *p == 'E')) {
        isDouble = true;
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        while (p < end && *p >= '0' && *p <= '9') p++;
      }
      std::string text(start, p - start);
      if (isDouble) {
        out.type = JVal::DBL;
        out.d = std::strtod(text.c_str(), nullptr);
      } else {
        out.type = JVal::INT;
        out.i = std::strtoll(text.c_str(), nullptr, 10);
      }
      return true;
    }
    return false;
  }
};

inline bool json_parse(const std::string& text, JVal& out) {
  JsonParser parser{text.data(), text.data() + text.size()};
  if (!parser.parse_value(out, 0)) return false;
  parser.ws();
  return parser.p == parser.end;
}

inline void json_dump(const JVal& v, std::string& out) {
  switch (v.type) {
    case JVal::NUL:
      out += "null";
      break;
    case JVal::BOOL:
      out += v.b ? "true" : "false";
      break;
    case JVal::INT:
      if (v.i < 0) {
        out.push_back('-');
        append_uint(out, (unsigned long long)(-(v.i + 1)) + 1);
      } else {
        append_uint(out, (unsigned long long)v.i);
      }
      break;
    case JVal::DBL: {
      char tmp[40];
      int n = std::snprintf(tmp, sizeof(tmp), "%.17g", v.d);
      out.append(tmp, n);
      break;
    }
    case JVal::STR:
      append_json_string(out, v.s);
      break;
    case JVal::ARR:
      out.push_back('[');
      for (size_t i = 0; i < v.arr.size(); i++) {
        if (i) out.push_back(',');
        json_dump(v.arr[i], out);
      }
      out.push_back(']');
      break;
    case JVal::OBJ:
      out.push_back('{');
      for (size_t i = 0; i < v.obj.size(); i++) {
        if (i) out.push_back(',');
        append_json_string(out, v.obj[i].first);
        out.push_back(':');
        json_dump(v.obj[i].second, out);
      }
      out.push_back('}');
      break;
  }
}

inline std::string json_text(const JVal& v) {
  std::string out;
  json_dump(v, out);
  return out;
}

}  // namespace kestrel
