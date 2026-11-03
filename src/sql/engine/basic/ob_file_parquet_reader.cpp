/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
#include "ob_file_parquet_reader.h"

#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {
constexpr size_t MAX_PAGE = 16 * 1024 * 1024, MAX_VALUES = 262144, MAX_MEMORY = 128 * 1024 * 1024;
struct Bytes {
  const unsigned char *p;
  size_t n, at = 0;
  bool ok = true;
  Bytes(const std::string &s) : p(reinterpret_cast<const unsigned char *>(s.data())), n(s.size()) {}
  Bytes(const unsigned char *data, size_t size) : p(data), n(size) {}
  uint64_t take(size_t k) {
    if (k > 8 || k > n - at) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < k; ++i) v |= uint64_t(p[at++]) << (8 * i);
    return v;
  }
  uint64_t var() {
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      uint64_t b = take(1);
      if (!ok || (shift == 63 && b > 1)) {
        ok = false;
        return 0;
      }
      v |= (b & 127) << shift;
      if (!(b & 128))
        return v;
    }
    ok = false;
    return 0;
  }
  int64_t zig() {
    uint64_t v = var();
    return static_cast<int64_t>((v >> 1) ^ uint64_t(-static_cast<int64_t>(v & 1)));
  }
  std::string string(size_t k) {
    if (k > n - at) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char *>(p + at), k);
    at += k;
    return s;
  }
};
struct Value {
  int64_t integer = 0;
  std::string string;
  std::map<int, Value> fields;
  std::vector<Value> list;
  const Value &get(int key) const {
    static const Value empty;
    auto it = fields.find(key);
    return it == fields.end() ? empty : it->second;
  }
  bool has(int key) const { return fields.count(key) != 0; }
  int64_t num(int key, int64_t fallback = 0) const {
    return has(key) ? get(key).integer : fallback;
  }
};
bool compact(Bytes &b, int type, Value &v, size_t &budget, int depth = 0) {
  if (depth > 32 || budget-- == 0)
    return false;
  switch (type) {
    case 1:
      v.integer = 1;
      break;
    case 2:
      v.integer = 0;
      break;
    case 3:
      v.integer = static_cast<int8_t>(b.take(1));
      break;
    case 4:
    case 5:
    case 6:
      v.integer = b.zig();
      break;
    case 7:
      b.take(8);
      break;
    case 8: {
      uint64_t n = b.var();
      if (n > MAX_PAGE)
        return false;
      v.string = b.string(n);
      break;
    }
    case 9:
    case 10: {
      unsigned h = b.take(1);
      uint64_t n = h >> 4;
      if (n == 15)
        n = b.var();
      if (n > MAX_VALUES || n > budget)
        return false;
      v.list.resize(n);
      for (auto &e : v.list)
        if (!compact(b, h & 15, e, budget, depth + 1))
          return false;
      break;
    }
    case 11: {
      uint64_t n = b.var();
      if (n > MAX_VALUES || n > budget)
        return false;
      if (n) {
        unsigned h = b.take(1);
        for (uint64_t i = 0; i < n; ++i) {
          Value key, item;
          if (!compact(b, h >> 4, key, budget, depth + 1) ||
              !compact(b, h & 15, item, budget, depth + 1))
            return false;
        }
      }
      break;
    }
    case 12: {
      int last = 0;
      for (;;) {
        unsigned h = b.take(1);
        if (!b.ok)
          return false;
        if (h == 0)
          break;
        int key = (h >> 4) ? last + (h >> 4) : b.zig();
        if (key < 1 || key > 32767 || v.fields.count(key))
          return false;
        last = key;
        if (!compact(b, h & 15, v.fields[key], budget, depth + 1))
          return false;
      }
      break;
    }
    default:
      return false;
  }
  return b.ok;
}
bool read_at(int fd, int64_t at, size_t n, std::string &s) {
  if (at < 0 || n > MAX_PAGE)
    return false;
  s.resize(n);
  size_t done = 0;
  while (done < n) {
    ssize_t got = ::pread(fd, &s[done], n - done, at + done);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return false;
    done += got;
  }
  return true;
}
bool snappy(const std::string &in, size_t expected, std::string &out) {
  Bytes b(in);
  uint64_t n = b.var();
  if (!b.ok || n != expected || n > MAX_PAGE)
    return false;
  out.clear();
  out.reserve(n);
  while (b.ok && b.at < b.n && out.size() < n) {
    unsigned tag = b.take(1), kind = tag & 3;
    size_t len = 0, offset = 0;
    if (kind == 0) {
      len = tag >> 2;
      if (len >= 60) {
        size_t count = len - 59;
        len = b.take(count);
      }
      ++len;
      if (len > n - out.size() || len > b.n - b.at)
        return false;
      out.append(b.string(len));
    } else {
      if (kind == 1) {
        len = 4 + ((tag >> 2) & 7);
        offset = ((tag & 224) << 3) | b.take(1);
      } else {
        len = 1 + (tag >> 2);
        offset = b.take(kind == 2 ? 2 : 4);
      }
      if (!b.ok || offset == 0 || offset > out.size() || len > n - out.size())
        return false;
      for (size_t i = 0; i < len; ++i) out.push_back(out[out.size() - offset]);
    }
  }
  return b.ok && out.size() == n && b.at == b.n;
}
bool decompress(int codec, const std::string &in, size_t size, std::string &out) {
  if (codec == 0) {
    out = in;
    return out.size() == size;
  }
  return codec == 1 && snappy(in, size, out);
}
bool hybrid(Bytes &b, unsigned bits, size_t count, std::vector<uint64_t> &out) {
  if (bits > 64 || count > MAX_VALUES)
    return false;
  out.clear();
  out.reserve(count);
  while (b.ok && out.size() < count) {
    uint64_t h = b.var();
    if (h == 0 || !b.ok)
      return false;
    if (!(h & 1)) {
      uint64_t value = b.take((bits + 7) / 8), n = h >> 1;
      if (n > MAX_VALUES || n > count - out.size())
        return false;
      out.insert(out.end(), n, value);
    } else {
      uint64_t groups = h >> 1;
      if (groups > MAX_VALUES / 8)
        return false;
      size_t n = groups * 8, bytes = groups * bits;
      if (bytes > b.n - b.at)
        return false;
      for (size_t i = 0; i < n && out.size() < count; ++i) {
        uint64_t value = 0;
        for (unsigned bit = 0; bit < bits; ++bit) {
          size_t at = i * bits + bit;
          value |= uint64_t((b.p[b.at + at / 8] >> (at % 8)) & 1) << bit;
        }
        out.push_back(value);
      }
      b.at += bytes;
    }
  }
  return b.ok && out.size() == count;
}

bool delta_binary(Bytes &b, std::vector<int64_t> &out) {
  uint64_t block = b.var(), minis = b.var(), total = b.var();
  int64_t first = b.zig();
  if (!b.ok || block == 0 || block > MAX_VALUES || block % 128 || minis == 0 || minis > block ||
      block % minis || ((block / minis) % 32) || total > MAX_VALUES)
    return false;
  out.clear();
  if (total == 0)
    return true;
  out.push_back(first);
  const size_t per = block / minis;
  while (b.ok && out.size() < total) {
    int64_t minimum = b.zig();
    std::vector<unsigned> widths;
    for (size_t i = 0; i < minis; ++i) {
      unsigned width = b.take(1);
      if (width > 64)
        return false;
      widths.push_back(width);
    }
    for (auto bits : widths) {
      size_t bytes = per * bits / 8;
      if (bytes > b.n - b.at)
        return false;
      for (size_t i = 0; i < per && out.size() < total; ++i) {
        uint64_t value = 0;
        for (unsigned bit = 0; bit < bits; ++bit) {
          size_t at = i * bits + bit;
          value |= uint64_t((b.p[b.at + at / 8] >> (at % 8)) & 1) << bit;
        }
        out.push_back(static_cast<int64_t>(static_cast<uint64_t>(out.back()) +
                                           static_cast<uint64_t>(minimum) + value));
      }
      b.at += bytes;
    }
  }
  return b.ok && out.size() == total;
}
bool delta_strings(Bytes &b, size_t n, bool prefix_encoded, std::vector<std::string> &out) {
  std::vector<int64_t> prefix, lengths;
  if (prefix_encoded && (!delta_binary(b, prefix) || prefix.size() != n))
    return false;
  if (!delta_binary(b, lengths) || lengths.size() != n)
    return false;
  out.clear();
  size_t total = 0;
  for (size_t i = 0; i < n; ++i) {
    if (lengths[i] < 0 || lengths[i] > MAX_FIELD_BYTES)
      return false;
    std::string value;
    if (prefix_encoded) {
      if (prefix[i] < 0 || (i == 0 && prefix[i] != 0) ||
          (i && static_cast<uint64_t>(prefix[i]) > out.back().size()))
        return false;
      if (i)
        value = out.back().substr(0, prefix[i]);
    }
    value += b.string(lengths[i]);
    if (!b.ok || value.size() > MAX_FIELD_BYTES || !valid_utf8(value))
      return false;
    total += value.size();
    if (total > MAX_PAGE)
      return false;
    out.push_back(std::move(value));
  }
  return true;
}
struct Physical {
  int type = 0, width = 0, converted = -1;
  bool nullable = false;
  Type logical = Type::NIL;
};
std::string civil(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  unsigned doe = days - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  int m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
  if (y < 1 || y > 9999)
    return {};
  char text[32];
  std::snprintf(text, sizeof(text), "%04lld-%02d-%02u", static_cast<long long>(y), m, d);
  return text;
}
bool plain(Bytes &b, const Physical &p, size_t n, std::vector<Cell> &values) {
  if (n > MAX_VALUES)
    return false;
  if (p.logical == Type::NIL && n != 0) return false;
  values.clear();
  values.reserve(n);
  size_t string_bytes = 0;
  for (size_t i = 0; i < n && b.ok; ++i) {
    Cell c;
    c.null = false;
    c.type = p.logical;
    int64_t integer = 0;
    char text[64];
    if (p.type == 0) {
      size_t byte = i / 8;
      if (byte >= b.n - b.at)
        return false;
      c.text = ((b.p[b.at + byte] >> (i % 8)) & 1) ? "true" : "false";
    } else if (p.type == 1 || p.type == 2) {
      integer = p.type == 1 ? static_cast<int32_t>(b.take(4)) : static_cast<int64_t>(b.take(8));
      if (p.logical == Type::DATE)
        c.text = civil(integer);
      else if (p.logical == Type::DATETIME) {
        if (p.converted == 9) {
          if (integer > INT64_MAX / 1000 || integer < INT64_MIN / 1000)
            return false;
          integer *= 1000;
        }
        int64_t days = integer / 86400000000LL, rest = integer % 86400000000LL;
        if (rest < 0) {
          --days;
          rest += 86400000000LL;
        }
        c.text = civil(days);
        if (c.text.empty())
          return false;
        std::snprintf(text, sizeof(text), " %02lld:%02lld:%02lld.%06lld",
                      static_cast<long long>(rest / 3600000000LL),
                      static_cast<long long>((rest / 60000000) % 60),
                      static_cast<long long>((rest / 1000000) % 60),
                      static_cast<long long>(rest % 1000000));
        c.text += text;
      } else
        c.text = p.converted == 14 ? std::to_string(static_cast<uint64_t>(integer))
                                   : (p.converted >= 11 && p.converted <= 13
                                          ? std::to_string(static_cast<uint32_t>(integer))
                                          : std::to_string(integer));
      if (c.text.empty())
        return false;
    } else if (p.type == 4 || p.type == 5) {
      double value;
      if (p.type == 4) {
        uint32_t raw = b.take(4);
        float f;
        std::memcpy(&f, &raw, 4);
        value = f;
      } else {
        uint64_t raw = b.take(8);
        std::memcpy(&value, &raw, 8);
      }
      if (!std::isfinite(value))
        return false;
      std::snprintf(text, sizeof(text), "%.17g", value);
      c.text = text;
    } else if (p.type == 6 || p.type == 7) {
      size_t len = p.type == 6 ? b.take(4) : p.width;
      if (len > MAX_FIELD_BYTES || len > b.n - b.at)
        return false;
      c.text = b.string(len);
      if (!valid_utf8(c.text))
        return false;
    } else
      return false;
    string_bytes += c.text.size();
    if (string_bytes > MAX_PAGE)
      return false;
    values.push_back(std::move(c));
  }
  if (p.type == 0)
    b.at += (n + 7) / 8;
  return b.ok && values.size() == n;
}
struct Chunk {
  int64_t offset = 0, end = 0, values = 0;
  int codec = 0;
};
struct Group {
  uint64_t rows = 0;
  std::vector<Chunk> chunks;
};
struct Cursor {
  int64_t offset = 0;
  std::vector<Cell> dictionary, page;
  size_t index = 0;
  uint64_t loaded = 0;
};
size_t memory(const std::vector<Cell> &v) {
  size_t size = v.capacity() * sizeof(Cell);
  for (auto &c : v) size += c.text.capacity();
  return size;
}
}  // namespace
struct ParquetReader::Impl {
  int fd = -1;
  int64_t size = 0, footer = 0;
  std::vector<Physical> types;
  std::vector<Group> groups;
  std::vector<size_t> projection;
  size_t group = 0;
  uint64_t row = 0;
  std::vector<Cursor> cursors;
  Status error(std::string &e, const char *m) {
    e = std::string("Parquet: ") + m;
    return Status::INVALID;
  }
  bool page(size_t column) {
    Cursor &c = cursors[column];
    const Chunk &chunk = groups[group].chunks[column];
    const Physical &physical = types[column];
    while (c.offset < chunk.end) {
      std::string header;
      size_t available = std::min<int64_t>(65536, chunk.end - c.offset);
      if (!read_at(fd, c.offset, available, header))
        return false;
      Bytes h(header);
      Value meta;
      size_t budget = 4096;
      if (!compact(h, 12, meta, budget))
        return false;
      int64_t raw = meta.num(2, -1), compressed = meta.num(3, -1), kind = meta.num(1, -1);
      if (raw < 0 || compressed < 0 || raw > MAX_PAGE || compressed > MAX_PAGE ||
          compressed > chunk.end - c.offset - static_cast<int64_t>(h.at))
        return false;
      c.offset += h.at;
      std::string packed, data;
      if (!read_at(fd, c.offset, compressed, packed))
        return false;
      c.offset += compressed;
      if (meta.has(4)) {
        uint32_t crc = ~uint32_t(0);
        for (unsigned char byte : packed) {
          crc ^= byte;
          for (int bit = 0; bit < 8; ++bit)
            crc =
                (crc >> 1) ^ (0xedb88320U & static_cast<uint32_t>(-static_cast<int32_t>(crc & 1)));
        }
        if ((~crc) != static_cast<uint32_t>(meta.num(4)))
          return false;
      }
      const Value &info = meta.get(kind == 2 ? 7 : kind == 3 ? 8 : 5);
      int64_t count = info.num(1, -1);
      if (count < 0 || count > MAX_VALUES)
        return false;
      int encoding = info.num(kind == 3 ? 4 : 2, -1);
      size_t definition = 0, repetition = 0;
      if (kind == 3) {
        int64_t def = info.num(5, -1), rep = info.num(6, -1);
        if (def < 0 || rep != 0 || def > compressed || def > raw)
          return false;
        definition = def;
        repetition = rep;
        std::string decoded;
        if (!decompress(info.num(7, 1) ? chunk.codec : 0, packed.substr(definition + repetition),
                        raw - definition - repetition, decoded))
          return false;
        data = packed.substr(0, definition + repetition) + decoded;
      } else if (!decompress(chunk.codec, packed, raw, data))
        return false;
      Bytes b(data);
      if (kind == 2) {
        if (encoding != 0 || !c.dictionary.empty() || !plain(b, physical, count, c.dictionary) ||
            b.at != b.n)
          return false;
        continue;
      }
      if (kind != 0 && kind != 3)
        return false;
      if (count == 0 || c.loaded + count > static_cast<uint64_t>(chunk.values))
        return false;
      std::vector<uint64_t> defs;
      if (physical.nullable) {
        if (kind == 0) {
          if (info.num(3) != 3)
            return false;
          definition = b.take(4);
        }
        if (definition > b.n - b.at)
          return false;
        Bytes levels(b.p + b.at, definition);
        if (!hybrid(levels, 1, count, defs))
          return false;
        b.at += definition;
      } else
        defs.assign(count, 1);
      size_t nonnull = 0;
      for (auto level : defs) {
        if (level > 1)
          return false;
        nonnull += level;
      }
      if (kind == 3 && static_cast<int64_t>(count - nonnull) != info.num(2, -1))
        return false;
      std::vector<Cell> values;
      if (encoding == 0) {
        if (!plain(b, physical, nonnull, values))
          return false;
      } else if (encoding == 2 || encoding == 8) {
        unsigned width = b.take(1);
        std::vector<uint64_t> indexes;
        if (!hybrid(b, width, nonnull, indexes))
          return false;
        for (auto index : indexes) {
          if (index >= c.dictionary.size())
            return false;
          values.push_back(c.dictionary[index]);
        }
      } else if (encoding == 5 && (physical.type == 1 || physical.type == 2)) {
        std::vector<int64_t> ints;
        if (!delta_binary(b, ints) || ints.size() != nonnull)
          return false;
        std::string raw_values;
        size_t width = physical.type == 1 ? 4 : 8;
        raw_values.reserve(width * nonnull);
        for (auto value : ints)
          for (size_t j = 0; j < width; ++j)
            raw_values.push_back(static_cast<uint64_t>(value) >> (8 * j));
        Bytes raw(raw_values);
        if (!plain(raw, physical, nonnull, values))
          return false;
      } else if ((encoding == 6 || encoding == 7) && physical.type == 6) {
        std::vector<std::string> strings;
        if (!delta_strings(b, nonnull, encoding == 7, strings))
          return false;
        for (auto &value : strings) values.push_back(Cell{false, Type::STRING, std::move(value)});
      } else if (encoding == 9 && (physical.type == 1 || physical.type == 2 || physical.type == 4 ||
                                   physical.type == 5)) {
        size_t width = physical.type == 1 || physical.type == 4 ? 4 : 8;
        if (nonnull * width > b.n - b.at)
          return false;
        std::string raw_values(nonnull * width, '\0');
        for (size_t i = 0; i < nonnull; ++i)
          for (size_t j = 0; j < width; ++j)
            raw_values[i * width + j] = b.p[b.at + j * nonnull + i];
        b.at += raw_values.size();
        Bytes raw(raw_values);
        if (!plain(raw, physical, nonnull, values))
          return false;
      } else if (encoding == 3 && physical.type == 0) {
        size_t bytes = b.take(4);
        if (bytes > b.n - b.at)
          return false;
        Bytes bits(b.p + b.at, bytes);
        std::vector<uint64_t> bools;
        if (!hybrid(bits, 1, nonnull, bools))
          return false;
        b.at += bytes;
        for (auto value : bools)
          values.push_back(Cell{false, Type::BOOL, value ? "true" : "false"});
      } else
        return false;
      if (!b.ok || b.at != b.n)
        return false;
      c.page.clear();
      c.page.reserve(count);
      size_t index = 0;
      for (auto level : defs) c.page.push_back(level ? std::move(values[index++]) : Cell());
      c.index = 0;
      c.loaded += count;
      size_t bytes = 0;
      for (auto &cursor : cursors) {
        bytes += memory(cursor.page) + memory(cursor.dictionary);
        if (bytes > MAX_MEMORY)
          return false;
      }
      return true;
    }
    return false;
  }
};
ParquetReader::ParquetReader() : impl_(new Impl) {}
ParquetReader::~ParquetReader() = default;
void ParquetReader::rewind() {
  impl_->group = 0;
  impl_->row = 0;
  impl_->cursors.clear();
}
Status ParquetReader::open(int fd, Schema &schema, const std::vector<size_t> &projection,
                           std::string &error) {
  impl_.reset(new Impl);
  auto &s = *impl_;
  s.fd = fd;
  s.size = schema.identity.size;
  s.projection = projection;
  if (s.size < 12)
    return s.error(error, "truncated file");
  std::string trailer, magic;
  if (!read_at(fd, 0, 4, magic) || magic != "PAR1" || !read_at(fd, s.size - 8, 8, trailer) ||
      trailer.substr(4) != "PAR1")
    return s.error(error, "invalid magic/footer");
  Bytes tail(trailer);
  size_t length = tail.take(4);
  if (length > 8 * 1024 * 1024 || length > static_cast<uint64_t>(s.size - 12))
    return s.error(error, "footer size limit");
  s.footer = s.size - 8 - length;
  std::string footer;
  if (!read_at(fd, s.footer, length, footer))
    return s.error(error, "cannot read footer");
  Bytes bytes(footer);
  Value meta;
  size_t budget = 100000;
  if (!compact(bytes, 12, meta, budget) || bytes.at != bytes.n)
    return s.error(error, "invalid compact metadata");
  const auto &columns = meta.get(2).list;
  if (columns.size() < 2 || columns.size() > MAX_COLUMNS + 1 ||
      columns[0].num(5) != static_cast<int64_t>(columns.size() - 1))
    return s.error(error, "only flat schema is supported");
  schema.columns.clear();
  std::set<std::string> names;
  for (size_t i = 1; i < columns.size(); ++i) {
    const auto &col = columns[i];
    Physical p;
    p.type = col.num(1, -1);
    p.width = col.num(2);
    p.converted = col.num(6, -1);
    p.nullable = col.num(3) == 1;
    if (col.num(5) != 0 || col.num(3) < 0 || col.num(3) > 1)
      return s.error(error, "nested/repeated columns are unsupported");
    std::string name = col.get(4).string, key = name;
    for (char &ch : key)
      if (ch >= 'A' && ch <= 'Z')
        ch += 'a' - 'A';
    if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos ||
        !valid_utf8(name) || !names.insert(key).second)
      return s.error(error, "invalid column name");
    if (p.type == 0)
      p.logical = Type::BOOL;
    else if (p.type == 1 || p.type == 2)
      p.logical = Type::INT;
    else if (p.type == 4 || p.type == 5)
      p.logical = Type::DOUBLE;
    else if (p.type == 6 || p.type == 7)
      p.logical = Type::STRING;
    else
      return s.error(error, "unsupported physical type");
    if (p.converted == 6)
      p.logical = Type::DATE;
    else if (p.converted == 9 || p.converted == 10)
      p.logical = Type::DATETIME;
    else if (p.converted == 14)
      p.logical = Type::STRING;
    else if (p.converted != -1 && p.converted != 0 && (p.converted < 11 || p.converted > 18))
      return s.error(error, "unsupported logical type");
    if ((p.logical == Type::DATE && p.type != 1) || (p.logical == Type::DATETIME && p.type != 2) ||
        (p.type == 7 && (p.width < 1 || p.width > MAX_FIELD_BYTES)))
      return s.error(error, "invalid logical type");
    const Value &logical = col.get(10);
    for (const auto &field : logical.fields)
      if (field.first != 1 && field.first != 6 && field.first != 8 && field.first != 10 &&
          field.first != 11)
        return s.error(error, "unsupported logical annotation");
    if (logical.has(11))
      p.logical = Type::NIL;
    if (logical.has(6))
      p.logical = Type::DATE;
    if (logical.has(8)) {
      const Value &unit = logical.get(8).get(2);
      if (unit.has(1))
        p.converted = 9;
      else if (unit.has(2))
        p.converted = 10;
      else
        return s.error(error, "unsupported timestamp unit");
      p.logical = Type::DATETIME;
    }
    if (logical.has(10)) {
      const Value &integer = logical.get(10);
      const int bits = integer.num(1, -1);
      if ((bits != 8 && bits != 16 && bits != 32 && bits != 64) ||
          (bits == 64 ? p.type != 2 : p.type != 1))
        return s.error(error, "invalid integer logical type");
      if (integer.num(2, 1) == 0) {
        p.converted = bits == 64 ? 14 : 13;
        p.logical = bits == 64 ? Type::STRING : Type::INT;
      }
    }
    if ((p.logical == Type::DATE && p.type != 1) ||
        (p.logical == Type::DATETIME && p.type != 2))
      return s.error(error, "logical type does not match physical type");
    s.types.push_back(p);
    schema.columns.push_back({name, p.logical, p.nullable,
                              p.logical == Type::DATETIME ? (p.converted == 9 ? 3 : 6) : 0});
  }
  uint64_t total = 0;
  const auto &groups = meta.get(4).list;
  if (groups.size() > 10000)
    return s.error(error, "row group limit");
  for (const auto &value : groups) {
    Group group;
    int64_t rows = value.num(3, -1);
    if (rows < 0)
      return s.error(error, "invalid row count");
    group.rows = rows;
    if (value.get(1).list.size() != s.types.size())
      return s.error(error, "column count mismatch");
    for (size_t i = 0; i < s.types.size(); ++i) {
      const auto &column = value.get(1).list[i];
      const auto &data = column.get(3);
      Chunk c;
      if (column.has(1) || column.has(8) || column.has(9) || !column.has(3) ||
          data.num(1, -1) != s.types[i].type)
        return s.error(error, "external/encrypted column is forbidden");
      if (data.get(3).list.size() != 1 || data.get(3).list[0].string != schema.columns[i].name)
        return s.error(error, "column path mismatch");
      c.offset = data.num(11, data.num(9, -1));
      c.values = data.num(5, -1);
      c.codec = data.num(4, -1);
      int64_t size = data.num(7, -1);
      if (c.offset < 4 || size < 0 || c.offset > s.footer || size > s.footer - c.offset ||
          c.values != rows || (c.codec != 0 && c.codec != 1))
        return s.error(error, "invalid chunk or unsupported compression");
      c.end = c.offset + size;
      group.chunks.push_back(c);
    }
    if (group.rows > UINT64_MAX - total)
      return s.error(error, "row count overflow");
    total += group.rows;
    s.groups.push_back(std::move(group));
  }
  if (meta.num(3, -1) < 0 || total != static_cast<uint64_t>(meta.num(3)))
    return s.error(error, "footer row count mismatch");
  schema.rows = total;
  for (auto i : s.projection)
    if (i >= s.types.size())
      return s.error(error, "invalid projection");
  return Status::OK;
}
Status ParquetReader::next(std::vector<Cell> &row, std::string &error) {
  auto &s = *impl_;
  while (s.group < s.groups.size() && s.row == s.groups[s.group].rows) {
    ++s.group;
    s.row = 0;
    s.cursors.clear();
  }
  if (s.group == s.groups.size())
    return Status::END;
  if (s.cursors.empty()) {
    s.cursors.resize(s.types.size());
    for (size_t i = 0; i < s.types.size(); ++i)
      s.cursors[i].offset = s.groups[s.group].chunks[i].offset;
  }
  row.assign(s.types.size(), Cell());
  for (auto column : s.projection) {
    auto &cursor = s.cursors[column];
    if (cursor.index == cursor.page.size() && !s.page(column))
      return s.error(error, "invalid/unsupported page encoding or page resource limit");
    row[column] = cursor.page[cursor.index++];
  }
  ++s.row;
  return Status::OK;
}
}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
