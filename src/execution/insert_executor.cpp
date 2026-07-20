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

#include "common/exception.h"
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
  auto *txn = exec_ctx_->GetTransaction();
  const auto table_oid = plan_->TableOid();

  // 插入一条记录前必须先在目标表上持有 IX（或更强的 SIX/X），这样后续为新 RID 获取行 X
  // 才符合多粒度锁协议。事务可能在前一条 SQL 中已经持有更强锁，此时不能重复请求较弱的 IX。
  const bool has_write_table_lock = txn->IsTableIntentionExclusiveLocked(table_oid) ||
                                    txn->IsTableSharedIntentionExclusiveLocked(table_oid) ||
                                    txn->IsTableExclusiveLocked(table_oid);
  if (!has_write_table_lock) {
    // 若事务已经持有整表 S，写意向应与 S 合并为 SIX；常规情况则直接请求 IX，或者把已有 IS 升级为 IX。
    const auto requested_mode = txn->IsTableSharedLocked(table_oid) ? LockManager::LockMode::SHARED_INTENTION_EXCLUSIVE
                                                                    : LockManager::LockMode::INTENTION_EXCLUSIVE;
    if (!exec_ctx_->GetLockManager()->LockTable(txn, requested_mode, table_oid)) {
      throw ExecutionException("InsertExecutor failed to acquire a table lock");
    }
  }

  // 表锁准备好以后，再从头初始化待插入数据的生产者。对于普通 INSERT，child 通常是 ValuesExecutor；
  // 对于 INSERT ... SELECT，child 也可能是一棵读取其他表的执行器树。
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
    // 先把完整 Tuple 写入目标表的 TableHeap。Task 3 需要额外传入 LockManager、Transaction 和表 OID：
    // TableHeap 在分配出新 RID 后会立刻为它申请行 X，从而让“记录出现”和“写事务拥有该行锁”关联起来。
    // 新行的 X 锁会一直保留到 Commit/Abort，其他 RR/RC 事务在读取它时必须等待本事务结束。
    auto inserted_rid =
        table_info_->table_->InsertTuple(TupleMeta{INVALID_TXN_ID, INVALID_TXN_ID, false}, child_tuple,
                                         exec_ctx_->GetLockManager(), exec_ctx_->GetTransaction(), plan_->TableOid());

    // 只有获得目标表中的新 RID 后，才能建立正确的“索引键 -> 目标记录位置”映射。
    BUSTUB_ASSERT(inserted_rid.has_value(), "Failed to insert tuple");

    // TableHeap 的写入立即生效，所以必须把新 RID 放进事务 write set。若事务之后因死锁或显式 ABORT
    // 失败，TransactionManager::Abort() 会逆序读取这些记录，把该新行重新标记为 deleted。
    // Spring 2023 基础任务的 TableWriteRecord 不需要记录 WType：插入和删除都通过反转 is_deleted_ 撤销。
    exec_ctx_->GetTransaction()->AppendTableWriteRecord(
        TableWriteRecord{plan_->TableOid(), inserted_rid.value(), table_info_->table_.get()});

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
