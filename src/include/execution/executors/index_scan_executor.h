//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_scan_executor.h
//
// Identification: src/include/execution/executors/index_scan_executor.h
//
// Copyright (c) 2015-20, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

#include "common/rid.h"
#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/index_scan_plan.h"
#include "storage/index/b_plus_tree_index.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * IndexScanExecutor executes an index scan over a table.
 */

class IndexScanExecutor : public AbstractExecutor {
 public:
  /**
   * Creates a new index scan executor.
   * @param exec_ctx the executor context
   * @param plan the index scan plan to be executed
   */
  IndexScanExecutor(ExecutorContext *exec_ctx, const IndexScanPlanNode *plan);

  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); }

  void Init() override;

  auto Next(Tuple *tuple, RID *rid) -> bool override;

 private:
  /** The index scan plan node to be executed. */
  const IndexScanPlanNode *plan_;

  /** Catalog 中的索引元数据，生命周期由 Catalog 管理 */
  IndexInfo *index_info_{nullptr};

  /** 索引所属表的元数据，生命周期由 Catalog 管理 */
  TableInfo *table_info_{nullptr};

  /** 具体的 B+ 树索引对象；Catalog 仍然拥有它 */
  BPlusTreeIndexForTwoIntegerColumn *tree_{nullptr};

  /** 当前 B+ 树叶子扫描位置；迭代器内部通过 PageGuard 管理页面 */
  BPlusTreeIndexIteratorForTwoIntegerColumn iterator_;
};
}  // namespace bustub
