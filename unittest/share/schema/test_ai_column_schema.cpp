// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "share/schema/ob_column_schema.h"
#include "lib/hash/ob_hashmap.h"
#include "rapidjson/document.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::share::schema;
static void require(bool ok, const char *why)
{
  if (!ok) { fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
int main()
{
  std::string source = R"JSON({"version":1,"expression":"AI_COMPLETE('m',body)","state_column_id":20,"request_column_id":21,"result_spec_column_id":22,"dependencies":[16]})JSON";
  const std::string expected = source;
  ObColumnSchemaV2 column;
  column.set_column_id(17);
  column.set_is_hidden(true);
  require(column.set_ai_column_info(ObString(source.size(), source.data())) == OB_SUCCESS, "set metadata");
  source.assign(source.size(), 'x');
  require(column.get_ai_column_info() == ObString::make_string(expected.c_str()), "metadata owns caller buffer");
  ObColumnSchemaV2 copy;
  require(copy.assign(column) == OB_SUCCESS, "schema deep copy");
  column.reset();
  require(!column.is_ai_column(), "reset removes AI definition");
  require(copy.is_ai_column() && copy.is_hidden() && copy.get_column_id() == 17, "copy preserves metadata and flags");
  require(copy.get_ai_column_info() == ObString::make_string(expected.c_str()), "copy survives source reset");
  std::vector<char> bytes(copy.get_serialize_size());
  int64_t pos = 0;
  const int serialize_ret = copy.serialize(bytes.data(), bytes.size(), pos);
  if (serialize_ret != OB_SUCCESS) fprintf(stderr, "serialize ret=%d size=%zu pos=%ld\n", serialize_ret, bytes.size(), pos);
  require(serialize_ret == OB_SUCCESS, "serialize AI metadata");
  require(pos == static_cast<int64_t>(bytes.size()), "exact serialized size");
  ObColumnSchemaV2 restored;
  pos = 0;
  require(restored.deserialize(bytes.data(), bytes.size(), pos) == OB_SUCCESS, "deserialize AI metadata");
  require(pos == static_cast<int64_t>(bytes.size()), "consume complete serialized schema");
  ObColumnSchemaV2 truncated;
  pos = 0;
  require(truncated.deserialize(bytes.data(), bytes.size()-1, pos) != OB_SUCCESS, "reject truncated metadata");
  std::fill(bytes.begin(), bytes.end(), 0);
  require(restored.get_ai_column_info() == ObString::make_string(expected.c_str()), "decoded schema owns wire bytes");
  hash::ObHashMap<uint64_t, uint64_t> ids;
  require(ids.create(16, ObModIds::OB_HASH_BUCKET) == OB_SUCCESS, "create column map");
  for (uint64_t id : {16,17,20,21,22}) require(ids.set_refactored(id,id+100) == OB_SUCCESS, "populate column map");
  require(restored.convert_column_id(ids) == OB_SUCCESS, "remap AI column and references");
  require(restored.get_column_id() == 117, "remap result ID");
  rapidjson::Document doc;
  doc.Parse(restored.get_ai_column_info().ptr(), restored.get_ai_column_info().length());
  require(!doc.HasParseError(), "metadata remains JSON after remap");
  require(doc["state_column_id"].GetUint64() == 120, "remap state ID");
  require(doc["request_column_id"].GetUint64() == 121, "remap request ID");
  require(doc["result_spec_column_id"].GetUint64() == 122, "remap result spec ID");
  require(doc["dependencies"][0].GetUint64() == 116, "remap dependency ID");
  require(std::string(doc["expression"].GetString()) == "AI_COMPLETE('m',body)", "preserve expression during remap");
  ObColumnSchemaV2 ordinary;
  bytes.resize(ordinary.get_serialize_size()); pos = 0;
  require(ordinary.serialize(bytes.data(), bytes.size(), pos) == OB_SUCCESS, "serialize ordinary column");
  pos = 0;
  require(restored.deserialize(bytes.data(), bytes.size(), pos) == OB_SUCCESS, "reuse decoder for ordinary column");
  require(!restored.is_ai_column(), "ordinary definition clears previous AI state");
  puts("PASS AI column schema: ownership, copy, reset, serialization, truncation, ID remapping");
}
