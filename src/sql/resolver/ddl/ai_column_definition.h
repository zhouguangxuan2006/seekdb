// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

#include "sql/parser/parse_node.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase { namespace sql {

// The definition is independent of transient parser allocations. Dependencies
// are resolved to schema column IDs by the caller after all columns are known.
struct AiColumnDefinition
{
  enum class ResultType { TEXT, JSON, INTEGER, REAL, BOOLEAN, VECTOR };
  enum class Error { NONE, EXPRESSION, OPTION, DEPENDENCY, DIMENSION, LIMIT };
  struct Column {
    std::string name;
    bool is_ai = false;
  };
  std::string expression;
  std::string model;
  std::vector<std::string> dependencies;
  std::string result_prompt;
  int64_t max_retries = 3;
  int64_t vector_dimension = 0;
  bool embedding = false;

  static std::string text(const ParseNode *node)
  {
    return node != nullptr && node->str_value_ != nullptr && node->str_len_ > 0
        ? std::string(node->str_value_, node->str_len_) : std::string();
  }
  static std::string lower(std::string value)
  {
    for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
  }
  static const ParseNode *child(const ParseNode *node, int index)
  {
    return node != nullptr && node->children_ != nullptr && index < node->num_child_
        ? node->children_[index] : nullptr;
  }
  static std::string function(const ParseNode *node)
  {
    return node != nullptr && node->type_ == T_FUN_SYS ? lower(text(child(node, 0))) : "";
  }
  static Error resolve(const ParseNode *definition, ResultType type, int64_t dimension,
                       const std::string &column_name, const std::vector<Column> &columns,
                       AiColumnDefinition &out)
  {
    AiColumnDefinition value;
    const ParseNode *expr = child(definition, 0);
    if (definition == nullptr || definition->type_ != T_AI_COLUMN_DEFINITION || expr == nullptr) {
      return Error::EXPRESSION;
    }
    value.expression = text(expr);
    if (value.expression.size() > 65536) return Error::LIMIT;
    const std::string name = function(expr);
    if (name != "ai_complete" && name != "ai_embed") return Error::EXPRESSION;
    value.embedding = name == "ai_embed";
    const ParseNode *args = child(expr, 1);
    if (args == nullptr || args->type_ != T_EXPR_LIST || args->num_child_ < 2 || args->num_child_ > 3) {
      return Error::EXPRESSION;
    }
    const ParseNode *model = child(args, 0);
    if (model == nullptr || model->type_ != T_VARCHAR || text(model).empty()) return Error::EXPRESSION;
    value.model = text(model);
    if (value.embedding != (type == ResultType::VECTOR)) return Error::EXPRESSION;
    if (value.embedding) {
      if (dimension < 1 || dimension > INT32_MAX) return Error::DIMENSION;
      value.vector_dimension = dimension;
      if (args->num_child_ == 3) {
        const ParseNode *dim = child(args, 2);
        if (dim == nullptr || dim->type_ != T_INT || dim->value_ != dimension) return Error::DIMENSION;
      }
    }
    const ParseNode *options = child(definition, 1);
    bool seen_mode = false;
    bool seen_retries = false;
    if (options != nullptr) {
      if (options->type_ != T_EXPR_LIST) return Error::OPTION;
      for (int i = 0; i < options->num_child_; ++i) {
        const ParseNode *option = child(options, i);
        const std::string key = lower(text(child(option, 0)));
        const ParseNode *option_value = child(option, 1);
        if (option == nullptr || option->type_ != T_OP_EQ || option_value == nullptr) return Error::OPTION;
        if (key == "sync_mode") {
          if (seen_mode || option_value->type_ != T_VARCHAR || lower(text(option_value)) != "async") {
            return Error::OPTION;
          }
          seen_mode = true;
        } else if (key == "max_retries") {
          if (seen_retries || option_value->type_ != T_INT || option_value->value_ < 0 || option_value->value_ > 32) {
            return Error::OPTION;
          }
          seen_retries = true;
          value.max_retries = option_value->value_;
        } else {
          return Error::OPTION;
        }
      }
    }
    Error error = collect(child(args, 1), column_name, columns, value.dependencies, 0);
    if (error != Error::NONE) return error;
    // Completion configuration must not introduce hidden dependencies or model calls.
    if (!value.embedding && args->num_child_ == 3) {
      error = collect(child(args, 2), column_name, columns, value.dependencies, 0);
      if (error != Error::NONE) return error;
    }
    switch (type) {
      case ResultType::JSON:
        value.result_prompt = "Return exactly one valid JSON value. Do not include markdown or explanations.";
        break;
      case ResultType::INTEGER:
        value.result_prompt = "Return exactly one base-10 integer in the signed 64-bit range. Do not include explanations.";
        break;
      case ResultType::REAL:
        value.result_prompt = "Return exactly one finite number. Do not include explanations.";
        break;
      case ResultType::BOOLEAN:
        value.result_prompt = "Return exactly true or false. Do not include explanations.";
        break;
      case ResultType::TEXT:
      case ResultType::VECTOR:
        break;
    }
    out = std::move(value);
    return Error::NONE;
  }
private:
  static Error collect(const ParseNode *node, const std::string &column_name,
                       const std::vector<Column> &columns, std::vector<std::string> &dependencies,
                       int depth)
  {
    if (node == nullptr) return Error::NONE;
    if (depth > 128) return Error::LIMIT;
    if (node->type_ == T_COLUMN_REF) {
      if (child(node, 0) != nullptr || child(node, 1) != nullptr || child(node, 2) == nullptr) {
        return Error::DEPENDENCY;
      }
      const std::string name = lower(text(child(node, 2)));
      if (name == lower(column_name)) return Error::DEPENDENCY;
      const auto found = std::find_if(columns.begin(), columns.end(), [&](const Column &c) {
        return lower(c.name) == name;
      });
      if (found == columns.end() || found->is_ai) return Error::DEPENDENCY;
      if (std::find(dependencies.begin(), dependencies.end(), found->name) == dependencies.end()) {
        dependencies.push_back(found->name);
      }
      return Error::NONE;
    }
    if (node->type_ == T_SELECT || node->type_ == T_SYSTEM_VARIABLE
        || node->type_ == T_USER_VARIABLE_IDENTIFIER || node->type_ == T_QUESTIONMARK || IS_AGGR_FUN(node->type_)) {
      return Error::EXPRESSION;
    }
    if (node->type_ > T_FUN_SYS && node->type_ < T_MAX_OP && node->type_ != T_FUN_SYS_CAST) {
      return Error::EXPRESSION;
    }
    if (node->type_ == T_FUN_SYS) {
      const std::string name = function(node);
      // Generated inputs must be deterministic, and cannot execute another model.
      static const char *const allowed[] = {"concat", "concat_ws", "ai_prompt", "coalesce", "ifnull",
          "lower", "upper", "trim", "ltrim", "rtrim", "substr", "substring", "replace",
          "json_object", "json_array", "json_extract", "json_unquote", "length", "char_length"};
      if (std::find_if(std::begin(allowed), std::end(allowed), [&](const char *s) { return name == s; })
          == std::end(allowed)) return Error::EXPRESSION;
    }
    for (int i = 0; i < node->num_child_; ++i) {
      const Error error = collect(child(node, i), column_name, columns, dependencies, depth + 1);
      if (error != Error::NONE) return error;
    }
    return Error::NONE;
  }
};

}} // namespace oceanbase::sql
