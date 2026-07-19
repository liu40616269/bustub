//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// topn_executor.h
//
// Identification: src/include/execution/executors/topn_executor.h
//
// Copyright (c) 2015-2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/topn_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * The TopNExecutor executor executes a topn.
 */
class TopNExecutor : public AbstractExecutor {
 public:
  /**
   * Construct a new TopNExecutor instance.
   * @param exec_ctx The executor context
   * @param plan The topn plan to be executed
   */
  TopNExecutor(ExecutorContext *exec_ctx, const TopNPlanNode *plan, std::unique_ptr<AbstractExecutor> &&child_executor);

  /** Initialize the topn */
  void Init() override;

  /**
   * Yield the next tuple from the topn.
   * @param[out] tuple The next tuple produced by the topn
   * @param[out] rid The next tuple RID produced by the topn
   * @return `true` if a tuple was produced, `false` if there are no more tuples
   */
  auto Next(Tuple *tuple, RID *rid) -> bool override;

  /** @return The output schema for the topn */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); }

  /** Sets new child executor (for testing only) */
  void SetChildExecutor(std::unique_ptr<AbstractExecutor> &&child_executor) {
    child_executor_ = std::move(child_executor);
  }

  /** @return The size of top_entries_ container, which will be called on each child_executor->Next(). */
  auto GetNumInHeap() -> size_t;

 private:
  /** 堆和最终输出都需要同时保存 Tuple 及 child 产生的 RID。 */
  struct TopNEntry {
    Tuple tuple_;
    RID rid_;
  };

  /**
   * 判断 left 是否应排在 right 前面。
   * priority_queue 使用该比较器后，堆顶会成为当前 Top N 中“最差”的记录。
   */
  class TopNComparator {
   public:
    explicit TopNComparator(const TopNPlanNode *plan) : plan_(plan) {}

    auto operator()(const TopNEntry &left, const TopNEntry &right) const -> bool;

   private:
    const TopNPlanNode *plan_;
  };

  using TopNHeap = std::priority_queue<TopNEntry, std::vector<TopNEntry>, TopNComparator>;

  /** The topn plan node to be executed */
  const TopNPlanNode *plan_;

  /** The child executor from which tuples are obtained */
  std::unique_ptr<AbstractExecutor> child_executor_;

  /** 始终只保留当前最优的 N 条记录，堆顶是其中最差的一条。 */
  TopNHeap top_entries_;

  /** 堆构建完成后，将其中记录整理成最终 ORDER BY 顺序供 Next() 输出。 */
  std::vector<TopNEntry> output_entries_;

  /** 下一次 Next() 应返回 output_entries_ 中的下标。 */
  std::size_t next_tuple_idx_{0};
};
}  // namespace bustub
