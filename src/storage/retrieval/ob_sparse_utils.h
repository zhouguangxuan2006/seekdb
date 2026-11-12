/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_SPARSE_UTILS_H_
#define OB_SPARSE_UTILS_H_

#include "ob_i_sparse_retrieval_iter.h"

namespace oceanbase
{
namespace sql
{
struct ObFtsEvalNode;
}
namespace storage
{

struct ObSRDaaTInnerProductRelevanceCollector;

struct ObSRDaaTRelevanceCollector
{
  ObSRDaaTRelevanceCollector() {}
  virtual ~ObSRDaaTRelevanceCollector() {}

  // Return a concrete view only for the final sum collector. Other collectors
  // keep the generic path, without relying on query mode or an unchecked cast.
  virtual ObSRDaaTInnerProductRelevanceCollector *as_inner_product() { return nullptr; }
  virtual void reset() = 0;
  virtual void reuse() = 0;
  virtual int collect_one_dim(const int64_t dim_idx, const double relevance) = 0;
  virtual int get_result(double &relevance, bool &is_valid) = 0;
};

struct ObSRDaaTInnerProductRelevanceCollector final : ObSRDaaTRelevanceCollector
{
  ObSRDaaTInnerProductRelevanceCollector() : ObSRDaaTRelevanceCollector(),
      total_relevance_(0),
      matched_cnt_(0),
      should_match_(0) {}
  virtual ~ObSRDaaTInnerProductRelevanceCollector() {};

  ObSRDaaTInnerProductRelevanceCollector *as_inner_product() override { return this; }
  int init(int64_t should_match = 0);
  virtual void reset() override;
  virtual void reuse() override;
  int collect_one_dim(const int64_t, const double relevance) override
  {
    total_relevance_ += relevance;
    ++matched_cnt_;
    return common::OB_SUCCESS;
  }
  int get_result(double &relevance, bool &is_valid) override
  {
    relevance = total_relevance_;
    is_valid = matched_cnt_ >= should_match_;
    total_relevance_ = 0.0;
    matched_cnt_ = 0;
    return common::OB_SUCCESS;
  }
private:
  double total_relevance_;
  int64_t matched_cnt_;
  int64_t should_match_;
};

struct ObSRDaaTBooleanRelevanceCollector : ObSRDaaTRelevanceCollector
{
  ObSRDaaTBooleanRelevanceCollector() : ObSRDaaTRelevanceCollector(),
      allocator_(nullptr), dim_cnt_(0), boolean_compute_node_(nullptr), boolean_relevances_() {}
  virtual ~ObSRDaaTBooleanRelevanceCollector() {};

  int init(ObIAllocator *allocator, const int64_t dim_cnt, sql::ObFtsEvalNode *node);
  virtual void reset() override;
  virtual void reuse() override;
  virtual int collect_one_dim(const int64_t dim_idx, const double) override;
  virtual int get_result(double &relevance, bool &is_valid) override;
private:
  ObIAllocator *allocator_;
  int64_t dim_cnt_;
  sql::ObFtsEvalNode *boolean_compute_node_;
  ObFixedArray<double, ObIAllocator> boolean_relevances_;
};

} // namespace storage
} // namespace oceanbase

#endif
