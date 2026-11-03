/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "ob_file_scan_utils.h"
namespace oceanbase {
namespace sql {
namespace file_sql {
class ParquetReader {
 public:
  ParquetReader();
  ~ParquetReader();
  Status open(int fd, Schema &schema, const std::vector<size_t> &projection, std::string &error);
  Status next(std::vector<Cell> &row, std::string &error);
  void rewind();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
