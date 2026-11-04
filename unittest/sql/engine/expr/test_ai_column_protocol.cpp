// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "rapidjson/document.h"
#include "sql/engine/expr/ob_expr_ai/ai_column_internal.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace oceanbase::common;
using namespace oceanbase::sql;
static void require(bool ok, const char *why)
{
  if (!ok) { fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
int main()
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("test-model");
  ObString input("original user input: {\"text\":\"hello\"}\nsecond line");
  ObString instruction("Return exactly one valid JSON value. Do not include markdown or explanations.");
  for (bool typed : {false, true}) {
    ObString system = typed ? instruction : ObString();
    ObJsonObject *body = nullptr;
    require(provider.get_body(allocator, model, system, input, nullptr, body) == OB_SUCCESS,
            "construct native completion request");
    ObString wire;
    require(ObAIFuncJsonUtils::print_json_to_str(allocator, body, wire) == OB_SUCCESS,
            "serialize native completion request");
    rapidjson::Document parsed;
    parsed.Parse(wire.ptr(), wire.length());
    require(!parsed.HasParseError(), "request body is JSON");
    const auto &messages = parsed["messages"];
    require(messages.Size() == (typed ? 2U : 1U), "typed request has distinct system message");
    if (typed) {
      require(std::string(messages[0]["role"].GetString()) == "system", "type instruction role");
      require(std::string(messages[0]["content"].GetString()) == std::string(instruction.ptr(), instruction.length()),
              "type instruction unchanged");
    }
    const auto &user = messages[messages.Size()-1];
    require(std::string(user["role"].GetString()) == "user", "business input role");
    require(std::string(user["content"].GetString()) == std::string(input.ptr(), input.length()),
            "business input is unchanged, including escaping and newline");
  }
  for (const char *wire : {"{\"data\":7}", "{\"data\":{}}", "{\"data\":[null]}"}) {
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ObString encoded = ObString::make_string(wire);
    require(ObAIFuncJsonUtils::get_json_object_form_str(allocator, encoded, response) == OB_SUCCESS,
            "parse malformed embedding fixture");
    ObOpenAIUtils::ObOpenAIEmbed embed;
    require(embed.parse_output(allocator, response, result) != OB_SUCCESS,
            "reject malformed remote embedding without unsafe casts");
  }
  for (const char *wire : {"{\"output\":1}", "{\"output\":{\"choices\":42}}", "{\"output\":{\"choices\":[{\"message\":false}]}}"}) {
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ObString encoded = ObString::make_string(wire);
    require(ObAIFuncJsonUtils::get_json_object_form_str(allocator, encoded, response) == OB_SUCCESS,
            "parse malformed completion fixture");
    ObDashscopeUtils::ObDashscopeComplete complete;
    require(complete.parse_output(allocator, response, result) != OB_SUCCESS,
            "reject malformed completion without unsafe casts");
  }
  std::string capture;
  require(ai_column_capture_expression("(/* AI_COMPLETE is only a comment */ AI_COMPLETE('m',concat('AI_COMPLETE(', body)))", capture),
          "capture supports parenthesized root and comments");
  require(capture == "(/* AI_COMPLETE is only a comment */ JSON_ARRAY('m',concat('AI_COMPLETE(', body)))",
          "capture changes only root call, preserves business SQL");
  require(!ai_column_capture_expression("concat('m', body)", capture), "capture rejects a non-AI root");
  puts("PASS AI-column protocol: native provider preserves system/user separation and ordinary request shape");
}
