// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "share/ob_thread_pool.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ai_column_internal.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/session/ob_sql_session_info.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <vector>
#include <unistd.h>

namespace oceanbase { namespace omt {
class AiColumnWorker : public lib::ThreadPool {
  using String = std::string;
  std::mutex recovery_mutex_;
  bool recovered_ = false;
  bool recover_after_restart() {
    // This engine is single-process: every old lease belongs to the previous
    // process. Recover it once before any worker is allowed to claim new work.
    std::lock_guard<std::mutex> lock(recovery_mutex_);
    if (!recovered_) {
      int64_t affected = 0;
      recovered_ = GCTX.sql_proxy_->write(
          "UPDATE oceanbase.__all_ai_column_task SET state=0,claim_owner=NULL,claim_epoch=claim_epoch+1,lease_until=NULL,eligible_ts=NOW(6) WHERE state=1",
          affected) == common::OB_SUCCESS;
    }
    return recovered_;
  }
  struct Task {
    int64_t table=0, column=0, version=0, current_version=0, epoch=0, state=0, retry=0, retries=0;
    String request, key, input, output, spec;
  };
  static String number(int64_t n) { return std::to_string(n); }
  static String literal(const String &s) { return sql::ai_column_literal(s); }
  static String identifier(const common::ObString &s) { return sql::ai_column_identifier(String(s.ptr(),s.length())); }
  static String group(int64_t table, const String &request) {
    return "table_id="+number(table)+" AND request_id="+literal(request);
  }
  static String json(const rapidjson::Value &value) {
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer); return String(buffer.GetString(),buffer.GetSize());
  }
  static int text(common::sqlclient::ObMySQLResult &result, const char *name, String &value) {
    common::ObString s; int ret=result.get_varchar(name,s);
    if (ret==common::OB_SUCCESS) value.assign(s.ptr(),s.length());
    return ret;
  }
  static int jobs(common::ObISQLClient &client, const String &where, std::vector<Task> &tasks, bool lock=false) {
    using namespace common;
    int ret=OB_SUCCESS;
    ObMySQLProxy::MySQLResult result;
    const String query="SELECT t.table_id,t.column_id,t.definition_version,t.request_id,t.rowkey_predicate,t.state,t.retry_count,t.max_retries,t.claim_epoch,"
        "t.input_payload,COALESCE(t.output_payload,'') AS output_payload,COALESCE(a.result_spec,'') AS result_spec,"
        "COALESCE(a.definition_version,0) AS current_version FROM oceanbase.__all_ai_column_task t LEFT JOIN oceanbase.__all_ai_column a "
        "ON a.table_id=t.table_id AND a.column_id=t.column_id WHERE "+where+" ORDER BY t.column_id"+(lock?" FOR UPDATE":"");
    if (OB_FAIL(client.read(result,query.c_str()))) return ret;
    auto *r=result.get_result(); if (r==nullptr) return OB_ERR_UNEXPECTED;
    while (OB_SUCC(ret) && OB_SUCC(ret=r->next())) {
      Task t;
      if (tasks.size()>=1024) return OB_SIZE_OVERFLOW;
      if (OB_FAIL(r->get_int("table_id",t.table)) || OB_FAIL(r->get_int("column_id",t.column)) ||
          OB_FAIL(r->get_int("definition_version",t.version)) || OB_FAIL(r->get_int("current_version",t.current_version)) ||
          OB_FAIL(r->get_int("state",t.state)) || OB_FAIL(r->get_int("retry_count",t.retry)) ||
          OB_FAIL(r->get_int("max_retries",t.retries)) || OB_FAIL(r->get_int("claim_epoch",t.epoch)) ||
          OB_FAIL(text(*r,"request_id",t.request)) || OB_FAIL(text(*r,"rowkey_predicate",t.key)) ||
          OB_FAIL(text(*r,"input_payload",t.input)) || OB_FAIL(text(*r,"output_payload",t.output)) ||
          OB_FAIL(text(*r,"result_spec",t.spec))) break;
      if (t.input.size()>1024*1024 || t.output.size()>1024*1024 || t.spec.size()>131072) return OB_SIZE_OVERFLOW;
      tasks.push_back(std::move(t));
    }
    return ret==OB_ITER_END?OB_SUCCESS:ret;
  }
  static int claim(const String &owner, int64_t &table, String &request, std::vector<Task> &tasks) {
    using namespace common;
    int ret=OB_SUCCESS; ObMySQLTransaction tx;
    if (OB_FAIL(tx.start(GCTX.sql_proxy_))) return ret;
    {
      ObMySQLProxy::MySQLResult result;
      const char *query="SELECT table_id,request_id FROM oceanbase.__all_ai_column_task WHERE "
          "((state IN (0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND lease_until<NOW(6))) "
          "ORDER BY eligible_ts,table_id,request_id,column_id LIMIT 1 FOR UPDATE";
      if (OB_FAIL(tx.read(result,query))) {
      } else if (result.get_result()==nullptr) { ret=OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result.get_result()->next())) {
      } else if (OB_FAIL(result.get_result()->get_int("table_id",table))) {
      } else { ret=text(*result.get_result(),"request_id",request); }
    }
    if (OB_SUCC(ret)) {
      int64_t affected=0;
      const String query="UPDATE oceanbase.__all_ai_column_task SET state=1,claim_owner="+literal(owner)+
          ",claim_epoch=claim_epoch+1,lease_until=DATE_ADD(NOW(6),INTERVAL 120 SECOND) WHERE "+group(table,request)+
          " AND ((state IN (0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND lease_until<NOW(6)))";
      if (OB_FAIL(tx.write(query.c_str(),affected))) {
      } else { ret=jobs(tx,"t.table_id="+number(table)+" AND t.request_id="+literal(request)+" AND t.state=1 AND t.claim_owner="+literal(owner),tasks); }
    }
    const int end=tx.end(OB_SUCC(ret));
    return OB_SUCC(ret)?end:ret;
  }
  static int normalize(const Task &task, const String &output, const share::schema::ObColumnSchemaV2 &column, String &value) {
    using namespace common;
    if (output.size()>1024*1024) return OB_SIZE_OVERFLOW;
    rapidjson::Document spec; spec.Parse(task.spec.c_str());
    if (spec.HasParseError() || !spec.IsObject() || !spec.HasMember("type") || !spec["type"].IsInt()) return OB_INVALID_DATA;
    const int type=spec["type"].GetInt();
    if (type==0) {
      {
        // SQL VARCHAR length is in characters; validate the returned UTF-8 first.
        int64_t chars=0;
        for (size_t i = 0; i < output.size();) {
          const unsigned char c = output[i++];
          uint32_t value = c; int extra = 0;
          if (c < 0x80) { ++chars; continue; }
          if (c >= 0xc2 && c <= 0xdf) { value = c & 31; extra = 1; }
          else if (c >= 0xe0 && c <= 0xef) { value = c & 15; extra = 2; }
          else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; extra = 3; }
          else return OB_INVALID_DATA;
          if (i + extra > output.size()) return OB_INVALID_DATA;
          for (int j = 0; j < extra; ++j) {
            unsigned char next = output[i++];
            if ((next & 0xc0) != 0x80) return OB_INVALID_DATA;
            value = (value << 6) | (next & 63);
          }
          if ((extra == 2 && value < 0x800) || (extra == 3 && value < 0x10000) ||
              value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return OB_INVALID_DATA;
          ++chars;
        }
        if (!is_lob_storage(column.get_data_type()) && chars > column.get_data_length()) return OB_SIZE_OVERFLOW;
        ObArenaAllocator conversion_allocator;
        ObString encoded, round_trip;
        ObString original(output.size(), output.data());
        int converted = ObCharset::charset_convert(conversion_allocator, original, CS_TYPE_UTF8MB4_BIN,
            column.get_collation_type(), encoded);
        if (converted != OB_SUCCESS) return converted;
        converted = ObCharset::charset_convert(conversion_allocator, encoded, column.get_collation_type(),
            CS_TYPE_UTF8MB4_BIN, round_trip);
        if (converted != OB_SUCCESS) return converted;
        if (round_trip.length() != output.size() || (round_trip.length() > 0 && MEMCMP(round_trip.ptr(), output.data(), output.size()) != 0)) return OB_INVALID_DATA;
        if ((column.get_data_type() == ObTinyTextType && encoded.length() > 255) ||
            (column.get_data_type() == ObTextType && encoded.length() > 65535)) return OB_SIZE_OVERFLOW;
      }
      value=literal(output); return OB_SUCCESS;
    }
    String trimmed=output;
    const auto first=trimmed.find_first_not_of(" \t\r\n"), last=trimmed.find_last_not_of(" \t\r\n");
    if (first==String::npos) return OB_INVALID_DATA;
    trimmed=trimmed.substr(first,last-first+1);
    if (type==2) {
      int64_t n=0; auto parsed=std::from_chars(trimmed.data(),trimmed.data()+trimmed.size(),n);
      if (parsed.ec!=std::errc() || parsed.ptr!=trimmed.data()+trimmed.size()) return OB_INVALID_DATA;
      int bits=64;
      switch(column.get_data_type()) {
        case ObTinyIntType: case ObUTinyIntType: bits=8; break;
        case ObSmallIntType: case ObUSmallIntType: bits=16; break;
        case ObMediumIntType: case ObUMediumIntType: bits=24; break;
        case ObInt32Type: case ObUInt32Type: bits=32; break;
        default: break;
      }
      if (ob_is_unsigned_type(column.get_data_type())) {
        if (n<0 || (bits<64 && static_cast<uint64_t>(n)>((uint64_t(1)<<bits)-1))) return OB_DATA_OUT_OF_RANGE;
      } else if (bits<64 && (n<-(int64_t(1)<<(bits-1)) || n>((int64_t(1)<<(bits-1))-1))) return OB_DATA_OUT_OF_RANGE;
      value=number(n); return OB_SUCCESS;
    }
    if (type==4) {
      if (trimmed!="true" && trimmed!="false") return OB_INVALID_DATA;
      value=trimmed=="true"?"1":"0"; return OB_SUCCESS;
    }
    rapidjson::Document parsed; parsed.Parse<rapidjson::kParseIterativeFlag|rapidjson::kParseValidateEncodingFlag>(trimmed.data(), trimmed.size());
    if (parsed.HasParseError()) return OB_INVALID_DATA;
    if (type==1) {
      // Bound nesting before passing the result to the native JSON cast.
      std::vector<std::pair<const rapidjson::Value *,int>> stack{{&parsed,0}};
      while(!stack.empty()) {
        auto [v,depth]=stack.back(); stack.pop_back(); if(depth>128) return OB_SIZE_OVERFLOW;
        if(v->IsArray()) for(const auto &x:v->GetArray()) stack.push_back({&x,depth+1});
        if(v->IsObject()) for(const auto &x:v->GetObject()) stack.push_back({&x.value,depth+1});
      }
      value="CAST("+literal(trimmed)+" AS JSON)"; return OB_SUCCESS;
    }
    if(type==3) {
      if(!parsed.IsNumber() || !std::isfinite(parsed.GetDouble())) return OB_INVALID_DATA;
      if(ob_is_unsigned_type(column.get_data_type()) && parsed.GetDouble()<0) return OB_DATA_OUT_OF_RANGE;
      if(ob_is_float_tc(column.get_data_type()) && std::fabs(parsed.GetDouble())>std::numeric_limits<float>::max()) return OB_DATA_OUT_OF_RANGE;
      value=trimmed; return OB_SUCCESS;
    }
    if(type==5) {
      if(!spec.HasMember("dimension") || !spec["dimension"].IsInt64() || !parsed.IsArray() || parsed.Size()!=spec["dimension"].GetInt64()) return OB_ERR_INVALID_VECTOR_DIM;
      for(const auto &x:parsed.GetArray()) if(!x.IsNumber() || !std::isfinite(x.GetDouble()) || std::fabs(x.GetDouble())>std::numeric_limits<float>::max()) return OB_INVALID_DATA;
      value=trimmed; return OB_SUCCESS;
    }
    return OB_INVALID_DATA;
  }
  static int infer(const Task &task, String &output) {
    using namespace common; using namespace sql;
    int ret=OB_SUCCESS;
    if(task.version!=task.current_version || task.spec.empty()) return OB_SCHEMA_ERROR;
    rapidjson::Document payload, spec; payload.Parse<rapidjson::kParseIterativeFlag>(task.input.c_str()); spec.Parse(task.spec.c_str());
    if(payload.HasParseError() || !payload.IsArray() || payload.Size()<2 || payload.Size()>3 || !payload[0].IsString() ||
       spec.HasParseError() || !spec.IsObject() || !spec.HasMember("type") || !spec["type"].IsInt() ||
       !spec.HasMember("prompt") || !spec["prompt"].IsString()) return OB_INVALID_DATA;
    ObArenaAllocator alloc;
    String content_storage,config_storage;
    ObString content;
    if(payload[1].IsString()) {
      content.assign_ptr(payload[1].GetString(),payload[1].GetStringLength());
    } else if(payload[1].IsObject()) {
      content_storage=json(payload[1]); ObString encoded(content_storage.size(),content_storage.data()); ObJsonObject *object=nullptr;
      if(OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(alloc,encoded,object))) return ret;
      if(!ObAIFuncPromptObjectUtils::is_valid_prompt_object(object)) return OB_INVALID_DATA;
      if(OB_FAIL(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc,object,content))) return ret;
    } else return OB_INVALID_ARGUMENT;
    if(content.empty()) return OB_INVALID_ARGUMENT;
    ObJsonObject *config=nullptr;
    const bool embedding=spec["type"].GetInt()==5;
    if(payload.Size()==3 && !payload[2].IsNull()) {
      if(embedding) {
        if(!payload[2].IsInt64() || payload[2].GetInt64()<=0) return OB_INVALID_ARGUMENT;
        config_storage="{\"dimensions\":"+number(payload[2].GetInt64())+"}";
      } else {
        rapidjson::Document config_document;
        if (payload[2].IsString()) {
          config_document.Parse<rapidjson::kParseIterativeFlag>(payload[2].GetString(), payload[2].GetStringLength());
          if (config_document.HasParseError() || !config_document.IsObject()) return OB_INVALID_ARGUMENT;
        } else if (payload[2].IsObject()) {
          config_document.CopyFrom(payload[2], config_document.GetAllocator());
        } else return OB_INVALID_ARGUMENT;
        for(const char *key:{"messages","model","input","prompt","system"}) if(config_document.HasMember(key)) return OB_INVALID_ARGUMENT;
        config_storage=json(config_document);
      }
      ObString encoded(config_storage.size(),config_storage.data());
      if(OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(alloc,encoded,config))) return ret;
    }
    ObString model_name(payload[0].GetStringLength(),payload[0].GetString());
    ObAIFuncExprInfo *info=nullptr; share::ObAiModelEndpointInfo endpoint;
    if(OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc,model_name,info))) return ret;
    if(OB_FAIL(share::ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(alloc,model_name,endpoint))) return ret;
    if(info==nullptr) return OB_ERR_UNEXPECTED;
    ObAIFuncModel model(alloc,*info,endpoint); ObString result;
    ObString prompt(spec["prompt"].GetStringLength(),spec["prompt"].GetString());
    if(embedding) ret=model.call_dense_embedding(content,config,result);
    else ret=model.call_completion(content,config,result,prompt);
    if(OB_FAIL(ret)) return ret;
    if(result.length()>1024*1024) return OB_SIZE_OVERFLOW;
    output.assign(result.ptr(),result.length());
    share::schema::ObSchemaGetterGuard guard; const share::schema::ObTableSchema *table=nullptr;
    if(OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard)) || OB_FAIL(guard.get_table_schema(task.table,table))) return ret;
    if(table==nullptr || table->get_column_schema(task.column)==nullptr) return OB_SCHEMA_ERROR;
    String ignored; return normalize(task,output,*table->get_column_schema(task.column),ignored);
  }
  static int record(const Task &task,const String &owner,int error,const String &output) {
    using namespace common;
    const bool retry=error!=OB_SUCCESS && task.retry<task.retries;
    const int state=error==OB_SUCCESS?2:(retry?3:4);
    const String query="UPDATE oceanbase.__all_ai_column_task SET state="+number(state)+",retry_count="+number(task.retry+(error!=OB_SUCCESS))+
        ",output_payload="+literal(error==OB_SUCCESS?output:String())+",error_code="+number(error)+
        ",error_message="+literal(error==OB_SUCCESS?String():String(ob_strerror(error)))+
        ",eligible_ts=DATE_ADD(NOW(6),INTERVAL "+number(retry?std::min<int64_t>(30,task.retry+1):0)+" SECOND),lease_until=NULL WHERE "+
        group(task.table,task.request)+" AND column_id="+number(task.column)+" AND state=1 AND claim_owner="+literal(owner)+
        " AND claim_epoch="+number(task.epoch)+" AND definition_version="+number(task.version);
    int64_t affected=0; return GCTX.sql_proxy_->write(query.c_str(),affected);
  }
  static int publish(int64_t table_id,const String &request) {
    using namespace common; using namespace sql; using namespace share::schema;
    int ret=OB_SUCCESS; ObMySQLTransaction tx;
    if(OB_FAIL(tx.start(GCTX.sql_proxy_))) return ret;
    {
      AiColumnInternalAccess access(query::ObInnerSQLConnectionAccess::get_session(tx.get_connection()));
      const String predicate = "t.table_id="+number(table_id)+" AND t.request_id="+literal(request);
      // Match business DML's lock order: business row first, task rows second.
      // In particular, never hold task/definition locks while waiting for DROP
      // TABLE or a dependency update to release the business table.
      std::vector<Task> observed, tasks;
      ret = jobs(tx, predicate, observed);
      if (OB_SUCC(ret) && !observed.empty()) {
        ObSchemaGetterGuard guard;
        const ObTableSchema *table = nullptr;
        const ObDatabaseSchema *database = nullptr;
        if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard)) ||
            OB_FAIL(guard.get_table_schema(table_id, table))) {
        } else if (table != nullptr && OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
        } else if (table != nullptr && database != nullptr) {
          const String lock_row = "SELECT /*+ USE_PLAN_CACHE(NONE) */ 1 FROM " +
              identifier(database->get_database_name_str()) + "." + identifier(table->get_table_name_str()) +
              " WHERE " + observed[0].key + " FOR UPDATE";
          ObMySQLProxy::MySQLResult result;
          if (OB_FAIL(tx.read(result, lock_row.c_str()))) {
          } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
          } else {
            ret = result.get_result()->next();
            if (ret == OB_ITER_END) ret = OB_SUCCESS;
          }
        }
      }
      // Re-read after obtaining the row lock: an update may have cancelled only
      // some columns in this group while this worker was waiting.
      if (OB_SUCC(ret)) ret=jobs(tx,predicate,tasks,true);
      bool ready=!tasks.empty(); for(const auto &task:tasks) ready &= task.state==2 || task.state==4;
      if(OB_SUCC(ret) && ready) {
        ObSchemaGetterGuard guard; const ObTableSchema *table=nullptr; const ObDatabaseSchema *database=nullptr;
        if(OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard)) || OB_FAIL(guard.get_table_schema(table_id,table))) {
        } else if(table!=nullptr && OB_FAIL(guard.get_database_schema(table->get_database_id(),database))) {
        } else {
          String assignments,conditions;
          if(table!=nullptr && database!=nullptr) {
            for(const auto &task:tasks) {
              auto *column=table->get_column_schema(task.column);
              if(column==nullptr || !column->is_ai_column() || task.version!=task.current_version) continue;
              rapidjson::Document spec; spec.Parse(task.spec.c_str());
              if(spec.HasParseError() || !spec.IsObject() || !spec.HasMember("state_column_id") || !spec["state_column_id"].IsUint64() ||
                  !spec.HasMember("request_column_id") || !spec["request_column_id"].IsUint64()) { ret=OB_INVALID_DATA; break; }
              auto *state=table->get_column_schema(spec["state_column_id"].GetUint64());
              auto *token=table->get_column_schema(spec["request_column_id"].GetUint64());
              if(state==nullptr || token==nullptr) { ret=OB_SCHEMA_ERROR; break; }
              String value="NULL"; bool success=task.state==2;
              if(success && normalize(task,task.output,*column,value)!=OB_SUCCESS) { success=false; value="NULL"; }
              if(!assignments.empty()) assignments+=",";
              assignments+=identifier(column->get_column_name_str())+"="+value+","+identifier(state->get_column_name_str())+"="+(success?"1":"2");
              conditions+=" AND "+identifier(token->get_column_name_str())+"="+literal(request);
            }
            if(OB_SUCC(ret) && !assignments.empty()) {
              const String query="UPDATE /*+ USE_PLAN_CACHE(NONE) */ "+identifier(database->get_database_name_str())+"."+identifier(table->get_table_name_str())+
                  " SET "+assignments+" WHERE "+tasks[0].key+conditions;
              int64_t affected=0; ret=tx.write(query.c_str(),affected);
            }
          }
          if(OB_SUCC(ret)) {
            int64_t affected=0; const String cleanup="DELETE FROM oceanbase.__all_ai_column_task WHERE "+group(table_id,request);
            ret=tx.write(cleanup.c_str(),affected);
          }
        }
      }
    }
    const int end=tx.end(OB_SUCC(ret)); return OB_SUCC(ret)?end:ret;
  }
  static int recover_completed() {
    using namespace common;
    int ret=OB_SUCCESS; int64_t table=0; String request;
    {
      ObMySQLProxy::MySQLResult result;
      const char *query="SELECT table_id,request_id FROM oceanbase.__all_ai_column_task GROUP BY table_id,request_id HAVING MIN(state IN (2,4))=1 LIMIT 1";
      if(OB_FAIL(GCTX.sql_proxy_->read(result,query))) return ret;
      if(result.get_result()==nullptr) return OB_ERR_UNEXPECTED;
      if(OB_FAIL(result.get_result()->next()) || OB_FAIL(result.get_result()->get_int("table_id",table)) ||
          OB_FAIL(text(*result.get_result(),"request_id",request))) return ret;
    }
    return publish(table,request);
  }
  std::mutex backfill_mutex_;
  int64_t backfill_ts_ = 0;
  int64_t backfill_table_ = 0, backfill_column_ = 0;
  void backfill_definitions() {
    using namespace common;
    std::unique_lock<std::mutex> lock(backfill_mutex_, std::try_to_lock);
    const int64_t now = ObTimeUtility::current_time();
    if (!lock.owns_lock() || now - backfill_ts_ < 1000000) return;
    backfill_ts_ = now;
    int64_t table_id = 0, column_id = 0;
    {
      ObMySQLProxy::MySQLResult result;
      const String query = "SELECT table_id,column_id FROM oceanbase.__all_ai_column WHERE table_id>" + number(backfill_table_) +
          " OR (table_id=" + number(backfill_table_) + " AND column_id>" + number(backfill_column_) + ") ORDER BY table_id,column_id LIMIT 1";
      if (GCTX.sql_proxy_->read(result, query.c_str()) != OB_SUCCESS || result.get_result() == nullptr) return;
      const int next = result.get_result()->next();
      if (next == OB_ITER_END) { backfill_table_ = backfill_column_ = 0; return; }
      if (next != OB_SUCCESS || result.get_result()->get_int("table_id", table_id) != OB_SUCCESS ||
          result.get_result()->get_int("column_id", column_id) != OB_SUCCESS) return;
    }
    backfill_table_ = table_id; backfill_column_ = column_id;
    share::schema::ObSchemaGetterGuard guard;
    const share::schema::ObTableSchema *table = nullptr;
    const share::schema::ObDatabaseSchema *database = nullptr;
    if (GCTX.schema_service_->get_runtime_schema_guard(guard) != OB_SUCCESS ||
        guard.get_table_schema(table_id, table) != OB_SUCCESS || table == nullptr ||
        guard.get_database_schema(table->get_database_id(), database) != OB_SUCCESS || database == nullptr) return;
    const auto *column = table->get_column_schema(column_id);
    if (column == nullptr || !column->is_ai_column()) return;
    rapidjson::Document info;
    info.Parse(column->get_ai_column_info().ptr(), column->get_ai_column_info().length());
    if (info.HasParseError() || !info.HasMember("request_column_id")) return;
    const auto *token = table->get_column_schema(info["request_column_id"].GetUint64());
    if (token == nullptr) return;
    const String qualified = identifier(database->get_database_name_str()) + "." + identifier(table->get_table_name_str());
    String key_expression;
    for (int64_t i = 0; i < table->get_column_count(); ++i) {
      const auto *key = table->get_column_schema_by_idx(i);
      if (key == nullptr || !key->is_rowkey_column()) continue;
      if (!key_expression.empty()) key_expression += "," + literal(" AND ") + ",";
      const String name = identifier(key->get_column_name_str());
      key_expression += literal(name + " = CONVERT(X'") + ",HEX(CAST(" + name + " AS CHAR CHARACTER SET utf8mb4))," + literal("' USING utf8mb4)");
    }
    if (key_expression.empty()) return;
    std::vector<String> keys;
    {
      ObMySQLProxy::MySQLResult result;
      const String query = "SELECT /*+ USE_PLAN_CACHE(NONE) */ CONCAT(" + key_expression + ") AS row_key FROM " + qualified +
          " WHERE " + identifier(token->get_column_name_str()) + " IS NULL LIMIT 64";
      if (GCTX.sql_proxy_->read(result, query.c_str()) != OB_SUCCESS || result.get_result() == nullptr) return;
      while (result.get_result()->next() == OB_SUCCESS) {
        String key; if (text(*result.get_result(), "row_key", key) != OB_SUCCESS) return;
        keys.push_back(std::move(key));
      }
    }
    for (const String &key : keys) {
      if (has_set_stop()) break;
      // RETRY uses native DML: concurrent writes/deletes are serialized and the
      // NULL-token predicate is rechecked under the business row lock.
      const String query = "ALTER TABLE " + qualified + " RETRY AI COLUMN " + identifier(column->get_column_name_str()) +
          " WHERE " + identifier(token->get_column_name_str()) + " IS NULL AND " + key;
      int64_t affected = 0;
      GCTX.sql_proxy_->write(query.c_str(), affected);
    }
  }
public:
  AiColumnWorker():lib::ThreadPool(4) {}
  void run1() override {
    using namespace common;
    int64_t iterations=0;
    while(!has_set_stop()) {
      try {
        if(GCTX.sql_proxy_!=nullptr && GCTX.schema_service_!=nullptr && recover_after_restart()) {
          backfill_definitions();
          char uuid[37]={};
          if(sql::ObExprUuid::gen_server_uuid(uuid,36)==OB_SUCCESS) {
            int64_t table=0; String request; std::vector<Task> tasks;
            const String owner(uuid,36);
            const int claimed=claim(owner,table,request,tasks);
            if(claimed==OB_SUCCESS) {
              for(const auto &task:tasks) {
                if(has_set_stop()) break;
                String output; const int error=infer(task,output);
                record(task,owner,error,output);
                // Keep the rest of this group leased while sequential requests run.
                int64_t affected=0;
                const String heartbeat="UPDATE oceanbase.__all_ai_column_task SET lease_until=DATE_ADD(NOW(6),INTERVAL 120 SECOND) WHERE "+
                    group(table,request)+" AND state=1 AND claim_owner="+literal(owner);
                GCTX.sql_proxy_->write(heartbeat.c_str(),affected);
              }
              publish(table,request);
            }
            if(claimed==OB_ITER_END || ++iterations%8==0) recover_completed();
          }
        }
      } catch (...) {
        // A failed claimant leaves a bounded lease, so another worker can recover.
        LOG_WARN_RET(OB_ERR_UNEXPECTED, "AI column worker iteration failed");
      }
      usleep(100000);
    }
  }
};
}} // namespace oceanbase::omt
