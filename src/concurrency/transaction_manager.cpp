//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// transaction_manager.cpp
//
// Identification: src/concurrency/transaction_manager.cpp
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "concurrency/transaction_manager.h"

#include <mutex>  // NOLINT
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "catalog/catalog.h"
#include "common/macros.h"
#include "storage/table/table_heap.h"
namespace bustub {

void TransactionManager::Commit(Transaction *txn) {
  // Release all the locks.
  ReleaseLocks(txn);

  txn->SetState(TransactionState::COMMITTED);
}

void TransactionManager::Abort(Transaction *txn) {
  // Executor 对 TableHeap 的 INSERT/DELETE 都是立即生效的：
  // - INSERT 创建一个 is_deleted_ == false 的新 Slot；
  // - DELETE 把已有 Slot 的 is_deleted_ 改成 true。
  // 因此事务不能只释放锁，还必须在释放 X 锁之前撤销这些物理修改，否则其他事务会在锁释放后
  // 看到一个本应中止的结果。
  auto write_set = txn->GetWriteSet();

  // 必须从队尾逆序撤销。举例：同一事务先 INSERT(r)，随后又 DELETE(r)，write set 为
  // [INSERT(r), DELETE(r)]；回滚时先撤销 DELETE 令记录恢复可见，再撤销 INSERT 令记录最终不可见，
  // 才能回到事务开始前“r 不存在”的状态。
  while (!write_set->empty()) {
    auto &write_record = write_set->back();

    // Spring 2023 基础任务只要求 Insert/Delete，并且二者都只改变 is_deleted_，所以不需要依赖
    // TableWriteRecord::wtype_：把当前删除标记反转一次，恰好就是对应操作的逆操作。
    // 读取完整旧 Tuple 并重写数据既没有必要，也可能覆盖其他元数据；这里只更新 TupleMeta。
    auto tuple_meta = write_record.table_heap_->GetTupleMeta(write_record.rid_);
    tuple_meta.is_deleted_ = !tuple_meta.is_deleted_;
    write_record.table_heap_->UpdateTupleMeta(tuple_meta, write_record.rid_);

    // 当前记录成功撤销后再移出 write set。下一轮继续处理更早发生的那次写入。
    write_set->pop_back();
  }

  // 回滚期间仍持有写操作的行 X，因此不会有其他 RR/RC 事务观察到中间状态。
  // ReleaseLocks() 会先释放全部行锁，再释放表锁，符合多粒度锁要求；锁释放后等待者才可继续执行。
  ReleaseLocks(txn);

  // 最后把事务标记为 ABORTED。若它是死锁检测选中的牺牲者，此前可能已经是 ABORTED，重复设置无害。
  txn->SetState(TransactionState::ABORTED);
}

void TransactionManager::BlockAllTransactions() { UNIMPLEMENTED("block is not supported now!"); }

void TransactionManager::ResumeTransactions() { UNIMPLEMENTED("resume is not supported now!"); }

}  // namespace bustub
