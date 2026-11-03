/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {
enum class Status { OK, END, INVALID, DENIED, CHANGED, UNSUPPORTED, LIMIT, IO };
enum class Type { NIL, INT, DOUBLE, BOOL, STRING, DATE, DATETIME };
enum class Format { CSV, JSONL, PARQUET, GLOB };
struct Options {
  Format format = Format::CSV;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr;
};
struct Cell {
  bool null = true;
  Type type = Type::NIL;
  std::string text;
};
struct Column {
  std::string name;
  Type type = Type::NIL;
  bool nullable = false;
  int scale = 0;
};
struct Identity {
  uint64_t device = 0, inode = 0;
  int64_t size = 0, mtime_sec = 0, mtime_ns = 0, ctime_sec = 0, ctime_ns = 0;
  bool operator==(const Identity &other) const;
};
struct Schema {
  std::string path;
  Options options;
  Identity identity;
  std::vector<Column> columns;
  uint64_t rows = 0;
};
constexpr size_t MAX_COLUMNS = 256;
constexpr size_t MAX_FIELD_BYTES = 1024 * 1024;
constexpr size_t MAX_ROW_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_SCHEMA_BYTES = 1024 * 1024;

class Reader {
 public:
  Reader();
  ~Reader();
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  Status open(const Schema &schema, const std::string &secure_root, std::string &error,
              const std::vector<size_t> &projection = {});
  Status next(std::vector<Cell> &row, std::string &error);
  Status rewind(std::string &error);
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class CsvWriter {
 public:
  CsvWriter();
  ~CsvWriter();
  Status open(const Schema &schema, const std::string &root, std::string &error);
  Status write(const std::vector<Cell> &row, std::string &error);
  Status commit(std::string &error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
Status infer(const std::string &path, const Options &options, const std::string &secure_root,
             Schema &schema, std::string &error);
std::string encode(const Schema &schema);
Status decode(const std::string &value, Schema &schema, std::string &error);
bool is_descriptor(const std::string &value);
Type infer_text(const std::string &value);
Type widen(Type left, Type right);
const char *type_name(Type type);
bool valid_utf8(const std::string &text);
// Open a regular file beneath secure_file_priv, resolving then walking each component
// with openat/O_NOFOLLOW. The caller owns the returned descriptor.
Status open_secure(const std::string &path, const std::string &root, int &fd,
                   std::string &canonical, Identity &identity, std::string &error);
Status verify_file(int fd, const std::string &canonical, const Identity &identity,
                   std::string &error);
}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
