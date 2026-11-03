// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0
// Line-oriented driver for the real renderer; see run_ai_prompt_renderer.py.
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "lib/allocator/page_arena.h"
#include <iostream>
#include <string>

using namespace oceanbase::common;

int main()
{
  std::string line;
  while (std::getline(std::cin, line)) {
    ObArenaAllocator allocator;
    ObJsonObject *object = nullptr;
    ObString input(static_cast<int32_t>(line.size()), line.data());
    ObString result;
    int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object);
    if (ret == 0) {
      ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object, result);
    }
    std::cout << ret << "\t";
    if (ret == 0) {
      const char *hex = "0123456789abcdef";
      for (int i = 0; i < result.length(); ++i) {
        const unsigned char c = result.ptr()[i];
        std::cout << hex[c >> 4] << hex[c & 15];
      }
    }
    std::cout << std::endl;
  }
}
