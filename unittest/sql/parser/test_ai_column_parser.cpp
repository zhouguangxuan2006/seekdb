// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ddl/ai_column_definition.h"
#include "lib/allocator/page_arena.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace oceanbase::common;
using namespace oceanbase::sql;

static void require(bool ok, const char *message)
{
  if (!ok) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static const ParseNode *find(const ParseNode *n, ObItemType type)
{
  if (n == nullptr) return nullptr;
  if (n->type_ == type) return n;
  for (int i = 0; i < n->num_child_; ++i) {
    if (const ParseNode *found = find(n->children_[i], type)) return found;
  }
  return nullptr;
}
static void check(const char *sql, ObItemType type, bool success = true)
{
  ObArenaAllocator arena;
  ObParser parser(arena, SMO_DEFAULT);
  ParseResult result{};
  int ret = parser.parse(ObString::make_string(sql), result);
  if (!success) { require(ret != 0, sql); return; }
  if (ret != 0) fprintf(stderr, "parse error %d: %s\n", ret, result.error_msg_);
  require(ret == 0, sql);
  const ParseNode *node = find(result.result_tree_, type);
  require(node != nullptr, sql);
  if (type == T_AI_COLUMN_DEFINITION) {
    require(node->num_child_ == 2 && node->children_[0] != nullptr, "AI expression retained");
    require(node->children_[0]->str_len_ > 0, "AI expression SQL retained");
    if (node->children_[1] != nullptr) {
      require(node->children_[1]->type_ == T_EXPR_LIST, "named options list");
      require(node->children_[1]->num_child_ == 2, "both named options retained");
    }
  }
  parser.free_result(result);
}
static void definition_check(const char *expression, const char *options,
                             AiColumnDefinition::ResultType type,
                             AiColumnDefinition::Error expected)
{
  const std::string sql = std::string("create table t (body text, result text ai column (")
      + expression + ") " + options + ")";
  ObArenaAllocator arena;
  ObParser parser(arena, SMO_DEFAULT);
  ParseResult parsed{};
  require(parser.parse(ObString::make_string(sql.c_str()), parsed) == 0, sql.c_str());
  AiColumnDefinition definition;
  const auto error = AiColumnDefinition::resolve(find(parsed.result_tree_, T_AI_COLUMN_DEFINITION),
      type, 4, "result", {{"body", false}, {"other_ai", true}}, definition);
  if (error != expected) fprintf(stderr, "resolver: got %d expected %d\n", int(error), int(expected));
  require(error == expected, sql.c_str());
  if (error == AiColumnDefinition::Error::NONE) {
    require(definition.dependencies == std::vector<std::string>{"body"}, "resolved dependency");
    if (type == AiColumnDefinition::ResultType::JSON) {
      require(definition.result_prompt.find("Return exactly one valid JSON value") == 0, "JSON result prompt");
    }
  }
  parser.free_result(parsed);
}
int main()
{
  check("create table t (id bigint primary key, body text, tags json AI COLUMN "
        "(AI_COMPLETE('text_model', concat('Classify: ', body))) "
        "WITH (sync_mode='async', max_retries=2))", T_AI_COLUMN_DEFINITION);
  check("create table t (body text, v vector(4) ai column (ai_embed('embed_model', body)))",
        T_AI_COLUMN_DEFINITION);
  check("alter table t add column summary varchar(100) ai column "
        "(ai_complete('m', body)) with (max_retries=0,sync_mode='async')", T_AI_COLUMN_DEFINITION);
  check("alter table t modify column summary varchar(200) ai column (ai_complete('m', body))",
        T_AI_COLUMN_DEFINITION);
  check("show ai columns from db.t", T_SHOW_AI_COLUMNS);
  check("show ai columns in t", T_SHOW_AI_COLUMNS);
  check("alter table t retry ai column summary where status='FAILED' and id in (1,3)", T_RETRY_AI_COLUMN);
  check("alter table t retry ai column summary where `status`='active'", T_RETRY_AI_COLUMN);
  check("alter table t retry ai column summary", T_RETRY_AI_COLUMN);
  check("create table t (a int, b int generated always as (a+1) stored)", T_COLUMN_DEFINITION);
  check("select ai, retry, status from t", T_SELECT);
  check("select * from read_csv('/tmp/data.csv', header=true)", T_SELECT);
  check("create table t (s text ai column ())", T_AI_COLUMN_DEFINITION, false);
  check("create table t (s text ai column (ai_complete('m','x')) with ())", T_AI_COLUMN_DEFINITION, false);
  check("create table t (s text ai column (ai_complete('m','x')) with (sync_mode))", T_AI_COLUMN_DEFINITION, false);
  check("alter table t retry ai column", T_RETRY_AI_COLUMN, false);
  using Type = AiColumnDefinition::ResultType;
  using Error = AiColumnDefinition::Error;
  definition_check("AI_COMPLETE('model', concat(body,body))", "", Type::JSON, Error::NONE);
  definition_check("AI_COMPLETE('model', body)", "with (sync_mode='async',max_retries=0)", Type::TEXT, Error::NONE);
  definition_check("AI_EMBED('model', body,4)", "", Type::VECTOR, Error::NONE);
  definition_check("AI_EMBED('model', body,3)", "", Type::VECTOR, Error::DIMENSION);
  definition_check("AI_COMPLETE('model', body)", "with (sync_mode='sync')", Type::TEXT, Error::OPTION);
  definition_check("AI_COMPLETE('model', body)", "with (max_retries=-1)", Type::TEXT, Error::OPTION);
  definition_check("AI_COMPLETE('model', body)", "with (max_retries=1,max_retries=2)", Type::TEXT, Error::OPTION);
  definition_check("AI_COMPLETE('model', body)", "with (unknown='value')", Type::TEXT, Error::OPTION);
  definition_check("AI_COMPLETE('model', missing)", "", Type::TEXT, Error::DEPENDENCY);
  definition_check("AI_COMPLETE('model', result)", "", Type::TEXT, Error::DEPENDENCY);
  definition_check("AI_COMPLETE('model', other_ai)", "", Type::TEXT, Error::DEPENDENCY);
  definition_check("AI_COMPLETE('model', t.body)", "", Type::TEXT, Error::DEPENDENCY);
  definition_check("AI_COMPLETE('model', AI_COMPLETE('model',body))", "", Type::TEXT, Error::EXPRESSION);
  definition_check("AI_COMPLETE('model', (select body from t))", "", Type::TEXT, Error::EXPRESSION);
  definition_check("AI_COMPLETE('model', @body)", "", Type::TEXT, Error::EXPRESSION);
  definition_check("AI_COMPLETE('model', concat(body,rand()))", "", Type::TEXT, Error::EXPRESSION);
  definition_check("AI_EMBED('model', body)", "", Type::TEXT, Error::EXPRESSION);
  puts("PASS AI COLUMN definition validation: options, dependencies, dimension, typed prompt");
  puts("PASS AI COLUMN parser: CREATE/ADD/MODIFY/SHOW/RETRY, malformed SQL, ordinary SQL");
}
