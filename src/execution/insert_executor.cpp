//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// insert_executor.cpp
//
// Identification: src/execution/insert_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>

#include "execution/executors/insert_executor.h"
#include "type/value_factory.h"

namespace bustub {

InsertExecutor::InsertExecutor(ExecutorContext *exec_ctx, const InsertPlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      table_info_(exec_ctx->GetCatalog()->GetTable(plan->TableOid())),
      child_executor_(std::move(child_executor)) {
  BUSTUB_ASSERT(table_info_ != nullptr, "Table does not exist");
}

void InsertExecutor::Init() {
  // 从头初始化待插入数据的生产者
  child_executor_->Init();

  // 允许本轮执行产生一次插入结果
  executed_ = false;
}

auto InsertExecutor::Next([[maybe_unused]] Tuple *tuple, RID *rid) -> bool {
  // InsertExecutor 是一个“只产生一行结果”的执行器。第一次调用 Next() 时，它会消费 child
  // 的全部 Tuple、完成所有插入，并返回一行“成功插入的记录数”；执行引擎随后还会继续调用
  // Next()，因此必须用 executed_ 在第二次调用时返回 false，表示该执行器已经结束。
  if (executed_) {
    return false;
  }

  // 在产生任何写操作之前就标记为已执行，避免本轮插入逻辑被重复触发。
  // 如果以后对同一个 Executor 开始一次全新的执行，Init() 会重新把它设为 false。
  executed_ = true;

  // insert_count 用来生成 INSERT 语句最终返回的影响行数，例如插入 3 行就返回 Tuple(3)。
  int32_t insert_count = 0;

  // child_tuple 保存 child 当前产生的待插入记录。
  // child_rid 是这条输入记录在 child 中的 RID：对于 VALUES 它没有物理意义；对于
  // INSERT INTO t2 SELECT * FROM t1，它属于源表 t1，不能用作目标表 t2 的索引 RID。
  Tuple child_tuple;
  RID child_rid;

  // 一张表可能有零个或多个索引。提前从 Catalog 获取全部 IndexInfo，避免每插入一条记录
  // 都重复查询 Catalog。没有索引时 indexes 为空，下面的索引维护循环会自然跳过。
  const auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(table_info_->name_);

  // InsertExecutor 自己不负责计算 VALUES、WHERE 或 SELECT 表达式，而是不断向 child
  // 拉取已经计算完成的 Tuple，直到 child->Next() 返回 false。
  while (child_executor_->Next(&child_tuple, &child_rid)) {
    // 先把完整 Tuple 写入目标表的 TableHeap。
    // 两个 INVALID_TXN_ID 在 Project 3 中无需参与事务可见性判断；is_deleted_=false 表示
    // 这是一条正常、未删除的新记录。InsertTuple 返回它在目标表中的全新物理位置。
    auto inserted_rid = table_info_->table_->InsertTuple(TupleMeta{INVALID_TXN_ID, INVALID_TXN_ID, false}, child_tuple);

    // 只有获得目标表中的新 RID 后，才能建立正确的“索引键 -> 目标记录位置”映射。
    BUSTUB_ASSERT(inserted_rid.has_value(), "Failed to insert tuple");

    // TableHeap 只保存完整记录；每个索引还需要单独保存自己的 Key 和新 RID。
    // 因此，每成功插入一条记录，都必须同步更新目标表上的所有索引。
    for (auto *index_info : indexes) {
      // KeyFromTuple 根据索引的 key_attrs，从完整 child_tuple 中抽取被索引的列。
      // 例如 Tuple=(id=7, name="Alice") 且索引建在 id 上，得到的 key 就是 Tuple(7)。
      auto key =
          child_tuple.KeyFromTuple(table_info_->schema_, index_info->key_schema_, index_info->index_->GetKeyAttrs());

      // 这里必须使用 inserted_rid（目标表中新记录的 RID），而不是 child_rid（输入来源的 RID）。
      index_info->index_->InsertEntry(key, inserted_rid.value(), exec_ctx_->GetTransaction());
    }

    // 表记录和它的全部索引项都处理完毕后，才将成功插入数量加一。
    insert_count++;
  }

  // InsertPlan 的输出 Schema 只有一个 INTEGER 列：__bustub_internal.insert_rows。
  // 即使 child 一条记录都没有产生，也要在第一次 Next() 时返回 Tuple(0)。
  *tuple = Tuple{{ValueFactory::GetIntegerValue(insert_count)}, &GetOutputSchema()};

  // InsertExecutor 的输出没有对应的物理表记录，所以传入的 rid 参数不需要填写。
  return true;
}

}  // namespace bustub
