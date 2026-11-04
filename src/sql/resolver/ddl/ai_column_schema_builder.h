// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/resolver/ddl/ai_column_definition.h"
#include "share/schema/ob_table_schema.h"
#include "query/vector/ob_vector_index_util.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace oceanbase { namespace sql {

// Called after ordinary column resolution, so forward references are checked
// against the complete schema and AI-to-AI references cannot depend on order.
inline int build_ai_column_schemas(const ParseNode *elements,
                                  share::schema::ObTableSchema &table,
                                  uint64_t &last_column_id)
{
  using namespace common;
  using namespace share::schema;
  using Type = AiColumnDefinition::ResultType;
  using Error = AiColumnDefinition::Error;
  int ret = OB_SUCCESS;
  std::vector<AiColumnDefinition::Column> columns;
  std::vector<std::pair<std::string, const ParseNode *>> definitions;
  if (elements == nullptr) return ret;
  for (int i = 0; i < elements->num_child_; ++i) {
    const ParseNode *node = AiColumnDefinition::child(elements, i);
    if (node != nullptr && node->type_ == T_COLUMN_DEFINITION && node->num_child_ == 5) {
      const ParseNode *definition = AiColumnDefinition::child(node, 4);
      if (definition != nullptr && definition->type_ == T_AI_COLUMN_DEFINITION) {
        const auto name = AiColumnDefinition::text(AiColumnDefinition::child(
            AiColumnDefinition::child(node, 0), 2));
        definitions.emplace_back(name, definition);
      }
    }
  }
  if (definitions.empty()) return ret;
  for (int64_t i = 0; i < table.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema_by_idx(i);
    if (column == nullptr) return OB_ERR_UNEXPECTED;
    if (column->is_hidden()) continue;
    const std::string name(column->get_column_name_str().ptr(), column->get_column_name_str().length());
    bool is_ai = column->is_ai_column();
    for (const auto &definition : definitions) {
      is_ai |= AiColumnDefinition::lower(name) == AiColumnDefinition::lower(definition.first);
    }
    columns.push_back({name, is_ai});
  }
  for (const auto &entry : definitions) {
    ObColumnSchemaV2 *column = table.get_column_schema(ObString::make_string(entry.first.c_str()));
    if (column == nullptr) return OB_ERR_UNEXPECTED;
    if (column->is_rowkey_column() || column->is_generated_column() || column->is_autoincrement()) {
      return OB_NOT_SUPPORTED;
    }
    Type type;
    int64_t dimension = 0;
    if (column->is_json()) {
      type = Type::JSON;
    } else if (column->is_collection()) {
      type = Type::VECTOR;
      if (OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
          column->get_extended_type_info(), dimension))) return ret;
    } else if (ob_is_integer_type(column->get_data_type())) {
      type = column->get_data_type() == ObTinyIntType && column->get_data_precision() == 1
          ? Type::BOOLEAN : Type::INTEGER;
    } else if (ob_is_float_tc(column->get_data_type()) || ob_is_double_tc(column->get_data_type())) {
      type = Type::REAL;
    } else if (column->is_string_type() || is_lob_storage(column->get_data_type())) {
      type = Type::TEXT;
    } else {
      return OB_NOT_SUPPORTED;
    }
    AiColumnDefinition definition;
    const Error error = AiColumnDefinition::resolve(entry.second, type, dimension, entry.first, columns, definition);
    if (error == Error::DIMENSION) {
      const ParseNode *args = AiColumnDefinition::child(AiColumnDefinition::child(entry.second, 0), 1);
      const ParseNode *requested = AiColumnDefinition::child(args, 2);
      const uint32_t actual = requested != nullptr && requested->type_ == T_INT
          ? static_cast<uint32_t>(requested->value_) : 0;
      LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(dimension), actual);
      return OB_ERR_INVALID_VECTOR_DIM;
    } else if (error != Error::NONE) {
      return OB_NOT_SUPPORTED;
    }
    // Resolve only model metadata here; DDL must never issue an endpoint request.
    ObArenaAllocator model_allocator;
    ObAIFuncExprInfo *model_info = nullptr;
    if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(model_allocator,
        ObString::make_string(definition.model.c_str()), model_info))) return ret;
    if (model_info == nullptr) return OB_ERR_UNEXPECTED;
    const auto expected_type = type == Type::VECTOR
        ? share::EndpointType::DENSE_EMBEDDING : share::EndpointType::COMPLETION;
    if (model_info->type_ != expected_type) return OB_NOT_SUPPORTED;
    uint64_t hidden_ids[3];
    // Allocate IDs before adding columns: add_column can invalidate schema pointers.
    const uint64_t result_id = column->get_column_id();
    for (int j = 0; j < 3; ++j) {
      hidden_ids[j] = ++last_column_id;
      const std::string name = "__ai_" + std::to_string(result_id) + "_" + std::to_string(j);
      if (table.get_column_schema(ObString::make_string(name.c_str())) != nullptr) return OB_NOT_SUPPORTED;
      ObColumnSchemaV2 hidden;
      hidden.set_table_id(table.get_table_id());
      hidden.set_column_id(hidden_ids[j]);
      hidden.set_schema_version(table.get_schema_version());
      hidden.set_is_hidden(true);
      hidden.set_nullable(true);
      hidden.set_data_type(j == 0 ? ObIntType : ObVarcharType);
      if (j != 0) {
        hidden.set_data_length(j == 1 ? 64 : 4096);
        hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
      if (OB_FAIL(hidden.set_column_name(ObString::make_string(name.c_str())))) return ret;
      if (j == 0) {
        ObObj pending; pending.set_int(0);
        if (OB_FAIL(hidden.set_orig_default_value(pending)) || OB_FAIL(hidden.set_cur_default_value(pending, false))) return ret;
      }
      if (OB_FAIL(table.add_column(hidden))) return ret;
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("version"); writer.Int(1);
    writer.Key("expression"); writer.String(definition.expression.data(), definition.expression.size());
    writer.Key("model"); writer.String(definition.model.data(), definition.model.size());
    writer.Key("prompt"); writer.String(definition.result_prompt.data(), definition.result_prompt.size());
    writer.Key("type"); writer.Int(static_cast<int>(type));
    writer.Key("dimension"); writer.Int64(dimension);
    writer.Key("max_retries"); writer.Int64(definition.max_retries);
    writer.Key("state_column_id"); writer.Uint64(hidden_ids[0]);
    writer.Key("request_column_id"); writer.Uint64(hidden_ids[1]);
    writer.Key("result_spec_column_id"); writer.Uint64(hidden_ids[2]);
    writer.Key("dependencies"); writer.StartArray();
    for (const auto &name : definition.dependencies) {
      const ObColumnSchemaV2 *dependency = table.get_column_schema(ObString::make_string(name.c_str()));
      if (dependency == nullptr) return OB_ERR_UNEXPECTED;
      writer.Uint64(dependency->get_column_id());
    }
    writer.EndArray(); writer.EndObject();
    column = table.get_column_schema(result_id);
    if (column == nullptr) return OB_ERR_UNEXPECTED;
    column->set_nullable(true);
    if (OB_FAIL(column->set_ai_column_info(ObString(buffer.GetSize(), buffer.GetString())))) return ret;
  }
  table.set_max_used_column_id(last_column_id);
  return ret;
}

}} // namespace oceanbase::sql
