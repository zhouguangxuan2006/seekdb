// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <string>
namespace oceanbase { namespace sql {
class ObSQLSessionInfo;
// Capability belongs to a specific internal connection, never to a thread or
// SQL-visible variable. The scope ends before the connection returns to its pool.
class AiColumnInternalAccess {
public:
  explicit AiColumnInternalAccess(const ObSQLSessionInfo *session) : session_(session) {
    std::lock_guard<std::mutex> guard(mutex_); ++sessions_[session_];
  }
  ~AiColumnInternalAccess() {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = sessions_.find(session_);
    if (it != sessions_.end() && --it->second == 0) sessions_.erase(it);
  }
  static bool contains(const ObSQLSessionInfo *session) {
    std::lock_guard<std::mutex> guard(mutex_); return sessions_.count(session) != 0;
  }
  AiColumnInternalAccess(const AiColumnInternalAccess &) = delete;
  AiColumnInternalAccess &operator=(const AiColumnInternalAccess &) = delete;
private:
  const ObSQLSessionInfo *session_;
  inline static std::mutex mutex_;
  inline static std::unordered_map<const ObSQLSessionInfo *, unsigned> sessions_;
};
template<class Schema> inline bool ai_column_has_definition(const Schema &schema) {
  for (int64_t i = 0; i < schema.get_column_count(); ++i) {
    const auto *column = schema.get_column_schema_by_idx(i);
    if (column != nullptr && column->is_ai_column()) return true;
  }
  return false;
}
inline std::string ai_column_identifier(const std::string &value) {
  std::string out = "`";
  for (char c : value) { out += c; if (c == '`') out += c; }
  return out + "`";
}
inline std::string ai_column_literal(const std::string &value) {
  // Hex literals do not depend on NO_BACKSLASH_ESCAPES or connection encoding.
  static const char hex[] = "0123456789abcdef";
  std::string out = "CONVERT(X'";
  for (unsigned char c : value) { out += hex[c >> 4]; out += hex[c & 15]; }
  return out + "' USING utf8mb4)";
}
// The definition has already passed the native parser and root-function check.
// Replace only the outer call, leaving argument SQL (including SQL literals)
// untouched. Native JSON_ARRAY evaluates and captures inputs without inference.
inline bool ai_column_capture_expression(const std::string &sql, std::string &out) {
  size_t i = 0;
  for (;;) {
    while (i < sql.size() && (sql[i] == '(' || static_cast<unsigned char>(sql[i]) <= 32)) ++i;
    if (sql.compare(i, 2, "/*") == 0) {
      auto end = sql.find("*/", i + 2); if (end == std::string::npos) return false;
      i = end + 2;
    } else if (i < sql.size() && (sql[i] == '#' ||
        (sql.compare(i, 2, "--") == 0 && i+2 < sql.size() && static_cast<unsigned char>(sql[i+2]) <= 32))) {
      auto end = sql.find('\n', i); if (end == std::string::npos) return false; i = end + 1;
    } else break;
  }
  size_t begin = i;
  std::string name;
  if (i < sql.size() && sql[i] == '`') {
    ++i;
    while (i < sql.size() && sql[i] != '`') name += sql[i++];
    if (i == sql.size()) return false; ++i;
  } else {
    while (i < sql.size() && ((sql[i] >= 'A' && sql[i] <= 'Z') ||
        (sql[i] >= 'a' && sql[i] <= 'z') || sql[i] == '_')) name += sql[i++];
  }
  for (char &c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  if (name != "ai_complete" && name != "ai_embed") return false;
  out = sql.substr(0, begin) + "JSON_ARRAY" + sql.substr(i);
  return true;
}
}} // namespace oceanbase::sql
