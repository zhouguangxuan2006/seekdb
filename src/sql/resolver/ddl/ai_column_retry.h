// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/resolver/ob_resolver_define.h"
#include "sql/ob_sql_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/engine/expr/ob_expr_ai/ai_column_internal.h"
#include "rapidjson/document.h"
#include <vector>
namespace oceanbase { namespace sql {
// Lower RETRY to a native UPDATE. Conditions, row locking, statement rollback,
// index invalidation and queue insertion consequently use the same DML path.
inline int lower_ai_column_retry(ObResolverParams &params, const ParseNode &tree, const ParseNode *&out) {
  using namespace common;
  int ret = OB_SUCCESS;
  out = &tree;
  if (tree.type_ != T_ALTER_TABLE || tree.num_child_ < 2 || tree.children_[1] == nullptr) return ret;
  const ParseNode *actions = tree.children_[1], *retry = nullptr;
  for (int i = 0; i < actions->num_child_; ++i) {
    if (actions->children_[i] != nullptr && actions->children_[i]->type_ == T_RETRY_AI_COLUMN) retry = actions->children_[i];
  }
  if (retry == nullptr) return ret;
  if (actions->num_child_ != 1) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action"); return OB_NOT_SUPPORTED;
  }
  const ParseNode *relation = tree.children_[0];
  if (relation == nullptr || relation->num_child_ < 2 || relation->children_[1] == nullptr || retry->num_child_ != 2 || retry->children_[0] == nullptr) return OB_ERR_UNEXPECTED;
  auto text = [](const ParseNode *n) { return ObString(static_cast<int32_t>(n->str_len_), n->str_value_); };
  ObString db = relation->children_[0] == nullptr ? params.session_info_->get_database_name() : text(relation->children_[0]);
  ObString name = text(relation->children_[1]);
  if (db.empty()) return OB_ERR_NO_DB_SELECTED;
  const share::schema::ObTableSchema *table = nullptr;
  if (OB_FAIL(params.schema_checker_->get_table_schema(db, name, false, table))) return ret;
  if (table == nullptr) return OB_TABLE_NOT_EXIST;
  const auto *column = table->get_column_schema(text(retry->children_[0]));
  if (column == nullptr || !column->is_ai_column()) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column"); return OB_NOT_SUPPORTED;
  }
  rapidjson::Document meta; meta.Parse(column->get_ai_column_info().ptr(), column->get_ai_column_info().length());
  if (meta.HasParseError() || !meta.IsObject() || !meta.HasMember("state_column_id") || !meta.HasMember("request_column_id")) return OB_ERR_UNEXPECTED;
  const auto *state = table->get_column_schema(meta["state_column_id"].GetUint64());
  const auto *token = table->get_column_schema(meta["request_column_id"].GetUint64());
  if (state == nullptr || token == nullptr) return OB_ERR_UNEXPECTED;
  auto ident = [](const ObString &s) { return ai_column_identifier(std::string(s.ptr(), s.length())); };
  const std::string qualified = ident(db) + "." + ident(name);
  const std::string prefix = "UPDATE /*+ USE_PLAN_CACHE(NONE) */ " + qualified + " SET " + ident(token->get_column_name_str()) + " = UUID()";
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
  ParseResult update_result, status_result;
  const std::string status_sql = prefix + " WHERE CASE " + ident(state->get_column_name_str()) + " WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END";
  ObString update_text, status_text;
  if (OB_FAIL(ob_write_string(*params.allocator_, ObString::make_string(prefix.c_str()), update_text))) return ret;
  if (OB_FAIL(ob_write_string(*params.allocator_, ObString::make_string(status_sql.c_str()), status_text))) return ret;
  if (OB_FAIL(parser.parse(update_text, update_result))) return ret;
  if (OB_FAIL(parser.parse(status_text, status_result))) return ret;
  ParseNode *update = update_result.result_tree_->children_[0];
  ParseNode *status = status_result.result_tree_->children_[0]->children_[3]->children_[0];
  ParseNode *where = retry->children_[1];
  // Replace only unqualified, unquoted status references; quoted status remains
  // a normal business column. String constants and function names are untouched.
  std::vector<ParseNode **> nodes; nodes.push_back(&where);
  while (!nodes.empty()) {
    ParseNode **slot = nodes.back(); nodes.pop_back();
    ParseNode *node = *slot;
    if (node == nullptr) continue;
    if (node->type_ == T_COLUMN_REF && node->num_child_ == 3 && node->children_[0] == nullptr && node->children_[1] == nullptr && node->children_[2] != nullptr &&
        !node->children_[2]->is_input_quoted_ && text(node->children_[2]).case_compare("status") == 0) {
      *slot = status;
    } else {
      for (int i = 0; i < node->num_child_; ++i) nodes.push_back(&node->children_[i]);
    }
  }
  update->children_[3] = where;
  params.query_ctx_->ai_retry_table_id_ = table->get_table_id();
  params.query_ctx_->ai_retry_column_id_ = column->get_column_id();
  out = update;
  return ret;
}
}} // namespace oceanbase::sql
