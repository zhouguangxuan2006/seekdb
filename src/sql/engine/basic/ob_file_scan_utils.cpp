/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
#include "ob_file_scan_utils.h"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>

#include "ob_file_parquet_reader.h"
#include "rapidjson/document.h"
#include "rapidjson/memorystream.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {
const std::string MAGIC = "seekdb:file-scan:v1:";
Status fail(Status status, std::string &error, const std::string &message) {
  error = message;
  return status;
}
std::string lower(std::string value) {
  for (char &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}
bool within(const std::string &path, const std::string &root) {
  return root == "/" || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
                         path[root.size()] == '/');
}
Identity identity_of(const struct stat &st) {
  Identity i;
  i.device = st.st_dev;
  i.inode = st.st_ino;
  i.size = st.st_size;
#ifdef __APPLE__
  i.mtime_sec = st.st_mtimespec.tv_sec;
  i.mtime_ns = st.st_mtimespec.tv_nsec;
  i.ctime_sec = st.st_ctimespec.tv_sec;
  i.ctime_ns = st.st_ctimespec.tv_nsec;
#else
  i.mtime_sec = st.st_mtim.tv_sec;
  i.mtime_ns = st.st_mtim.tv_nsec;
  i.ctime_sec = st.st_ctim.tv_sec;
  i.ctime_ns = st.st_ctim.tv_nsec;
#endif
  return i;
}
bool descriptor_path(int fd, const std::string &expected) {
  char resolved[PATH_MAX];
#ifdef __APPLE__
  return ::fcntl(fd, F_GETPATH, resolved) == 0 && expected == resolved;
#else
  const std::string path = "/proc/self/fd/" + std::to_string(fd);
  return ::realpath(path.c_str(), resolved) != nullptr && expected == resolved;
#endif
}
bool digits(const std::string &s, size_t pos, size_t n) {
  if (pos + n > s.size())
    return false;
  for (size_t i = pos; i < pos + n; ++i)
    if (s[i] < '0' || s[i] > '9')
      return false;
  return true;
}
int number(const std::string &s, size_t pos, size_t n) {
  int v = 0;
  for (size_t i = pos; i < pos + n; ++i) v = v * 10 + s[i] - '0';
  return v;
}
bool date_valid(const std::string &s) {
  if (s.size() < 10 || !digits(s, 0, 4) || !digits(s, 5, 2) || !digits(s, 8, 2) || s[4] != '-' ||
      s[7] != '-')
    return false;
  int y = number(s, 0, 4), m = number(s, 5, 2), d = number(s, 8, 2);
  if (y < 1 || m < 1 || m > 12)
    return false;
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max = days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  return d >= 1 && d <= max;
}
bool datetime_valid(const std::string &s) {
  if (s.size() < 19 || !date_valid(s) || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' ||
      s[16] != ':' || !digits(s, 11, 2) || !digits(s, 14, 2) || !digits(s, 17, 2))
    return false;
  if (number(s, 11, 2) > 23 || number(s, 14, 2) > 59 || number(s, 17, 2) > 59)
    return false;
  return s.size() == 19 ||
         (s.size() >= 21 && s.size() <= 26 && s[19] == '.' && digits(s, 20, s.size() - 20));
}
struct Input {
  int fd = -1;
  std::string path;
  Identity identity;
  char buffer[65536];
  size_t pos = 0, count = 0;
  uint64_t line = 1;
  bool first = true;
  ~Input() {
    if (fd >= 0)
      ::close(fd);
  }
  int get() {
    if (pos == count) {
      ssize_t n;
      do {
        n = ::read(fd, buffer, sizeof(buffer));
      } while (n < 0 && errno == EINTR);
      if (n < 0)
        return -2;
      if (n == 0)
        return -1;
      pos = 0;
      count = static_cast<size_t>(n);
      if (first) {
        first = false;
        if (count >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
            static_cast<unsigned char>(buffer[1]) == 0xBB &&
            static_cast<unsigned char>(buffer[2]) == 0xBF)
          pos = 3;
      }
      if (pos == count)
        return get();
    }
    return static_cast<unsigned char>(buffer[pos++]);
  }
  void unget() {
    if (pos)
      --pos;
  }
  Status rewind(std::string &error) {
    Status status = verify_file(fd, path, identity, error);
    if (status != Status::OK)
      return status;
    if (::lseek(fd, 0, SEEK_SET) < 0)
      return fail(Status::IO, error, "cannot rewind file");
    pos = count = 0;
    line = 1;
    first = true;
    return Status::OK;
  }
};
Status csv_row(Input &input, const Options &options, std::vector<Cell> &row, std::string &error) {
  row.clear();
  Cell cell;
  cell.null = false;
  bool quoted = false, in_quote = false, after_quote = false, started = false;
  size_t bytes = 0;
  const uint64_t line = input.line;
  auto finish = [&]() -> Status {
    if (cell.text.size() > MAX_FIELD_BYTES)
      return fail(Status::LIMIT, error, "CSV field exceeds size limit");
    if (row.size() >= MAX_COLUMNS)
      return fail(Status::LIMIT, error, "too many CSV columns");
    if (!valid_utf8(cell.text))
      return fail(Status::INVALID, error, "invalid UTF-8 at CSV line " + std::to_string(line));
    cell.null = !quoted &&
                (cell.text.empty() || (!options.nullstr.empty() && cell.text == options.nullstr));
    cell.type = cell.null ? Type::NIL : infer_text(cell.text);
    if (quoted && cell.text.empty())
      cell.type = Type::STRING;
    row.push_back(std::move(cell));
    cell = Cell();
    cell.null = false;
    quoted = in_quote = after_quote = started = false;
    return Status::OK;
  };
  for (;;) {
    int ch = input.get();
    if (ch == -2)
      return fail(Status::IO, error, "CSV read failed");
    if (ch == -1) {
      if (in_quote)
        return fail(Status::INVALID, error,
                    "unterminated quote at CSV line " + std::to_string(line));
      if (bytes == 0 && row.empty() && cell.text.empty())
        return Status::END;
      return finish();
    }
    if (++bytes > MAX_ROW_BYTES || cell.text.size() > MAX_FIELD_BYTES)
      return fail(Status::LIMIT, error, "CSV record exceeds size limit");
    if (ch == '\n')
      ++input.line;
    if (in_quote) {
      if (ch == options.escape && options.escape != options.quote) {
        int next = input.get();
        if (next < 0)
          return fail(Status::INVALID, error, "incomplete CSV escape");
        if (next == '\n')
          ++input.line;
        cell.text.push_back(static_cast<char>(next));
        ++bytes;
      } else if (ch == options.quote) {
        if (options.escape == options.quote) {
          int next = input.get();
          if (next == options.quote) {
            cell.text.push_back(options.quote);
            ++bytes;
            continue;
          }
          if (next == -2)
            return fail(Status::IO, error, "CSV read failed");
          if (next >= 0)
            input.unget();
        }
        in_quote = false;
        after_quote = true;
      } else
        cell.text.push_back(static_cast<char>(ch));
    } else if (ch == options.delimiter) {
      Status st = finish();
      if (st != Status::OK)
        return st;
    } else if (ch == '\n' || ch == '\r') {
      if (ch == '\r') {
        int next = input.get();
        if (next == '\n')
          ++input.line;
        else if (next >= 0)
          input.unget();
        else if (next == -2)
          return fail(Status::IO, error, "CSV read failed");
      }
      return finish();
    } else if (!started && !after_quote && ch == options.quote && options.quote != 0) {
      quoted = in_quote = started = true;
    } else if (after_quote || ch == options.quote) {
      return fail(Status::INVALID, error,
                  "unexpected character after/in CSV field at line " + std::to_string(line));
    } else {
      started = true;
      cell.text.push_back(static_cast<char>(ch));
    }
  }
}
Status json_row(Input &input, std::vector<std::pair<std::string, Cell>> &row, std::string &error) {
  row.clear();
  std::string line;
  uint64_t number = input.line;
  for (;;) {
    int ch = input.get();
    if (ch == -2)
      return fail(Status::IO, error, "JSONL read failed");
    if (ch == -1 && line.empty())
      return Status::END;
    if (ch == -1 || ch == '\n') {
      if (ch == '\n')
        ++input.line;
      if (line.find_first_not_of(" \r\t") == std::string::npos) {
        if (ch == -1)
          return Status::END;
        line.clear();
        number = input.line;
        continue;
      }
      break;
    }
    if (line.size() >= MAX_ROW_BYTES)
      return fail(Status::LIMIT, error, "JSONL record exceeds size limit");
    line.push_back(static_cast<char>(ch));
  }
  size_t at = 0;
  std::set<std::string> names;
  auto whitespace = [&]() {
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) ++at;
  };
  auto invalid = [&]() {
    return fail(Status::INVALID, error,
                "invalid flat JSON object or nested value at line " + std::to_string(number) +
                    ", byte " + std::to_string(at));
  };
  auto string = [&](std::string &value) -> bool {
    if (at == line.size() || line[at] != '"')
      return false;
    size_t start = at++;
    bool escaped = false, closed = false;
    while (at < line.size()) {
      char c = line[at++];
      if (!escaped && c == '"') {
        closed = true;
        break;
      }
      if (!escaped && c == '\\')
        escaped = true;
      else
        escaped = false;
    }
    if (!closed)
      return false;
    rapidjson::Document text;
    text.Parse<rapidjson::kParseValidateEncodingFlag>(line.data() + start, at - start);
    if (text.HasParseError() || !text.IsString())
      return false;
    value.assign(text.GetString(), text.GetStringLength());
    return value.size() <= MAX_FIELD_BYTES;
  };
  whitespace();
  if (at == line.size() || line[at++] != '{')
    return invalid();
  whitespace();
  if (at < line.size() && line[at] == '}') {
    ++at;
    whitespace();
    return at == line.size() ? Status::OK : invalid();
  }
  for (;;) {
    whitespace();
    std::string name;
    if (!string(name) || name.empty() || name.size() > 128 ||
        name.find('\0') != std::string::npos || !names.insert(lower(name)).second)
      return invalid();
    if (row.size() >= MAX_COLUMNS)
      return fail(Status::LIMIT, error, "too many JSONL columns");
    whitespace();
    if (at == line.size() || line[at++] != ':')
      return invalid();
    whitespace();
    if (at == line.size())
      return invalid();
    Cell cell;
    if (line[at] == '"') {
      cell.null = false;
      if (!string(cell.text))
        return invalid();
      cell.type = infer_text(cell.text);
      if (cell.type != Type::DATE && cell.type != Type::DATETIME)
        cell.type = Type::STRING;
    } else if (line.compare(at, 4, "null") == 0)
      at += 4;
    else if (line.compare(at, 4, "true") == 0) {
      at += 4;
      cell = {false, Type::BOOL, "true"};
    } else if (line.compare(at, 5, "false") == 0) {
      at += 5;
      cell = {false, Type::BOOL, "false"};
    } else {
      size_t start = at;
      if (line[at] == '-')
        ++at;
      if (at == line.size())
        return invalid();
      if (line[at] == '0')
        ++at;
      else if (line[at] >= '1' && line[at] <= '9') {
        while (at < line.size() && line[at] >= '0' && line[at] <= '9') ++at;
      } else
        return invalid();
      if (at < line.size() && line[at] == '.') {
        ++at;
        size_t digit = at;
        while (at < line.size() && line[at] >= '0' && line[at] <= '9') ++at;
        if (digit == at)
          return invalid();
      }
      if (at < line.size() && (line[at] == 'e' || line[at] == 'E')) {
        ++at;
        if (at < line.size() && (line[at] == '+' || line[at] == '-'))
          ++at;
        size_t digit = at;
        while (at < line.size() && line[at] >= '0' && line[at] <= '9') ++at;
        if (digit == at)
          return invalid();
      }
      if (at - start > MAX_FIELD_BYTES)
        return fail(Status::LIMIT, error, "JSONL number exceeds size limit");
      cell.text = line.substr(start, at - start);
      cell.null = false;
      cell.type = infer_text(cell.text);
    }
    row.push_back({std::move(name), std::move(cell)});
    whitespace();
    if (at == line.size())
      return invalid();
    char separator = line[at++];
    if (separator == '}')
      break;
    if (separator != ',')
      return invalid();
  }
  whitespace();
  if (at != line.size())
    return invalid();
  return Status::OK;
}
Status input_open(Input &input, const std::string &path, const std::string &root,
                  std::string &error) {
  return open_secure(path, root, input.fd, input.path, input.identity, error);
}
Status header(Input &input, const Options &options, std::vector<std::string> &names,
              std::string &error) {
  std::vector<Cell> cells;
  Status st = csv_row(input, options, cells, error);
  if (st == Status::END)
    return fail(Status::INVALID, error, "empty CSV has no schema");
  if (st != Status::OK)
    return st;
  if (!cells.empty() && cells[0].text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    cells[0].text.erase(0, 3);
  std::set<std::string> used;
  for (size_t i = 0; i < cells.size(); ++i) {
    std::string base = options.header ? cells[i].text : "column" + std::to_string(i + 1);
    if (base.empty())
      base = "column" + std::to_string(i + 1);
    if (base.size() > 128 || base.find('\0') != std::string::npos)
      return fail(Status::INVALID, error, "invalid CSV column name");
    std::string name = base;
    int suffix = 2;
    while (!used.insert(lower(name)).second) name = base + "__" + std::to_string(suffix++);
    names.push_back(name);
  }
  if (!options.header)
    return input.rewind(error);
  return Status::OK;
}

Status discover(const std::string &pattern, const std::string &root,
                std::vector<std::string> &files, std::string &error) {
  if (pattern.empty() || pattern[0] != '/' || pattern.find('\0') != std::string::npos ||
      pattern.size() >= PATH_MAX || root.empty() || lower(root) == "null")
    return fail(Status::DENIED, error, "glob requires an authorized absolute path");
  char real[PATH_MAX];
  if (!::realpath(root.c_str(), real))
    return fail(Status::DENIED, error, "invalid secure_file_priv");
  std::string base = real;
  if (!within(pattern, base))
    return fail(Status::DENIED, error, "glob is outside secure_file_priv");
  std::vector<std::string> parts;
  size_t at = base == "/" ? 1 : base.size() + 1;
  while (at < pattern.size()) {
    size_t end = pattern.find('/', at);
    std::string part = pattern.substr(at, end == std::string::npos ? end : end - at);
    if (part.empty() || part == "." || part == "..")
      return fail(Status::DENIED, error, "invalid glob path component");
    parts.push_back(part);
    if (end == std::string::npos)
      break;
    at = end + 1;
  }
  if (parts.empty() || parts.size() > 64)
    return fail(Status::LIMIT, error, "invalid glob depth");
  size_t visited = 0;
  std::function<Status(const std::string &, size_t)> walk;
  walk = [&](const std::string &dir, size_t level) -> Status {
    if (++visited > 10000)
      return fail(Status::LIMIT, error, "glob directory limit exceeded");
    int dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0)
      return fail(Status::DENIED, error, "cannot open glob directory");
    DIR *stream = ::fdopendir(dirfd);
    if (!stream) {
      ::close(dirfd);
      return fail(Status::IO, error, "cannot scan glob directory");
    }
    Status status = Status::OK;
    struct dirent *entry;
    while (status == Status::OK && (entry = ::readdir(stream)) != nullptr) {
      std::string name = entry->d_name;
      if (name == "." || name == ".." ||
          ::fnmatch(parts[level].c_str(), name.c_str(), FNM_PERIOD) != 0)
        continue;
      struct stat info;
      if (::fstatat(dirfd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        status = fail(Status::IO, error, "glob file changed");
        break;
      }
      std::string path = dir + (dir == "/" ? "" : "/") + name;
      if (level + 1 < parts.size()) {
        if (S_ISDIR(info.st_mode))
          status = walk(path, level + 1);
      } else if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) {
        int fd = -1;
        std::string canonical;
        Identity identity;
        status = open_secure(path, root, fd, canonical, identity, error);
        if (fd >= 0)
          ::close(fd);
        if (status == Status::OK) {
          files.push_back(path);
          if (files.size() > 10000)
            status = fail(Status::LIMIT, error, "glob match limit exceeded");
        }
      }
    }
    ::closedir(stream);
    return status;
  };
  Status status = walk(base, 0);
  std::sort(files.begin(), files.end());
  return status;
}
}  // namespace
bool Identity::operator==(const Identity &v) const {
  return device == v.device && inode == v.inode && size == v.size && mtime_sec == v.mtime_sec &&
         mtime_ns == v.mtime_ns && ctime_sec == v.ctime_sec && ctime_ns == v.ctime_ns;
}
bool valid_utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i++];
    if (c < 0x80)
      continue;
    int n = 0;
    uint32_t cp = 0, min = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      n = 1;
      cp = c & 31;
      min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
      n = 2;
      cp = c & 15;
      min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
      n = 3;
      cp = c & 7;
      min = 0x10000;
    } else
      return false;
    if (i + n > s.size())
      return false;
    while (n--) {
      unsigned char d = s[i++];
      if ((d & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (d & 63);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
  }
  return true;
}
Type infer_text(const std::string &s) {
  if (lower(s) == "true" || lower(s) == "false")
    return Type::BOOL;
  if (s.size() == 10 && date_valid(s))
    return Type::DATE;
  if (datetime_valid(s))
    return Type::DATETIME;
  size_t start = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
  if (start < s.size() && digits(s, start, s.size() - start)) {
    char *end = nullptr;
    errno = 0;
    std::strtoll(s.c_str(), &end, 10);
    return errno == ERANGE ? Type::STRING : Type::INT;
  }
  if (!s.empty() && s.find_first_not_of("+-0123456789.eE") == std::string::npos) {
    char *end = nullptr;
    errno = 0;
    double d = std::strtod(s.c_str(), &end);
    if (end == s.c_str() + s.size() && end != s.c_str() && errno != ERANGE && std::isfinite(d))
      return Type::DOUBLE;
  }
  return Type::STRING;
}
Type widen(Type a, Type b) {
  if (a == b || b == Type::NIL)
    return a;
  if (a == Type::NIL)
    return b;
  if ((a == Type::INT && b == Type::DOUBLE) || (a == Type::DOUBLE && b == Type::INT))
    return Type::DOUBLE;
  if ((a == Type::DATE && b == Type::DATETIME) || (a == Type::DATETIME && b == Type::DATE))
    return Type::DATETIME;
  return Type::STRING;
}
const char *type_name(Type t) {
  static const char *names[] = {"NULL",    "BIGINT", "DOUBLE",  "BOOLEAN",
                                "VARCHAR", "DATE",   "DATETIME"};
  return names[static_cast<int>(t)];
}
Status open_secure(const std::string &path, const std::string &root, int &fd,
                   std::string &canonical, Identity &identity, std::string &error) {
  fd = -1;
  if (path.empty() || path.size() >= PATH_MAX || path.find('\0') != std::string::npos ||
      root.empty() || root.find('\0') != std::string::npos || lower(root) == "null")
    return fail(Status::DENIED, error, "file access denied by secure_file_priv");
  char root_buf[PATH_MAX], file_buf[PATH_MAX];
  if (!::realpath(root.c_str(), root_buf) || !::realpath(path.c_str(), file_buf))
    return fail(Status::DENIED, error, "cannot resolve authorized local file");
  std::string base = root_buf;
  canonical = file_buf;
  if (!within(canonical, base))
    return fail(Status::DENIED, error, "file is outside secure_file_priv");
  int current = ::open(base.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (current < 0)
    return fail(Status::DENIED, error, "cannot open secure_file_priv directory");
  if (!descriptor_path(current, base)) {
    ::close(current);
    return fail(Status::DENIED, error, "secure_file_priv directory changed");
  }
  std::string rel = canonical.substr(base == "/" ? 1 : base.size() + 1);
  size_t pos = 0;
  for (;;) {
    size_t slash = rel.find('/', pos);
    std::string part = rel.substr(pos, slash == std::string::npos ? slash : slash - pos);
    if (part.empty() || part == "." || part == "..") {
      ::close(current);
      return fail(Status::DENIED, error, "invalid path component");
    }
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (slash != std::string::npos)
      flags |= O_DIRECTORY;
    int next = ::openat(current, part.c_str(), flags);
    ::close(current);
    current = next;
    if (current < 0)
      return fail(Status::DENIED, error, "cannot open local file without following symlinks");
    if (slash == std::string::npos)
      break;
    pos = slash + 1;
  }
  struct stat st;
  if (::fstat(current, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(current);
    return fail(Status::DENIED, error, "input must be a regular file");
  }
  if (!descriptor_path(current, canonical)) {
    ::close(current);
    return fail(Status::DENIED, error, "file path changed while opening");
  }
  fd = current;
  identity = identity_of(st);
  Status status = verify_file(fd, canonical, identity, error);
  if (status != Status::OK) {
    ::close(fd);
    fd = -1;
  }
  return status;
}
Status verify_file(int fd, const std::string &path, const Identity &expected, std::string &error) {
  struct stat opened, named;
  if (::fstat(fd, &opened) != 0 || ::stat(path.c_str(), &named) != 0 ||
      !(identity_of(opened) == expected) || !(identity_of(named) == expected))
    return fail(Status::CHANGED, error, "file changed since schema inference: " + path);
  return Status::OK;
}
Status infer(const std::string &path, const Options &options, const std::string &root,
             Schema &schema, std::string &error) {
  if (options.format == Format::GLOB) {
    std::vector<std::string> files;
    Status st = discover(path, root, files, error);
    if (st != Status::OK)
      return st;
    schema = Schema();
    schema.path = path;
    schema.options = options;
    schema.columns.push_back({"file", Type::STRING, false});
    schema.rows = files.size();
    return Status::OK;
  }
  if (options.format == Format::CSV &&
      (options.quote == '\n' || options.quote == '\r' || options.escape == '\n' ||
       options.escape == '\r' || options.nullstr.size() > 4096))
    return fail(Status::INVALID, error, "invalid CSV quoting or NULL text option");
  Input input;
  Status st = input_open(input, path, root, error);
  if (st != Status::OK)
    return st;
  schema = Schema();
  schema.path = input.path;
  schema.identity = input.identity;
  schema.options = options;
  if (options.format == Format::PARQUET) {
    ParquetReader reader;
    st = reader.open(input.fd, schema, {}, error);
    return st == Status::OK ? verify_file(input.fd, input.path, input.identity, error) : st;
  }
  if (options.delimiter == 0 || options.delimiter == '\n' || options.delimiter == '\r' ||
      options.delimiter == options.quote)
    return fail(Status::INVALID, error, "invalid CSV delimiter");
  if (options.format == Format::CSV) {
    std::vector<std::string> names;
    st = header(input, options, names, error);
    if (st != Status::OK)
      return st;
    for (auto &n : names) schema.columns.push_back({n, Type::NIL, false});
    std::vector<Cell> cells;
    while ((st = csv_row(input, options, cells, error)) == Status::OK) {
      if (cells.size() != schema.columns.size())
        return fail(Status::INVALID, error,
                    "CSV field count mismatch at line " + std::to_string(input.line));
      for (size_t i = 0; i < cells.size(); ++i) {
        schema.columns[i].type = widen(schema.columns[i].type, cells[i].type);
        schema.columns[i].nullable |= cells[i].null;
        if (cells[i].type == Type::DATETIME && cells[i].text.size() > 20)
          schema.columns[i].scale =
              std::max(schema.columns[i].scale, static_cast<int>(cells[i].text.size() - 20));
      }
      ++schema.rows;
    }
  } else {
    std::map<std::string, size_t> names;
    std::vector<std::pair<std::string, Cell>> cells;
    while ((st = json_row(input, cells, error)) == Status::OK) {
      std::set<size_t> seen;
      for (auto &c : cells) {
        auto it = names.find(lower(c.first));
        size_t i;
        if (it == names.end()) {
          if (names.size() >= MAX_COLUMNS)
            return fail(Status::LIMIT, error, "too many inferred columns");
          i = schema.columns.size();
          names[lower(c.first)] = i;
          schema.columns.push_back({c.first, Type::NIL, schema.rows != 0});
        } else
          i = it->second;
        seen.insert(i);
        schema.columns[i].type = widen(schema.columns[i].type, c.second.type);
        schema.columns[i].nullable |= c.second.null;
        if (c.second.type == Type::DATETIME && c.second.text.size() > 20)
          schema.columns[i].scale =
              std::max(schema.columns[i].scale, static_cast<int>(c.second.text.size() - 20));
      }
      for (size_t i = 0; i < schema.columns.size(); ++i)
        if (!seen.count(i))
          schema.columns[i].nullable = true;
      ++schema.rows;
    }
  }
  if (st != Status::END)
    return st;
  if (schema.columns.empty())
    return fail(Status::INVALID, error, "file has no columns");
  if (schema.rows == 0)
    for (auto &col : schema.columns) col.nullable = true;
  return verify_file(input.fd, input.path, input.identity, error);
}
struct Reader::Impl {
  Input input;
  Schema schema;
  std::vector<size_t> projection;
  std::vector<std::string> files;
  size_t file_index = 0;
  std::unique_ptr<ParquetReader> parquet;
};
Reader::Reader() : impl_(new Impl) {}
Reader::~Reader() = default;
void Reader::close() { impl_.reset(new Impl); }
Status Reader::open(const Schema &schema, const std::string &root, std::string &error,
                    const std::vector<size_t> &projection) {
  close();
  impl_->schema = schema;
  impl_->projection = projection;
  if (schema.options.format == Format::GLOB)
    return discover(schema.path, root, impl_->files, error);
  Status st = input_open(impl_->input, schema.path, root, error);
  if (st != Status::OK)
    return st;
  if (!(impl_->input.identity == schema.identity))
    return fail(Status::CHANGED, error, "file changed before scan");
  if (schema.options.format == Format::PARQUET) {
    impl_->parquet.reset(new ParquetReader);
    return impl_->parquet->open(impl_->input.fd, impl_->schema, projection, error);
  }
  if (schema.options.format == Format::CSV && schema.options.header) {
    std::vector<std::string> n;
    return header(impl_->input, schema.options, n, error);
  }
  return Status::OK;
}
Status Reader::rewind(std::string &error) {
  if (impl_->schema.options.format == Format::GLOB) {
    impl_->file_index = 0;
    return Status::OK;
  }
  Status st = impl_->input.rewind(error);
  if (st != Status::OK)
    return st;
  if (impl_->parquet) {
    impl_->parquet->rewind();
    return Status::OK;
  }
  if (impl_->schema.options.format == Format::CSV && impl_->schema.options.header) {
    std::vector<std::string> n;
    return header(impl_->input, impl_->schema.options, n, error);
  }
  return Status::OK;
}
Status Reader::next(std::vector<Cell> &row, std::string &error) {
  if (impl_->schema.options.format == Format::GLOB) {
    if (impl_->file_index == impl_->files.size())
      return Status::END;
    row.assign(1, Cell{false, Type::STRING, impl_->files[impl_->file_index++]});
    return Status::OK;
  }
  Status st = verify_file(impl_->input.fd, impl_->input.path, impl_->schema.identity, error);
  if (st != Status::OK)
    return st;
  if (impl_->parquet) {
    st = impl_->parquet->next(row, error);
    if (st == Status::OK || st == Status::END) {
      Status checked =
          verify_file(impl_->input.fd, impl_->input.path, impl_->schema.identity, error);
      if (checked != Status::OK)
        return checked;
    }
    return st;
  }
  if (impl_->schema.options.format == Format::CSV) {
    st = csv_row(impl_->input, impl_->schema.options, row, error);
    if (st == Status::OK && row.size() != impl_->schema.columns.size())
      return fail(Status::INVALID, error, "CSV row width changed");
  } else if (impl_->schema.options.format == Format::JSONL) {
    std::vector<std::pair<std::string, Cell>> cells;
    st = json_row(impl_->input, cells, error);
    row.assign(impl_->schema.columns.size(), Cell());
    for (auto &c : cells) {
      size_t i = 0;
      while (i < impl_->schema.columns.size() &&
             lower(impl_->schema.columns[i].name) != lower(c.first))
        ++i;
      if (i == row.size())
        return fail(Status::CHANGED, error, "JSONL schema changed");
      row[i] = std::move(c.second);
    }
  } else
    return fail(Status::UNSUPPORTED, error, "unsupported scan format");
  if (st == Status::END)
    return verify_file(impl_->input.fd, impl_->input.path, impl_->schema.identity, error) ==
                   Status::OK
               ? Status::END
               : Status::CHANGED;
  if (st == Status::OK)
    return verify_file(impl_->input.fd, impl_->input.path, impl_->schema.identity, error);
  return st;
}
bool is_descriptor(const std::string &s) { return s.compare(0, MAGIC.size(), MAGIC) == 0; }
std::string encode(const Schema &s) {
  rapidjson::StringBuffer b;
  rapidjson::Writer<rapidjson::StringBuffer> w(b);
  w.StartObject();
  w.Key("path");
  w.String(s.path.data(), s.path.size());
  w.Key("format");
  w.Int(static_cast<int>(s.options.format));
  w.Key("header");
  w.Bool(s.options.header);
  w.Key("delimiter");
  w.Int(static_cast<unsigned char>(s.options.delimiter));
  w.Key("quote");
  w.Int(static_cast<unsigned char>(s.options.quote));
  w.Key("escape");
  w.Int(static_cast<unsigned char>(s.options.escape));
  w.Key("nullstr");
  w.String(s.options.nullstr.data(), s.options.nullstr.size());
  w.Key("identity");
  w.StartArray();
  w.Uint64(s.identity.device);
  w.Uint64(s.identity.inode);
  w.Int64(s.identity.size);
  w.Int64(s.identity.mtime_sec);
  w.Int64(s.identity.mtime_ns);
  w.Int64(s.identity.ctime_sec);
  w.Int64(s.identity.ctime_ns);
  w.EndArray();
  w.Key("rows");
  w.Uint64(s.rows);
  w.Key("columns");
  w.StartArray();
  for (auto &c : s.columns) {
    w.StartArray();
    w.String(c.name.data(), c.name.size());
    w.Int(static_cast<int>(c.type));
    w.Bool(c.nullable);
    w.Int(c.scale);
    w.EndArray();
  }
  w.EndArray();
  w.EndObject();
  return MAGIC + std::string(b.GetString(), b.GetSize());
}
Status decode(const std::string &s, Schema &out, std::string &error) {
  if (!is_descriptor(s) || s.size() > MAX_SCHEMA_BYTES)
    return fail(Status::INVALID, error, "invalid file scan descriptor");
  rapidjson::Document d;
  d.Parse(s.data() + MAGIC.size(), s.size() - MAGIC.size());
  if (d.HasParseError() || !d.IsObject())
    return fail(Status::INVALID, error, "invalid file scan metadata");
  for (auto name : {"path", "format", "header", "delimiter", "quote", "escape", "nullstr",
                    "identity", "rows", "columns"})
    if (!d.HasMember(name))
      return fail(Status::INVALID, error, "incomplete file scan metadata");
  if (!d["path"].IsString() || !d["format"].IsInt() || d["format"].GetInt() < 0 ||
      d["format"].GetInt() > 3 || !d["header"].IsBool() || !d["delimiter"].IsInt() ||
      !d["quote"].IsInt() || !d["escape"].IsInt() || !d["nullstr"].IsString() ||
      !d["identity"].IsArray() || d["identity"].Size() != 7 || !d["rows"].IsUint64() ||
      !d["columns"].IsArray() || d["columns"].Size() > MAX_COLUMNS)
    return fail(Status::INVALID, error, "malformed file scan metadata");
  for (size_t i = 0; i < 7; ++i)
    if (i < 2 ? !d["identity"][i].IsUint64() : !d["identity"][i].IsInt64())
      return fail(Status::INVALID, error, "invalid file identity");
  for (auto name : {"delimiter", "quote", "escape"})
    if (d[name].GetInt() < 0 || d[name].GetInt() > 255)
      return fail(Status::INVALID, error, "invalid delimiter metadata");
  out = Schema();
  out.path.assign(d["path"].GetString(), d["path"].GetStringLength());
  out.options.format = static_cast<Format>(d["format"].GetInt());
  out.options.header = d["header"].GetBool();
  out.options.delimiter = d["delimiter"].GetInt();
  out.options.quote = d["quote"].GetInt();
  out.options.escape = d["escape"].GetInt();
  out.options.nullstr.assign(d["nullstr"].GetString(), d["nullstr"].GetStringLength());
  auto &id = d["identity"];
  out.identity.device = id[0].GetUint64();
  out.identity.inode = id[1].GetUint64();
  out.identity.size = id[2].GetInt64();
  out.identity.mtime_sec = id[3].GetInt64();
  out.identity.mtime_ns = id[4].GetInt64();
  out.identity.ctime_sec = id[5].GetInt64();
  out.identity.ctime_ns = id[6].GetInt64();
  out.rows = d["rows"].GetUint64();
  for (auto &v : d["columns"].GetArray()) {
    if (!v.IsArray() || (v.Size() != 3 && v.Size() != 4) || !v[0].IsString() || !v[1].IsInt() ||
        v[1].GetInt() < 0 || v[1].GetInt() > 6 || !v[2].IsBool())
      return fail(Status::INVALID, error, "invalid column metadata");
    out.columns.push_back({std::string(v[0].GetString(), v[0].GetStringLength()),
                           static_cast<Type>(v[1].GetInt()), v[2].GetBool()});
    if (v.Size() == 4) {
      if (!v[3].IsInt() || v[3].GetInt() < 0 || v[3].GetInt() > 6)
        return fail(Status::INVALID, error, "invalid datetime precision");
      out.columns.back().scale = v[3].GetInt();
    }
  }
  return Status::OK;
}

struct CsvWriter::Impl {
  int fd = -1, dir = -1;
  std::string parent, base, temp, root;
  Options options;
  size_t columns = 0;
  ~Impl() {
    if (fd >= 0)
      ::close(fd);
    if (dir >= 0) {
      if (!temp.empty())
        ::unlinkat(dir, temp.c_str(), 0);
      ::close(dir);
    }
  }
  bool authorized() {
    return descriptor_path(dir, parent) && (parent == root || within(parent, root));
  }
};
CsvWriter::CsvWriter() : impl_(new Impl) {}
CsvWriter::~CsvWriter() = default;
Status CsvWriter::open(const Schema &schema, const std::string &root, std::string &error) {
  impl_.reset(new Impl);
  auto &s = *impl_;
  s.options = schema.options;
  s.columns = schema.columns.size();
  if (schema.path.empty() || schema.path[0] != '/' || schema.path.find('\0') != std::string::npos ||
      root.empty() || root.find('\0') != std::string::npos || lower(root) == "null")
    return fail(Status::DENIED, error, "COPY path denied by secure_file_priv");
  if (s.columns == 0 || s.columns > MAX_COLUMNS || s.options.delimiter == 0 ||
      s.options.delimiter == s.options.quote || s.options.delimiter == '\r' ||
      s.options.delimiter == '\n' || s.options.quote == 0 || s.options.escape == 0)
    return fail(Status::INVALID, error, "invalid COPY options");
  if (s.options.nullstr.size() > 4096 || !valid_utf8(s.options.nullstr) ||
      s.options.nullstr.find_first_of(std::string() + s.options.delimiter + s.options.quote + '\r' + '\n') != std::string::npos ||
      s.options.quote == '\r' || s.options.quote == '\n' ||
      s.options.escape == '\r' || s.options.escape == '\n')
    return fail(Status::INVALID, error, "COPY options cannot represent NULL unambiguously");
  size_t slash = schema.path.rfind('/');
  s.base = schema.path.substr(slash + 1);
  if (s.base.empty() || s.base == "." || s.base == "..")
    return fail(Status::DENIED, error, "invalid COPY destination");
  char buf[PATH_MAX];
  if (!::realpath(root.c_str(), buf))
    return fail(Status::DENIED, error, "invalid secure_file_priv");
  s.root = buf;
  if (!::realpath(schema.path.substr(0, slash == 0 ? 1 : slash).c_str(), buf))
    return fail(Status::DENIED, error, "invalid COPY directory");
  s.parent = buf;
  if (s.parent != s.root && !within(s.parent, s.root))
    return fail(Status::DENIED, error, "COPY outside secure_file_priv");
  s.dir = ::open(s.parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (s.dir < 0 || !s.authorized())
    return fail(Status::DENIED, error, "COPY directory changed");
  struct stat st;
  if (::fstatat(s.dir, s.base.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
    return fail(Status::DENIED, error, "COPY destination already exists");
  for (int attempt = 0; attempt < 32; ++attempt) {
    uint64_t nonce = 0;
    int random = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random < 0)
      return fail(Status::IO, error, "cannot create COPY temporary name");
    ssize_t got = ::read(random, &nonce, sizeof(nonce));
    ::close(random);
    if (got != sizeof(nonce))
      return fail(Status::IO, error, "cannot create COPY temporary name");
    s.temp = ".seekdb-copy-" + std::to_string(nonce) + ".tmp";
    s.fd =
        ::openat(s.dir, s.temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s.fd >= 0)
      break;
    if (errno != EEXIST) {
      s.temp.clear();
      return fail(Status::IO, error, "cannot create COPY temporary file");
    }
    s.temp.clear();
  }
  if (s.fd < 0)
    return fail(Status::IO, error, "cannot allocate COPY temporary file");
  if (s.options.header) {
    std::vector<Cell> row;
    for (auto &column : schema.columns) row.push_back(Cell{false, Type::STRING, column.name});
    return write(row, error);
  }
  return Status::OK;
}
Status CsvWriter::write(const std::vector<Cell> &row, std::string &error) {
  auto &s = *impl_;
  if (s.fd < 0 || row.size() != s.columns)
    return fail(Status::INVALID, error, "invalid COPY row");
  if (!s.authorized())
    return fail(Status::DENIED, error, "COPY directory moved");
  std::string record;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i)
      record.push_back(s.options.delimiter);
    const auto &cell = row[i];
    if (cell.null)
      record += s.options.nullstr;
    else {
      if (cell.text.size() > MAX_FIELD_BYTES || !valid_utf8(cell.text))
        return fail(Status::LIMIT, error, "invalid/oversized COPY field");
      bool quote = cell.text.empty() || cell.text == s.options.nullstr ||
                   cell.text.find_first_of(std::string() + s.options.delimiter + s.options.quote +
                                           s.options.escape + '\r' + '\n') != std::string::npos;
      if (quote)
        record.push_back(s.options.quote);
      for (char c : cell.text) {
        if (quote && (c == s.options.quote || c == s.options.escape))
          record.push_back(s.options.escape);
        record.push_back(c);
      }
      if (quote)
        record.push_back(s.options.quote);
    }
    if (record.size() > MAX_ROW_BYTES)
      return fail(Status::LIMIT, error, "COPY record exceeds limit");
  }
  record.push_back('\n');
  size_t written = 0;
  while (written < record.size()) {
    ssize_t n = ::write(s.fd, record.data() + written, record.size() - written);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return fail(Status::IO, error, "COPY write failed");
    written += n;
  }
  return Status::OK;
}
Status CsvWriter::commit(std::string &error) {
  auto &s = *impl_;
  if (s.fd < 0 || !s.authorized())
    return fail(Status::DENIED, error, "COPY directory changed");
  if (::fsync(s.fd) != 0)
    return fail(Status::IO, error, "COPY sync failed");
  // linkat publishes atomically without ever replacing an existing destination.
  if (::linkat(s.dir, s.temp.c_str(), s.dir, s.base.c_str(), 0) != 0)
    return fail(Status::IO, error, "COPY destination exists or publish failed");
  ::unlinkat(s.dir, s.temp.c_str(), 0);
  s.temp.clear();
  ::close(s.fd);
  s.fd = -1;
  return Status::OK;
}
}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
