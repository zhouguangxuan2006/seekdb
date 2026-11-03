/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
#include <unistd.h>

#include <cassert>
#include <fstream>
#include <iostream>

#include "sql/engine/basic/ob_file_scan_utils.h"
using namespace oceanbase::sql::file_sql;
int main(int argc, char **argv) {
  assert(argc == 2);
  std::string root = argv[1], error;
  Options options;
  Schema schema;
  Reader reader;
  std::vector<Cell> row;
  auto expect = [&](Status status) {
    if (status != Status::OK) {
      std::cerr << error << "\n";
      std::abort();
    }
  };
  expect(infer(root + "/basic.csv", options, root, schema, error));
  assert(schema.rows == 3 && schema.columns.size() == 8);
  assert(schema.columns[0].type == Type::INT && schema.columns[2].type == Type::DOUBLE &&
         schema.columns[3].type == Type::BOOL);
  assert(schema.columns[4].type == Type::DATE && schema.columns[4].nullable &&
         schema.columns[5].type == Type::DATETIME);
  assert(schema.columns[7].name == "id__2");
  Schema decoded;
  expect(decode(encode(schema), decoded, error));
  assert(decoded.identity == schema.identity);
  expect(reader.open(schema, root, error));
  expect(reader.next(row, error));
  assert(row[6].text == "hello,\nworld");
  expect(reader.next(row, error));
  assert(row[4].null);
  expect(reader.next(row, error));
  assert(row[6].text == "say \"hi\"");
  assert(reader.next(row, error) == Status::END);
  expect(reader.rewind(error));
  expect(reader.next(row, error));
  assert(row[0].text == "1");
  reader.close();
  options.format = Format::JSONL;
  expect(infer(root + "/basic.jsonl", options, root, schema, error));
  assert(schema.columns.size() == 6 && schema.rows == 3);
  expect(reader.open(schema, root, error));
  expect(reader.next(row, error));
  assert(row[4].null);
  reader.close();
  options.format = Format::PARQUET;
  expect(infer(root + "/simple.parquet", options, root, schema, error));
  assert(schema.rows == 7 && schema.columns.size() == 1);
  expect(reader.open(schema, root, error, {0}));
  int64_t sum = 0;
  for (int i = 0; i < 7; ++i) {
    expect(reader.next(row, error));
    sum += std::stoll(row[0].text);
  }
  assert(sum == 21 && reader.next(row, error) == Status::END);
  reader.close();
  options.format = Format::GLOB;
  expect(infer(root + "/*.csv", options, root, schema, error));
  expect(reader.open(schema, root, error));
  expect(reader.next(row, error));
  assert(row[0].text == root + "/basic.csv");
  reader.close();
  assert(infer_text("9223372036854775807") == Type::INT);
  assert(infer_text("9223372036854775808") == Type::STRING);
  assert(infer_text("-9223372036854775808") == Type::INT);
  assert(infer_text("2024-02-29") == Type::DATE);
  assert(infer_text("2023-02-29") == Type::STRING);
  assert(infer_text("2026-01-01 24:00:00") == Type::STRING);
  assert(widen(Type::INT, Type::DOUBLE) == Type::DOUBLE &&
         widen(Type::BOOL, Type::INT) == Type::STRING);
  options.format = Format::CSV;
  assert(infer("/etc/passwd", options, root, schema, error) == Status::DENIED);
  assert(infer(root + "/basic.csv", options, "", schema, error) == Status::DENIED);
  std::string link = root + "/escape.csv";
  assert(::symlink("/etc/passwd", link.c_str()) == 0);
  assert(infer(link, options, root, schema, error) == Status::DENIED);
  ::unlink(link.c_str());
  std::string changed = root + "/changed.csv";
  {
    std::ofstream f(changed);
    f << "id\n1\n";
  }
  expect(infer(changed, options, root, schema, error));
  expect(reader.open(schema, root, error));
  {
    std::ofstream f(changed, std::ios::app);
    f << "2\n";
  }
  assert(reader.next(row, error) == Status::CHANGED);
  reader.close();
  ::unlink(changed.c_str());

  std::string scratch = root + "/unit.csv";
  {
    std::ofstream f(scratch);
    f << "\xEF\xBB\xBF\"id\",value\r\n1,\"x\r\ny\"\r\n";
  }
  options = Options();
  expect(infer(scratch, options, root, schema, error));
  expect(reader.open(schema, root, error));
  expect(reader.next(row, error));
  assert(row[1].text == "x\r\ny");
  reader.close();
  {
    std::ofstream f(scratch);
    f << "id,value\n1,\"unfinished";
  }
  assert(infer(scratch, options, root, schema, error) == Status::INVALID);
  {
    std::ofstream f(scratch);
    f << "id,value\n1,2,3\n";
  }
  assert(infer(scratch, options, root, schema, error) == Status::INVALID);
  {
    std::ofstream f(scratch);
    f << "id,value\n1,\xff\n";
  }
  assert(infer(scratch, options, root, schema, error) == Status::INVALID);
  {
    std::ofstream f(scratch);
    f << "id,value\n1," << std::string(MAX_FIELD_BYTES + 1, 'a') << "\n";
  }
  assert(infer(scratch, options, root, schema, error) == Status::LIMIT);
  {
    std::ofstream f(scratch);
    f << R"(1;"say \"hi\"")" << '\n';
  }
  options.header = false;
  options.delimiter = ';';
  options.escape = '\\';
  expect(infer(scratch, options, root, schema, error));
  expect(reader.open(schema, root, error));
  expect(reader.next(row, error));
  assert(row[1].text == "say \"hi\"");
  reader.close();
  options = Options();
  options.format = Format::JSONL;
  {
    std::ofstream f(scratch);
    f << "{\"a\":\"123\",\"huge\":1e999,\"nil\":null}\n{\"a\":42,\"b\":true}\n";
  }
  expect(infer(scratch, options, root, schema, error));
  assert(schema.columns[0].type == Type::STRING && schema.columns[1].type == Type::STRING &&
         schema.columns[2].type == Type::NIL && schema.columns[3].nullable);
  {
    std::ofstream f(scratch);
    f << "{\"a\":{\"b\":1}}\n";
  }
  assert(infer(scratch, options, root, schema, error) != Status::OK);
  {
    std::ofstream f(scratch);
    f << "{\"a\":1,\"A\":2}\n";
  }
  assert(infer(scratch, options, root, schema, error) != Status::OK);
  ::unlink(scratch.c_str());
  Schema exported;
  exported.path = root + "/roundtrip.csv";
  exported.columns = {{"id", Type::INT, false}, {"text", Type::STRING, true}};
  exported.options.nullstr = "NULL";
  {
    CsvWriter writer;
    expect(writer.open(exported, root, error));
    expect(writer.write(
        {Cell{false, Type::INT, "1"}, Cell{false, Type::STRING, "quoted \"text\"\nline"}}, error));
    expect(writer.write({Cell{false, Type::INT, "2"}, Cell()}, error));
    expect(writer.commit(error));
  }
  expect(infer(exported.path, exported.options, root, schema, error));
  expect(reader.open(schema, root, error));
  expect(reader.next(row, error));
  assert(row[1].text == "quoted \"text\"\nline");
  expect(reader.next(row, error));
  assert(row[1].null);
  reader.close();
  {
    CsvWriter writer;
    assert(writer.open(exported, root, error) != Status::OK);
  }
  ::unlink(exported.path.c_str());
  {
    CsvWriter writer;
    expect(writer.open(exported, root, error));
    expect(writer.write({Cell{false, Type::INT, "3"}, Cell{false, Type::STRING, "uncommitted"}},
                        error));
  }
  assert(::access(exported.path.c_str(), F_OK) != 0);
  std::cout << "CSV, JSONL, Parquet, glob, schema, rescan and security checks passed\n";
}
