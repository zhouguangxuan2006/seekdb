/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
#include <iostream>

#include "sql/engine/basic/ob_file_scan_utils.h"
using namespace oceanbase::sql::file_sql;
int main(int argc, char **argv) {
  if (argc < 4)
    return 2;
  Options options;
  options.format = std::string(argv[1]) == "parquet" ? Format::PARQUET
                   : std::string(argv[1]) == "json"  ? Format::JSONL
                                                     : Format::CSV;
  Schema schema;
  std::string error;
  auto status = infer(argv[2], options, argv[3], schema, error);
  if (status != Status::OK) {
    std::cerr << error;
    return 1;
  }
  Reader reader;
  std::vector<size_t> projection;
  for (size_t i = 0; i < schema.columns.size(); ++i) projection.push_back(i);
  if (reader.open(schema, argv[3], error, projection) != Status::OK) {
    std::cerr << error;
    return 1;
  }
  std::vector<Cell> row;
  const char *hex = "0123456789abcdef";
  while ((status = reader.next(row, error)) == Status::OK) {
    for (size_t i = 0; i < row.size(); ++i) {
      if (i)
        std::cout << '\t';
      if (row[i].null)
        std::cout << "NULL";
      else
        for (unsigned char c : row[i].text) std::cout << hex[c >> 4] << hex[c & 15];
    }
    std::cout << '\n';
  }
  if (status != Status::END) {
    std::cerr << error;
    return 1;
  }
  return 0;
}
