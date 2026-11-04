// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/expr/ob_expr_ai/ai_column_internal.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "rapidjson/document.h"
namespace oceanbase { namespace sql {
inline int check_ai_column_strict_read(ObExecContext &ctx, const ObPhysicalPlan &plan) {
  using namespace common;
  int ret = OB_SUCCESS;
  auto *session = ctx.get_my_session();
  if (plan.ai_column_refs_.empty() || AiColumnInternalAccess::contains(session)) return ret;
  int64_t enabled = 0;
  if (OB_FAIL(session->get_sys_variable_by_name(ObString::make_string("ai_column_strict_read"), enabled))) return ret;
  if (!enabled) return ret;
  auto *guard = ctx.get_sql_ctx()->schema_guard_;
  if (guard == nullptr) return OB_ERR_UNEXPECTED;
  for (int64_t i = 0; OB_SUCC(ret) && i + 1 < plan.ai_column_refs_.count(); i += 2) {
    const uint64_t table_id = plan.ai_column_refs_.at(i), column_id = plan.ai_column_refs_.at(i + 1);
    const share::schema::ObTableSchema *table = nullptr;
    const share::schema::ObDatabaseSchema *database = nullptr;
    if (OB_FAIL(guard->get_table_schema(table_id, table))) break;
    if (table == nullptr) { ret = OB_TABLE_NOT_EXIST; break; }
    const auto *column = table->get_column_schema(column_id);
    if (column == nullptr || !column->is_ai_column()) { ret = OB_SCHEMA_ERROR; break; }
    rapidjson::Document meta;
    meta.Parse(column->get_ai_column_info().ptr(), column->get_ai_column_info().length());
    if (meta.HasParseError() || !meta.IsObject() || !meta.HasMember("state_column_id") || !meta["state_column_id"].IsUint64()) { ret = OB_ERR_UNEXPECTED; break; }
    const auto *state = table->get_column_schema(meta["state_column_id"].GetUint64());
    if (state == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
    if (OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) break;
    if (database == nullptr) { ret = OB_ERR_BAD_DATABASE; break; }
    auto ident = [](const ObString &s) { return ai_column_identifier(std::string(s.ptr(), s.length())); };
    const std::string query = "SELECT /*+ USE_PLAN_CACHE(NONE) */ 1 FROM " + ident(database->get_database_name_str()) + "." + ident(table->get_table_name_str()) +
        " WHERE " + ident(state->get_column_name_str()) + " IS NULL OR " + ident(state->get_column_name_str()) + " <> 1 LIMIT 1";
    sqlclient::ObISQLConnectionGuard connection;
    ObIInnerSQLConnection *conn = nullptr;
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_spi_connection_with_external_session(session, connection))) break;
    if ((conn = as_inner_sql_connection(connection.get_ptr())) == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
    ObSQLSessionInfo::StmtSavedValue saved_session;
    ObIInnerSQLConnection::SavedValue saved_conn;
    if (OB_FAIL(conn->begin_nested_session(saved_session, saved_conn, true))) break;
    bool unfinished = false;
    {
      AiColumnInternalAccess access(session);
      ObMySQLProxy::MySQLResult result;
      if (OB_FAIL(conn->execute_read(query.c_str(), result))) {
      } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
      } else {
        ret = result.get_result()->next();
        if (ret == OB_ITER_END) ret = OB_SUCCESS;
        else if (OB_SUCC(ret)) unfinished = true;
      }
    }
    const int end_ret = conn->end_nested_session(saved_session, saved_conn);
    if (OB_SUCC(ret)) ret = end_ret;
    if (OB_SUCC(ret) && unfinished) {
      ret = OB_AI_COLUMN_NOT_READY;
      LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, table_id, column_id);
    }
  }
  return ret;
}
}} // namespace oceanbase::sql
