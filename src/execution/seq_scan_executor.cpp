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

#include "common/exception.h"

namespace bustub {

SeqScanExecutor::SeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan), table_info_(exec_ctx->GetCatalog()->GetTable(plan->GetTableOid())) {
  BUSTUB_ASSERT(table_info_ != nullptr, "Table does not exist");
}

void SeqScanExecutor::Init() {
  auto *txn = exec_ctx_->GetTransaction();
  auto *lock_manager = exec_ctx_->GetLockManager();
  const auto table_oid = plan_->GetTableOid();

  // SeqScan 不仅用于 SELECT，也会作为 DELETE/UPDATE 的子执行器寻找目标记录。
  // ExecutorContext::IsDelete() 在这两类修改语句中为 true，因此这里必须先区分：
  // - 普通读只需要在表上声明“准备读取行”（IS）；
  // - 删除扫描会修改扫描到的行，需要在表上声明“准备修改行”（IX）。
  if (exec_ctx_->IsDelete()) {
    // IX、SIX、X 都足以保护后续的行 X 锁。事务可能在前一条 SQL 中已经取得其中一种锁，
    // 此时不能再无脑请求 IX：例如 X -> IX 属于降级，会被 LockManager 判为非法升级。
    const bool has_write_table_lock = txn->IsTableIntentionExclusiveLocked(table_oid) ||
                                      txn->IsTableSharedIntentionExclusiveLocked(table_oid) ||
                                      txn->IsTableExclusiveLocked(table_oid);
    if (!has_write_table_lock) {
      // 极少数情况下事务可能已经显式持有表 S；S 与“准备修改行”的 IX 语义合并后应升级为 SIX。
      // 执行器通常只会持有 IS，因此常规路径是 IS -> IX 或直接申请 IX。
      const auto requested_mode = txn->IsTableSharedLocked(table_oid)
                                      ? LockManager::LockMode::SHARED_INTENTION_EXCLUSIVE
                                      : LockManager::LockMode::INTENTION_EXCLUSIVE;
      if (!lock_manager->LockTable(txn, requested_mode, table_oid)) {
        // LockTable 返回 false 通常表示事务在等待期间被死锁检测器终止。
        // 转换为 ExecutionException 后，ExecutionEngine 会停止本条 SQL 并向调用者报告失败。
        throw ExecutionException("SeqScanExecutor failed to acquire a table lock for deletion");
      }
    }
  } else if (txn->GetIsolationLevel() != IsolationLevel::READ_UNCOMMITTED) {
    // READ_UNCOMMITTED 不允许也不需要 S/IS 锁；RR 与 RC 的普通扫描则必须先持有能保护行 S 的表锁。
    // IS、S、IX、SIX、X 都满足这个前置条件，因此只在五种锁都没有时请求新的 IS。
    const bool has_read_table_lock =
        txn->IsTableIntentionSharedLocked(table_oid) || txn->IsTableSharedLocked(table_oid) ||
        txn->IsTableIntentionExclusiveLocked(table_oid) || txn->IsTableSharedIntentionExclusiveLocked(table_oid) ||
        txn->IsTableExclusiveLocked(table_oid);
    if (!has_read_table_lock && !lock_manager->LockTable(txn, LockManager::LockMode::INTENTION_SHARED, table_oid)) {
      throw ExecutionException("SeqScanExecutor failed to acquire a table lock for reading");
    }
  }

  // Project 4 要求使用 EagerIterator。它不固定创建迭代器时的尾 RID，适合并发事务持续向 TableHeap
  // 追加记录的场景；Project 3 的 MakeIterator() 会记录停止位置，主要用于规避 Update 的 Halloween 问题。
  table_iter_ = std::make_unique<TableIterator>(table_info_->table_->MakeEagerIterator());
}

auto SeqScanExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  auto *txn = exec_ctx_->GetTransaction();
  auto *lock_manager = exec_ctx_->GetLockManager();
  const auto table_oid = plan_->GetTableOid();
  const bool is_delete = exec_ctx_->IsDelete();
  const auto isolation_level = txn->GetIsolationLevel();

  while (!table_iter_->IsEnd()) {
    // 第 1 步：先保存当前位置。行锁以 RID 为资源标识，所以必须在读取 Tuple 之前得到当前 RID。
    const auto current_rid = table_iter_->GetRID();

    // 第 2 步：根据本次操作与隔离级别决定是否加行锁。
    // acquired_row_lock 只表示“本轮循环新取得了锁”。如果锁来自事务之前执行的 SQL，就不能在下面
    // 因为当前记录不可见而误释放它；这也是题目强调一个事务可能包含多条查询的原因。
    bool acquired_row_lock = false;
    if (is_delete) {
      // DELETE/UPDATE 必须持有行 X，并一直保留到 Commit/Abort。若当前只有行 S，调用 LockRow(X)
      // 会走 Task 1 实现的 S -> X 升级；若已经有 X，则无需生成重复请求。
      if (!txn->IsRowExclusiveLocked(table_oid, current_rid)) {
        // 先记住事务是否早已持有 S。S -> X 虽然会调用 LockRow，但它是对旧锁的升级，不是本轮
        // 新获得的一份资源所有权；即使这条记录后来被谓词跳过，也不能把升级后的 X 提前释放掉。
        const bool had_shared_lock = txn->IsRowSharedLocked(table_oid, current_rid);
        if (!lock_manager->LockRow(txn, LockManager::LockMode::EXCLUSIVE, table_oid, current_rid)) {
          throw ExecutionException("SeqScanExecutor failed to acquire an exclusive row lock");
        }
        acquired_row_lock = !had_shared_lock;
      }
    } else if (isolation_level != IsolationLevel::READ_UNCOMMITTED) {
      // RR/RC 的普通读取需要行 S；事务已经持有 S 或更强的 X 时都可以直接读取。
      // RU 允许脏读，并且 LockManager 明确禁止 RU 请求 S，所以该隔离级别完全跳过此分支。
      const bool already_locked =
          txn->IsRowSharedLocked(table_oid, current_rid) || txn->IsRowExclusiveLocked(table_oid, current_rid);
      if (!already_locked) {
        if (!lock_manager->LockRow(txn, LockManager::LockMode::SHARED, table_oid, current_rid)) {
          throw ExecutionException("SeqScanExecutor failed to acquire a shared row lock");
        }
        acquired_row_lock = true;
      }
    }

    // 第 3 步：成功取得所需行锁后，才能读取 TupleMeta 和 Tuple。这样若另一个事务正在写该行，
    // RR/RC 读者会先在 LockRow 中等待，直到写事务提交或回滚，再观察稳定的删除标记和数据。
    auto [meta, current_tuple] = table_iter_->GetTuple();

    // 当前 Tuple 已复制出来，可以把迭代器推进到下一 RID。下一次循环会对新 RID 独立完成加锁流程。
    ++(*table_iter_);

    // 第 4 步：逻辑删除记录不能返回。若启用了可选的 Filter -> SeqScan 下推优化，谓词不满足的
    // 记录同样不应交给父执行器；默认 starter optimizer 下 filter_predicate_ 为 nullptr。
    bool should_return = !meta.is_deleted_;
    if (should_return && plan_->filter_predicate_ != nullptr) {
      const auto predicate = plan_->filter_predicate_->Evaluate(&current_tuple, table_info_->schema_);
      should_return = !predicate.IsNull() && predicate.GetAs<bool>();
    }

    if (!should_return) {
      if (acquired_row_lock && !lock_manager->UnlockRow(txn, table_oid, current_rid, true)) {
        // force=true 表示这条记录实际上没有被本次操作访问，不应因提前释放 S/X 而让事务进入 SHRINKING。
        throw ExecutionException("SeqScanExecutor failed to release a skipped row lock");
      }
      continue;
    }

    // 第 5 步：READ_COMMITTED 只保证“读取这一刻”数据已经提交，所以本轮新取得的 S 可以立即释放。
    // REPEATABLE_READ 必须把 S 保留到事务结束，保证再次读取仍看到相同记录；DELETE 的 X 无论隔离级别
    // 都必须保留到事务结束。事务之前已经持有的锁也不归本轮扫描释放。
    if (!is_delete && isolation_level == IsolationLevel::READ_COMMITTED && acquired_row_lock &&
        !lock_manager->UnlockRow(txn, table_oid, current_rid)) {
      throw ExecutionException("SeqScanExecutor failed to release a shared row lock");
    }

    // 第 6 步：只有通过删除标记（以及可选谓词）检查的记录，才作为本次 Next() 的一个输出返回。
    *tuple = std::move(current_tuple);
    *rid = current_rid;
    return true;
  }

  // 所有物理 Slot 均已扫描完毕，通知父执行器当前 SeqScan 到达 EOF。
  return false;
}

}  // namespace bustub
