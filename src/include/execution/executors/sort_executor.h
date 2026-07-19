//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// sort_executor.h
//
// Identification: src/include/execution/executors/sort_executor.h
//
// Copyright (c) 2015-2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/sort_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * The SortExecutor executor executes a sort.
 */
class SortExecutor : public AbstractExecutor {
 public:
  /**
   * Construct a new SortExecutor instance.
   * @param exec_ctx The executor context
   * @param plan The sort plan to be executed
   */
  SortExecutor(ExecutorContext *exec_ctx, const SortPlanNode *plan, std::unique_ptr<AbstractExecutor> &&child_executor);

  /** Initialize the sort */
  void Init() override;

  /**
   * Yield the next tuple from the sort.
   * @param[out] tuple The next tuple produced by the sort
   * @param[out] rid The next tuple RID produced by the sort
   * @return `true` if a tuple was produced, `false` if there are no more tuples
   */
  auto Next(Tuple *tuple, RID *rid) -> bool override;

  /** @return The output schema for the sort */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); }

 private:
  /** 按 Plan 中的多列 ORDER BY 规则判断 left 是否应排在 right 前面。 */
  auto CompareTuples(const Tuple &left, const Tuple &right) const -> bool;

  /** The sort plan node to be executed */
  const SortPlanNode *plan_;

  /** Sort 的 child Executor；Init() 会一次性消费其全部输入。 */
  std::unique_ptr<AbstractExecutor> child_executor_;

  /** 排序时同时保存 Tuple 与 child 返回的 RID，Next() 再将二者原样向上传递。 */
  std::vector<std::pair<Tuple, RID>> sorted_entries_;

  /** 下一次 Next() 应返回 sorted_entries_ 中的下标。 */
  std::size_t next_tuple_idx_{0};
};
}  // namespace bustub
