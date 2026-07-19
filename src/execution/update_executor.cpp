//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// update_executor.cpp
//
// Identification: src/execution/update_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#include <memory>

#include "execution/executors/update_executor.h"

namespace bustub {

UpdateExecutor::UpdateExecutor(ExecutorContext *exec_ctx, const UpdatePlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      table_info_(exec_ctx->GetCatalog()->GetTable(plan->TableOid())),
      child_executor_(std::move(child_executor)) {
  // UpdatePlan 中只保存目标表的 OID；Executor 运行时需要通过 Catalog 找到实际的 TableHeap 和 Schema。
  BUSTUB_ASSERT(table_info_ != nullptr, "Table does not exist");
}

void UpdateExecutor::Init() {
  // child 负责找出满足 WHERE 条件的旧 Tuple 及其 RID，每次执行前必须将 child 重置到起点。
  child_executor_->Init();

  // UpdateExecutor 会在第一次 Next() 中消费 child 的全部结果，并只输出一次更新数量。
  // Init() 代表开始一次新的执行，因此需要清除上一次执行留下的状态。
  executed_ = false;
}

auto UpdateExecutor::Next([[maybe_unused]] Tuple *tuple, RID *rid) -> bool {
  // UpdateExecutor 在第一次 Next() 中完成全部更新并返回一行更新数量。
  // 执行引擎还会继续调用 Next()，因此第二次及以后必须返回 false。
  if (executed_) {
    return false;
  }
  executed_ = true;

  int32_t update_count = 0;
  Tuple old_tuple;
  RID old_rid;

  // 一张表可能有多个索引。更新记录时，不仅要修改 TableHeap，还必须同步删除旧索引项、插入新索引项。
  const auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(table_info_->name_);

  // child 通常是 Filter + SeqScan，它会依次返回满足 WHERE 条件的旧 Tuple 以及旧记录的 RID。
  while (child_executor_->Next(&old_tuple, &old_rid)) {
    // UpdatePlan 为目标表的每一列准备了一个表达式：
    // 1. SET 中出现的列使用用户指定的表达式；
    // 2. 未修改的列使用 ColumnValueExpression 读取旧值。
    // 所有表达式都必须基于 old_tuple 计算，最终组合成一条完整的新记录。
    std::vector<Value> values;
    values.reserve(plan_->target_expressions_.size());
    for (const auto &expression : plan_->target_expressions_) {
      values.push_back(expression->Evaluate(&old_tuple, child_executor_->GetOutputSchema()));
    }
    Tuple new_tuple{values, &table_info_->schema_};

    // 当前实现采用“逻辑删除旧记录 + 插入新记录”，而不是原地覆盖。
    // 原地覆盖无法简单处理 VARCHAR 变长、记录放不回原 Page 等情况。
    table_info_->table_->UpdateTupleMeta(TupleMeta{INVALID_TXN_ID, INVALID_TXN_ID, true}, old_rid);

    // 旧记录已经失效，因此它在每个索引中的 old_key -> old_rid 映射也必须删除。
    // 即使索引列的值没有变化也要删除，因为新记录稍后会获得一个不同的 RID。
    for (auto *index_info : indexes) {
      auto old_key =
          old_tuple.KeyFromTuple(table_info_->schema_, index_info->key_schema_, index_info->index_->GetKeyAttrs());
      index_info->index_->DeleteEntry(old_key, old_rid, exec_ctx_->GetTransaction());
    }

    // 将计算得到的新 Tuple 插入 TableHeap，并取得它在目标表中的新物理位置。
    auto new_rid = table_info_->table_->InsertTuple(TupleMeta{INVALID_TXN_ID, INVALID_TXN_ID, false}, new_tuple);
    BUSTUB_ASSERT(new_rid.has_value(), "Failed to insert updated tuple");

    // 根据 new_tuple 重新生成每个索引的 Key，建立 new_key -> new_rid 映射。
    for (auto *index_info : indexes) {
      auto new_key =
          new_tuple.KeyFromTuple(table_info_->schema_, index_info->key_schema_, index_info->index_->GetKeyAttrs());
      index_info->index_->InsertEntry(new_key, new_rid.value(), exec_ctx_->GetTransaction());
    }

    // 旧记录删除、新记录插入、全部索引维护完毕后，才认为这一行更新成功。
    update_count++;
  }

  // UpdatePlan 的输出 Schema 只有一个 INTEGER 列，用于返回受影响的记录数。
  // 即使 WHERE 没有匹配任何记录，第一次调用也要返回 Tuple(0)。
  *tuple = Tuple{{ValueFactory::GetIntegerValue(update_count)}, &GetOutputSchema()};

  // Update 的计数结果不是 TableHeap 中的物理记录，因此输出参数 rid 不需要填写。
  return true;
}

}  // namespace bustub
