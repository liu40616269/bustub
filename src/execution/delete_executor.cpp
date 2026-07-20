//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// delete_executor.cpp
//
// Identification: src/execution/delete_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>

#include "execution/executors/delete_executor.h"
#include "type/value_factory.h"

namespace bustub {

DeleteExecutor::DeleteExecutor(ExecutorContext *exec_ctx, const DeletePlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      table_info_(exec_ctx->GetCatalog()->GetTable(plan->TableOid())),
      child_executor_(std::move(child_executor)) {
  // DeletePlan 只保存目标表 OID；执行时还需要通过 Catalog 找到实际的 TableHeap、Schema 和索引信息。
  BUSTUB_ASSERT(table_info_ != nullptr, "Table does not exist");
}

void DeleteExecutor::Init() {
  // child 通常由 SeqScan 和 Filter 组成，负责找出需要删除的旧 Tuple 及其 RID。
  child_executor_->Init();

  // DeleteExecutor 在第一次 Next() 中完成全部删除；Init() 开始新一轮执行时要重置该状态。
  executed_ = false;
}

auto DeleteExecutor::Next([[maybe_unused]] Tuple *tuple, RID *rid) -> bool {
  // 第一次 Next() 会消费 child 的全部结果并返回一次删除数量；后续调用表示已经到达 EOF。
  if (executed_) {
    return false;
  }
  executed_ = true;

  int32_t delete_count = 0;
  Tuple old_tuple;
  RID old_rid;

  // 一张表可能有零个或多个索引。删除 TableHeap 记录时，必须同步移除所有索引项。
  const auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(table_info_->name_);

  // child 只返回满足 DELETE 的 WHERE 条件的记录；没有 WHERE 时则返回表中的全部有效记录。
  while (child_executor_->Next(&old_tuple, &old_rid)) {
    // DeleteExecutor 不再重复申请锁：ExecutorContext::IsDelete() 会让下层 SeqScan 在返回 old_rid 前
    // 已经取得目标表 IX 和该行 X。这里直接执行逻辑删除，并且保留 TupleMeta 中除删除标记外的字段。
    // X 锁不会在此释放，它必须一直保留到 Commit/Abort，防止其他 RR/RC 事务看到未提交删除。
    auto old_meta = table_info_->table_->GetTupleMeta(old_rid);
    old_meta.is_deleted_ = true;
    table_info_->table_->UpdateTupleMeta(old_meta, old_rid);

    // 逻辑删除已经立即写入 TableHeap，因此同步记录到 write set。若事务中止，Abort() 会按相反顺序
    // 处理记录，并把当前 is_deleted_=true 反转回 false，使这条旧记录重新可见。
    exec_ctx_->GetTransaction()->AppendTableWriteRecord(
        TableWriteRecord{plan_->TableOid(), old_rid, table_info_->table_.get()});

    // 每个索引只保存“索引 Key -> RID”。记录被删除后，必须用删除前的完整 Tuple
    // 提取对应索引 Key，再删除 old_key -> old_rid 映射，避免 IndexScan 找到失效记录。
    for (auto *index_info : indexes) {
      auto old_key =
          old_tuple.KeyFromTuple(table_info_->schema_, index_info->key_schema_, index_info->index_->GetKeyAttrs());
      index_info->index_->DeleteEntry(old_key, old_rid, exec_ctx_->GetTransaction());
    }

    // TableHeap 和全部索引都处理完后，才将成功删除数量加一。
    delete_count++;
  }

  // DeletePlan 的输出 Schema 只有一个 INTEGER 列：__bustub_internal.delete_rows。
  // 即使没有任何记录符合条件，第一次 Next() 也应返回 Tuple(0)。
  *tuple = Tuple{{ValueFactory::GetIntegerValue(delete_count)}, &GetOutputSchema()};

  // 删除数量是计算结果，不对应 TableHeap 中的物理记录，因此输出参数 rid 不需要填写。
  return true;
}

}  // namespace bustub
