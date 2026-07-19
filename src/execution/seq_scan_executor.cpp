//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// seq_scan_executor.cpp
//
// Identification: src/execution/seq_scan_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/seq_scan_executor.h"

namespace bustub {

SeqScanExecutor::SeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan), table_info_(exec_ctx->GetCatalog()->GetTable(plan->GetTableOid())) {
  BUSTUB_ASSERT(table_info_ != nullptr, "Table does not exist");
}

void SeqScanExecutor::Init() { table_iter_ = std::make_unique<TableIterator>(table_info_->table_->MakeIterator()); }

auto SeqScanExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  while (!table_iter_->IsEnd()) {
    // 1.保存当前记录的RID
    const auto current_rid = table_iter_->GetRID();

    // 2.读取当前记录及其元数据
    auto [meta, current_tuple] = table_iter_->GetTuple();

    // 3.提前移动到下一条
    ++(*table_iter_);

    // 4.已经删除的记录不能返回
    if (meta.is_deleted_) {
      continue;
    }
    // 5.返回当前有效记录
    *tuple = std::move(current_tuple);
    *rid = current_rid;

    return true;
  }
  return false;
}

}  // namespace bustub
