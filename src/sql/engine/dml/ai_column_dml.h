// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/expr/ob_expr_ai/ai_column_internal.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/rc/ob_server_runtime.h"
#include "rapidjson/document.h"
#include "query/engine/expr/ob_expr_lob_utils.h"
#include "common/json_type/ob_json_base.h"
#include <map>
#include <vector>

namespace oceanbase { namespace sql {
class AiColumnDml {
public:
  enum class Action { INSERT, UPDATE, DELETE };
  static int process(const ObDMLBaseCtDef &def, ObTableModifyOp &op, Action action) {
    try { return process_impl(def, op, action); }
    catch (const std::bad_alloc &) { return common::OB_ALLOCATE_MEMORY_FAILED; }
    catch (...) { return common::OB_ERR_UNEXPECTED; }
  }
private:
  class NestedStatement {
  public:
    explicit NestedStatement(ObTableModifyOp &op) : op_(op) {}
    int begin() { int ret = op_.begin_nested_session(true); active_ = ret == common::OB_SUCCESS; return ret; }
    int end() { if (!active_) return common::OB_SUCCESS; active_ = false; return op_.end_nested_session(); }
    ~NestedStatement() { if (active_) (void)op_.end_nested_session(); }
  private:
    ObTableModifyOp &op_;
    bool active_ = false;
  };
  static int process_impl(const ObDMLBaseCtDef &def, ObTableModifyOp &op, Action action) {
    using namespace common;
    using namespace share::schema;
    int ret = OB_SUCCESS;
    auto &ctx = op.get_eval_ctx();
    auto *session = op.get_exec_ctx().get_my_session();
    auto *sql_ctx = op.get_exec_ctx().get_sql_ctx();
    if (session == nullptr) return OB_ERR_UNEXPECTED;
    if (AiColumnInternalAccess::contains(session) || session->get_ddl_info().is_ddl()) return ret;
    // Native DDL/PX workers need not carry the coordinator's SQL schema guard.
    // Inspect their target through a runtime guard before deciding whether this
    // is an AI table; ordinary index construction must remain unaffected.
    ObSchemaGetterGuard runtime_guard;
    ObSchemaGetterGuard *guard = sql_ctx == nullptr ? nullptr : sql_ctx->schema_guard_;
    if (guard == nullptr) {
      if (GCTX.schema_service_ == nullptr) return OB_ERR_UNEXPECTED;
      if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(runtime_guard))) return ret;
      guard = &runtime_guard;
    }
    const ObTableSchema *table = nullptr;
    if (OB_FAIL(guard->get_table_schema(def.das_base_ctdef_.index_tid_, table))) return ret;
    if (table == nullptr) return OB_ERR_UNEXPECTED;
    if (!ai_column_has_definition(*table) || AiColumnInternalAccess::contains(session)) return ret;
    const auto *plan = op.get_spec().plan_;
    const uint64_t retry_column = plan != nullptr && plan->ai_retry_table_id_ == table->get_table_id() ? plan->ai_retry_column_id_ : 0;
    ObArenaAllocator allocator;
    std::map<uint64_t, std::string> old_values, new_values;
    auto literal = [&](uint64_t id, bool old, std::string &out) -> int {
      auto &cache = old ? old_values : new_values;
      auto found = cache.find(id);
      if (found != cache.end()) { out = found->second; return OB_SUCCESS; }
      int result = OB_SUCCESS;
      const auto &row = old ? def.old_row_ : def.new_row_;
      int64_t idx = index(def, id);
      if (idx < 0 || idx >= row.count() || row.at(idx) == nullptr) return OB_ERR_UNEXPECTED;
      ObExpr *expr = row.at(idx);
      ObDatum *datum = nullptr;
      ObObj obj;
      char *buf = nullptr; int64_t size = 0, pos = 0;
      if (OB_SUCCESS != (result = expr->eval(ctx, datum))) return result;
      if (!datum->is_null() && expr->obj_meta_.get_type() == ObJsonType) {
        ObString binary;
        if (OB_SUCCESS != (result = ObTextStringHelper::read_real_string_data(
            op.get_exec_ctx(), allocator, *datum, expr->datum_meta_, expr->obj_meta_.has_lob_header(), binary))) return result;
        if (binary.length() > 1024 * 1024) return OB_SIZE_OVERFLOW;
        ObIJsonBase *json_value = nullptr;
        ObJsonBuffer json_text(&allocator);
        if (OB_SUCCESS != (result = ObJsonBaseFactory::get_json_base(&allocator, binary,
            ObJsonInType::JSON_BIN, ObJsonInType::JSON_BIN, json_value, 0))) return result;
        if (json_value == nullptr) return OB_ERR_UNEXPECTED;
        if (OB_SUCCESS != (result = json_value->print(json_text, true))) return result;
        if (json_text.length() > 1024 * 1024) return OB_SIZE_OVERFLOW;
        out = "CAST(" + ai_column_literal(std::string(json_text.ptr(), json_text.length())) + " AS JSON)";
        cache.emplace(id, out);
        return OB_SUCCESS;
      }
      if (!datum->is_null() && is_lob_storage(expr->obj_meta_.get_type())) {
        ObString value;
        if (OB_SUCCESS != (result = ObTextStringHelper::read_real_string_data(
            op.get_exec_ctx(), allocator, *datum, expr->datum_meta_, expr->obj_meta_.has_lob_header(), value))) return result;
        if (value.length() > 1024 * 1024) return OB_SIZE_OVERFLOW;
        obj.set_varchar(value);
        obj.set_collation_type(expr->obj_meta_.get_collation_type());
      } else if (OB_SUCCESS != (result = datum->to_obj(obj, expr->obj_meta_))) return result;
      if (!obj.is_null() && ob_is_string_type(obj.get_type())) {
        const auto *column = table->get_column_schema(id);
        if (column == nullptr) return OB_ERR_UNEXPECTED;
        const ObString bytes = obj.get_string();
        out = ai_column_literal(str(bytes));
        const size_t charset = out.rfind("utf8mb4");
        const char *name = ObCharset::charset_name(column->get_charset_type());
        if (charset == std::string::npos || name == nullptr) return OB_ERR_UNEXPECTED;
        out.replace(charset, 7, name);
        out += " COLLATE ";
        out += ObCharset::collation_name(column->get_collation_type());
        cache.emplace(id, out);
        return OB_SUCCESS;
      }
      if (OB_SUCCESS != (result = obj.print_sql_literal(buf, size, pos, allocator, op.get_obj_print_params()))) return result;
      out.assign(buf, pos); cache.emplace(id, out);
      return OB_SUCCESS;
    };
    auto predicate = [&](bool old, std::string &out) -> int {
      for (int64_t i = 0; i < table->get_column_count(); ++i) {
        const auto *column = table->get_column_schema_by_idx(i);
        if (!column->is_rowkey_column()) continue;
        std::string value;
        int result = literal(column->get_column_id(), old, value);
        if (result != OB_SUCCESS) return result;
        if (!out.empty()) out += " AND ";
        out += ai_column_identifier(str(column->get_column_name_str())) + " = " + value;
      }
      return out.empty() ? OB_NOT_SUPPORTED : OB_SUCCESS;
    };
    std::string old_key, new_key;
    if (action != Action::INSERT && OB_FAIL(predicate(true, old_key))) return ret;
    if (action != Action::DELETE && OB_FAIL(predicate(false, new_key))) return ret;
    struct Pending { const ObColumnSchemaV2 *column; std::string select; uint64_t state; uint64_t request; int64_t retries; };
    std::vector<Pending> pending;
    size_t captured_bytes = 0, payload_bytes = 0;
    for (int64_t i = 0; action != Action::DELETE && i < table->get_column_count(); ++i) {
      const auto *column = table->get_column_schema_by_idx(i);
      if (!column->is_ai_column() || (retry_column != 0 && column->get_column_id() != retry_column)) continue;
      rapidjson::Document meta; meta.Parse(column->get_ai_column_info().ptr(), column->get_ai_column_info().length());
      if (meta.HasParseError() || !meta.IsObject() || !meta.HasMember("dependencies") || !meta["dependencies"].IsArray() ||
          !meta.HasMember("expression") || !meta["expression"].IsString() ||
          !meta.HasMember("state_column_id") || !meta["state_column_id"].IsUint64() ||
          !meta.HasMember("request_column_id") || !meta["request_column_id"].IsUint64() ||
          !meta.HasMember("max_retries") || !meta["max_retries"].IsInt64()) return OB_ERR_UNEXPECTED;
      bool changed = retry_column != 0 || action == Action::INSERT || old_key != new_key;
      std::string source;
      for (const auto &dep : meta["dependencies"].GetArray()) {
        if (!dep.IsUint64()) return OB_ERR_UNEXPECTED;
        auto *dependency = table->get_column_schema(dep.GetUint64());
        if (dependency == nullptr) return OB_ERR_UNEXPECTED;
        std::string value, old;
        if (OB_FAIL(literal(dep.GetUint64(), false, value))) return ret;
        if (action == Action::UPDATE) {
          if (OB_FAIL(literal(dep.GetUint64(), true, old))) return ret;
          changed |= old != value;
        }
        if (!source.empty()) source += ",";
        if (source.size() + value.size() > 4 * 1024 * 1024) return OB_SIZE_OVERFLOW;
        source += value + " AS " + ai_column_identifier(str(dependency->get_column_name_str()));
      }
      if (!changed) continue;
      std::string captured;
      if (!ai_column_capture_expression(meta["expression"].GetString(), captured)) return OB_ERR_UNEXPECTED;
      std::string query = "SELECT CAST(" + captured + " AS CHAR CHARACTER SET utf8mb4) AS payload";
      if (!source.empty()) query += " FROM (SELECT " + source + ") AS __ai_inputs";
      captured_bytes += query.size();
      if (captured_bytes > 8 * 1024 * 1024) return OB_SIZE_OVERFLOW;
      pending.push_back({column, query, meta["state_column_id"].GetUint64(), meta["request_column_id"].GetUint64(), meta["max_retries"].GetInt64()});
    }
    if (action != Action::DELETE && pending.empty()) return ret;
    char request[37] = {};
    if (action != Action::DELETE && OB_FAIL(ObExprUuid::gen_server_uuid(request, 36))) return ret;
    if (OB_FAIL(op.open_inner_conn())) return ret;
    // Never start an autonomous transaction: these statements use the business
    // session's nested connection and therefore share commit and rollback.
    if (action == Action::DELETE) {
      const std::string sql = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=" +
          std::to_string(table->get_table_id()) + " AND BINARY rowkey_predicate=BINARY " + ai_column_literal(old_key);
      ret = write(op, sql);
    } else {
      for (const auto &task : pending) {
        if (OB_FAIL(ret)) break;
        std::string payload;
        {
          NestedStatement nested(op);
          if (OB_FAIL(nested.begin())) break;
          {
          AiColumnInternalAccess access(session);
          ObMySQLProxy::MySQLResult result;
          if (OB_FAIL(op.execute_read(task.select.c_str(), result))) {
          } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(result.get_result()->next())) {
          } else {
            ObString value;
            if (OB_FAIL(result.get_result()->get_varchar("payload", value))) {
            } else if (value.length() > 1024 * 1024) { ret = OB_SIZE_OVERFLOW;
            } else {
              payload_bytes += value.length();
              if (payload_bytes > 8 * 1024 * 1024) ret = OB_SIZE_OVERFLOW;
              else payload = str(value);
            }
          }
          }
          const int end_ret = nested.end();
          if (OB_SUCC(ret)) ret = end_ret;
        }
        if (OB_SUCC(ret) && action == Action::UPDATE) {
          const std::string cleanup = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=" +
              std::to_string(table->get_table_id()) + " AND column_id=" + std::to_string(task.column->get_column_id()) +
              " AND BINARY rowkey_predicate=BINARY " + ai_column_literal(old_key);
          ret = write(op, cleanup);
        }
        if (OB_SUCC(ret)) {
          const std::string sql = "INSERT INTO oceanbase.__all_ai_column_task "
              "(table_id,request_id,column_id,definition_version,rowkey_predicate,state,retry_count,max_retries,eligible_ts,input_payload,claim_epoch,error_code) VALUES(" +
              std::to_string(table->get_table_id()) + "," + ai_column_literal(request) + "," + std::to_string(task.column->get_column_id()) + "," +
              std::to_string(task.column->get_schema_version()) + "," + ai_column_literal(new_key) + ",0,0," + std::to_string(task.retries) +
              ",NOW(6)," + ai_column_literal(payload) + ",0,0)";
          ret = write(op, sql);
        }
      }
    }
    for (const auto &task : pending) {
      if (OB_FAIL(ret)) break;
      ObExpr *result = expr(def, task.column->get_column_id());
      ObExpr *state = expr(def, task.state);
      ObExpr *token = expr(def, task.request);
      if (result == nullptr || state == nullptr || token == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
      result->locate_datum_for_write(ctx).set_null(); result->get_eval_info(ctx).evaluated_ = true;
      state->locate_datum_for_write(ctx).set_int(0); state->get_eval_info(ctx).evaluated_ = true;
      char *memory = token->get_str_res_mem(ctx, 36);
      if (memory == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; break; }
      MEMCPY(memory, request, 36); token->locate_datum_for_write(ctx).set_string(memory, 36);
      token->get_eval_info(ctx).evaluated_ = true;
    }
    return ret;
  }
private:
  static int write(ObTableModifyOp &op, const std::string &sql) {
    NestedStatement nested(op);
    int ret = nested.begin();
    if (ret == common::OB_SUCCESS) {
      AiColumnInternalAccess access(op.get_exec_ctx().get_my_session());
      ret = op.execute_write(sql.c_str());
      const int end_ret = nested.end();
      if (ret == common::OB_SUCCESS) ret = end_ret;
    }
    return ret;
  }
  static std::string str(const common::ObString &s) { return std::string(s.ptr(), s.length()); }
  static int64_t index(const ObDMLBaseCtDef &def, uint64_t id) {
    for (int64_t i = 0; i < def.column_ids_.count(); ++i) if (def.column_ids_.at(i) == id) return i;
    return -1;
  }
  static ObExpr *expr(const ObDMLBaseCtDef &def, uint64_t id) {
    const int64_t i = index(def, id);
    return i >= 0 && i < def.new_row_.count() ? def.new_row_.at(i) : nullptr;
  }
};
}} // namespace oceanbase::sql
