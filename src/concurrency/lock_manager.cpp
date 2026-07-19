//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lock_manager.cpp
//
// Identification: src/concurrency/lock_manager.cpp
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "concurrency/lock_manager.h"

#include "common/config.h"
#include "concurrency/transaction.h"
#include "concurrency/transaction_manager.h"

namespace bustub {

namespace {

using LockMode = LockManager::LockMode;

/**
 * Task 1 同时维护两份互相对应的状态：
 *
 * 1. LockRequestQueue 是并发控制的真实依据。某个请求只有 granted_ == true，才会阻挡其他不兼容请求。
 * 2. Transaction 中的若干 lock set 是事务自己的 bookkeeping。执行器和 TransactionManager 依靠这些集合
 *    判断事务持有哪些锁，并在 Commit/Abort 时找到需要释放的资源。
 *
 * 因此，授予、升级或释放锁时，不能只修改请求队列，也必须同步修改对应的事务锁集合。
 * 下列 bookkeeping helper 都要求调用者已经持有 txn 自身的 latch，避免多个线程同时修改同一事务的集合。
 *
 * 本文件采用的锁职责如下：
 * - table_lock_map_latch_ / row_lock_map_latch_：只保护“资源 ID -> 请求队列”的哈希表；
 * - LockRequestQueue::latch_：保护某一个资源的请求链表、granted_ 和 upgrading_；
 * - Transaction::latch_：保护该事务的状态和所有 lock set。
 *
 * 正常路径取得资源队列后会立刻释放 map latch；阻塞等待时只持有/释放资源队列 latch，绝不持有全局 map latch。
 */
void AddTableLockToTxn(Transaction *txn, LockMode lock_mode, const table_oid_t &oid) {
  // 每种表锁有独立集合。一个资源在正常情况下只会出现在其中一个集合中；
  // 升级时必须先从旧集合删除，再在升级成功后加入新集合。
  switch (lock_mode) {
    case LockMode::SHARED:
      txn->GetSharedTableLockSet()->insert(oid);
      return;
    case LockMode::EXCLUSIVE:
      txn->GetExclusiveTableLockSet()->insert(oid);
      return;
    case LockMode::INTENTION_SHARED:
      txn->GetIntentionSharedTableLockSet()->insert(oid);
      return;
    case LockMode::INTENTION_EXCLUSIVE:
      txn->GetIntentionExclusiveTableLockSet()->insert(oid);
      return;
    case LockMode::SHARED_INTENTION_EXCLUSIVE:
      txn->GetSharedIntentionExclusiveTableLockSet()->insert(oid);
      return;
  }

  UNREACHABLE("Unsupported table lock mode");
}

void RemoveTableLockFromTxn(Transaction *txn, LockMode lock_mode, const table_oid_t &oid) {
  // 必须根据请求对象记录的真实模式删除，不能根据调用者当前想要的模式删除。
  switch (lock_mode) {
    case LockMode::SHARED:
      txn->GetSharedTableLockSet()->erase(oid);
      return;
    case LockMode::EXCLUSIVE:
      txn->GetExclusiveTableLockSet()->erase(oid);
      return;
    case LockMode::INTENTION_SHARED:
      txn->GetIntentionSharedTableLockSet()->erase(oid);
      return;
    case LockMode::INTENTION_EXCLUSIVE:
      txn->GetIntentionExclusiveTableLockSet()->erase(oid);
      return;
    case LockMode::SHARED_INTENTION_EXCLUSIVE:
      txn->GetSharedIntentionExclusiveTableLockSet()->erase(oid);
      return;
  }

  UNREACHABLE("Unsupported table lock mode");
}

void AddRowLockToTxn(Transaction *txn, LockMode lock_mode, const table_oid_t &oid, const RID &rid) {
  // 行锁集合按 table oid 再分组，便于 UnlockTable() 快速判断该表中是否仍有行锁，
  // 也便于 TransactionManager 在提交或回滚时按表释放所有行锁。
  if (lock_mode == LockMode::SHARED) {
    (*txn->GetSharedRowLockSet())[oid].insert(rid);
    return;
  }
  if (lock_mode == LockMode::EXCLUSIVE) {
    (*txn->GetExclusiveRowLockSet())[oid].insert(rid);
    return;
  }

  UNREACHABLE("Rows only support shared and exclusive locks");
}

void RemoveRowLockFromTxn(Transaction *txn, LockMode lock_mode, const table_oid_t &oid, const RID &rid) {
  // 删除最后一个 RID 后顺便移除空的 table 分组，避免空集合一直留在事务中。
  auto row_lock_set = lock_mode == LockMode::SHARED ? txn->GetSharedRowLockSet() : txn->GetExclusiveRowLockSet();
  auto table_it = row_lock_set->find(oid);
  if (table_it == row_lock_set->end()) {
    return;
  }

  table_it->second.erase(rid);
  if (table_it->second.empty()) {
    row_lock_set->erase(table_it);
  }
}

void UpdateTransactionStateOnUnlock(Transaction *txn, LockMode lock_mode, bool force) {
  // force 用于事务清理等内部路径：只释放锁，不触发两阶段锁的状态迁移。
  // 已经处于 SHRINKING/ABORTED/COMMITTED 的事务也不需要再次改变状态。
  if (force || txn->GetState() != TransactionState::GROWING) {
    return;
  }

  // 项目给定的状态迁移规则：
  // - REPEATABLE_READ：释放 S 或 X 后进入 SHRINKING；
  // - READ_COMMITTED：只有释放 X 才进入 SHRINKING，释放 S 后仍可继续读取；
  // - READ_UNCOMMITTED：只允许写相关锁，因此释放 X 后进入 SHRINKING；
  // - IS、IX、SIX 的释放按照项目说明不会单独触发状态迁移。
  const auto isolation_level = txn->GetIsolationLevel();
  if ((isolation_level == IsolationLevel::REPEATABLE_READ &&
       (lock_mode == LockMode::SHARED || lock_mode == LockMode::EXCLUSIVE)) ||
      ((isolation_level == IsolationLevel::READ_COMMITTED || isolation_level == IsolationLevel::READ_UNCOMMITTED) &&
       lock_mode == LockMode::EXCLUSIVE)) {
    txn->SetState(TransactionState::SHRINKING);
  }
}

auto IsTransactionFinished(Transaction *txn) -> bool {
  // 等待谓词同时检查 ABORTED 和 COMMITTED，确保已经结束的事务不会在被唤醒后取得新锁。
  return txn->GetState() == TransactionState::ABORTED || txn->GetState() == TransactionState::COMMITTED;
}

}  // namespace

/**
 * 为事务申请表锁的完整流程：
 *
 * 1. 拒绝已经结束的事务；
 * 2. 从 table_lock_map_ 中取得该表唯一的请求队列；
 * 3. 若事务已持有同模式锁，直接成功；若持有其他模式，转入 UpgradeLockTable()；
 * 4. 对全新请求检查隔离级别与两阶段锁状态，然后将请求追加到队尾；
 * 5. 尝试按 FIFO 授予请求，不能立即授予时使用条件变量睡眠；
 * 6. 被唤醒后区分“锁已授予”和“事务已结束”，最后更新事务的 table lock set。
 *
 * 注意：condition_variable::wait() 会在睡眠前原子地释放 queue latch，被唤醒后再重新取得它。
 * 因此其他线程可以在当前线程睡眠期间进入 UnlockTable()，删除旧请求并授予队首等待者。
 */
auto LockManager::LockTable(Transaction *txn, LockMode lock_mode, const table_oid_t &oid) -> bool {
  // 步骤 1：已经结束的事务不能发起新的锁请求。等待期间被死锁检测终止后再次进入也会走这里。
  if (IsTransactionFinished(txn)) {
    return false;
  }

  std::shared_ptr<LockRequestQueue> request_queue;
  {
    // 步骤 2：取得该表的请求队列。
    // shared_ptr 保证释放全局 map latch 后队列对象仍然存活。全局 latch 只保护哈希表本身，
    // 绝不能带着它进入条件变量等待，否则其他表的 Lock/Unlock 也会全部被阻塞。
    std::lock_guard<std::mutex> map_guard(table_lock_map_latch_);
    auto &queue = table_lock_map_[oid];
    if (queue == nullptr) {
      queue = std::make_shared<LockRequestQueue>();
    }
    request_queue = queue;
  }

  std::unique_lock<std::mutex> queue_guard(request_queue->latch_);
  const auto txn_id = txn->GetTransactionId();

  // 步骤 3：检查事务在该表上是否已经有 granted 请求。
  // 队列中的 granted 请求才代表事务真正持有锁。必须先处理重复请求，再调用 CanTxnTakeLock()：
  // 事务在 SHRINKING 阶段再次请求自己已持有的同模式锁并没有获得“新锁”，所以应直接成功。
  auto existing_it =
      std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                   [txn_id](const LockRequest *request) { return request->txn_id_ == txn_id && request->granted_; });
  if (existing_it != request_queue->request_queue_.end()) {
    if ((*existing_it)->lock_mode_ == lock_mode) {
      // 例如事务已经持有表 S，又请求一次表 S。unordered_set 无需重复插入，队列也不能出现重复请求。
      return !IsTransactionFinished(txn);
    }

    // 已持有锁但请求模式不同，属于升级。升级函数会重新取得同一个队列锁，
    // 因此必须先释放当前 unique_lock，否则同一线程会再次锁住非递归 mutex，造成自锁死。
    queue_guard.unlock();
    if (!CanTxnTakeLock(txn, lock_mode)) {
      return false;
    }
    return UpgradeLockTable(txn, lock_mode, oid);
  }

  // 步骤 4：这是全新请求，必须符合隔离级别以及 GROWING/SHRINKING 状态约束。
  // CanTxnTakeLock() 对非法操作会先把事务置为 ABORTED，再抛出带具体原因的异常。
  if (!CanTxnTakeLock(txn, lock_mode)) {
    return false;
  }

  // 步骤 5：请求入队。
  // LockRequest 在“等待期间”和“锁持有期间”都必须留在队列中，因此这里在堆上创建。
  // 成功后由 UnlockTable() 删除；等待时事务结束则由下面的失败分支删除；最后还有 UnlockAll() 兜底清理。
  auto *request = new LockRequest(txn_id, lock_mode, oid);
  request_queue->request_queue_.push_back(request);

  // GrantNewLocksIfPossible() 既处理“空资源立即授予”，也处理“多个兼容请求一起授予”。
  // 若请求仍未授予，wait() 会释放 queue latch 并睡眠。谓词可抵抗虚假唤醒：
  // 只有自己的 granted_ 变为 true，或者事务已经结束，线程才真正离开 wait()。
  GrantNewLocksIfPossible(request_queue.get());
  request_queue->cv_.wait(queue_guard, [request, txn] { return request->granted_ || IsTransactionFinished(txn); });

  // 步骤 6：同步 Transaction bookkeeping。
  // 事务可能在等待期间被终止。即使请求恰好刚被标记为 granted，也不能把锁登记给已结束事务。
  // 这里在 txn latch 下完成“再次检查状态 + 插入 lock set”，避免两个操作之间被其他线程穿插。
  txn->LockTxn();
  const bool txn_finished = IsTransactionFinished(txn);
  if (!txn_finished) {
    AddTableLockToTxn(txn, lock_mode, oid);
  }
  txn->UnlockTxn();

  if (txn_finished) {
    // 当前请求如果已经被授予，删除它相当于立即释放该锁；如果还在等待，删除它则消除了 FIFO 队首阻塞。
    // 两种情况下都要重新调用调度器，让后面的请求获得一次继续前进的机会。
    request_queue->request_queue_.remove(request);
    delete request;
    GrantNewLocksIfPossible(request_queue.get());
    request_queue->cv_.notify_all();
    return false;
  }

  return true;
}

/**
 * 释放事务在指定表上的已授予锁：
 *
 * 1. 找到表队列以及该事务的 granted 请求；
 * 2. 验证事务已经释放这个表中的全部行锁；
 * 3. 从 Transaction lock set 和请求队列中同时移除锁；
 * 4. 根据隔离级别更新事务阶段；
 * 5. 重新调度并唤醒等待该表的事务。
 *
 * Unlock 不检查事务当前是否已经 ABORTED，因为 TransactionManager::Abort() 本身就需要通过这些接口
 * 释放事务遗留的锁。只要请求仍是 granted，它就必须能够被清理。
 */
auto LockManager::UnlockTable(Transaction *txn, const table_oid_t &oid) -> bool {
  std::shared_ptr<LockRequestQueue> request_queue;
  {
    // 步骤 1a：只读取 map 并复制 shared_ptr；离开作用域后立刻释放全局 map latch。
    std::lock_guard<std::mutex> map_guard(table_lock_map_latch_);
    const auto queue_it = table_lock_map_.find(oid);
    if (queue_it != table_lock_map_.end()) {
      request_queue = queue_it->second;
    }
  }

  // 资源队列不存在，说明从未有人在该表上创建过请求，该事务自然不可能持有它。
  // “没有锁却解锁”是事务协议错误，不是普通的 false 返回，因此需要 Abort + throw。
  if (request_queue == nullptr) {
    txn->SetState(TransactionState::ABORTED);
    throw TransactionAbortException(txn->GetTransactionId(), AbortReason::ATTEMPTED_UNLOCK_BUT_NO_LOCK_HELD);
  }

  std::unique_lock<std::mutex> queue_guard(request_queue->latch_);
  const auto txn_id = txn->GetTransactionId();
  // 步骤 1b：等待请求不等于持有锁，所以查找时必须同时要求 txn_id 相同且 granted_ == true。
  auto request_it =
      std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                   [txn_id](const LockRequest *request) { return request->txn_id_ == txn_id && request->granted_; });
  if (request_it == request_queue->request_queue_.end()) {
    txn->SetState(TransactionState::ABORTED);
    throw TransactionAbortException(txn_id, AbortReason::ATTEMPTED_UNLOCK_BUT_NO_LOCK_HELD);
  }

  auto *request = *request_it;
  const auto released_lock_mode = request->lock_mode_;

  txn->LockTxn();

  // 步骤 2：检查“先行后表”的释放顺序。
  // 多粒度锁要求先释放表内所有行锁，再释放表锁。举例：事务仍持有某行 X，却先释放表 IX，
  // 其他事务随后可能取得表 S；表 S 与那把仍存活的行 X 在层次协议中就失去了冲突保护。
  bool holds_row_lock = false;
  const auto shared_rows_it = txn->GetSharedRowLockSet()->find(oid);
  if (shared_rows_it != txn->GetSharedRowLockSet()->end() && !shared_rows_it->second.empty()) {
    holds_row_lock = true;
  }
  const auto exclusive_rows_it = txn->GetExclusiveRowLockSet()->find(oid);
  if (exclusive_rows_it != txn->GetExclusiveRowLockSet()->end() && !exclusive_rows_it->second.empty()) {
    holds_row_lock = true;
  }

  if (holds_row_lock) {
    // 此处不能删除表请求；异常被上层捕获后，TransactionManager::Abort() 会先释放行锁，再回来释放表锁。
    txn->SetState(TransactionState::ABORTED);
    txn->UnlockTxn();
    throw TransactionAbortException(txn_id, AbortReason::TABLE_UNLOCKED_BEFORE_UNLOCKING_ROWS);
  }

  // 步骤 3、4：先在 txn latch 下删除事务视角的记录并更新 2PL 状态，
  // 再删除队列中的请求。整个期间仍持有 queue latch，所以没有其他线程能观察或修改本资源队列。
  RemoveTableLockFromTxn(txn, released_lock_mode, oid);
  UpdateTransactionStateOnUnlock(txn, released_lock_mode, false);
  txn->UnlockTxn();

  request_queue->request_queue_.erase(request_it);
  delete request;

  // 步骤 5：当前锁释放后，队首的一批兼容请求可能可以继续执行。
  // GrantNewLocksIfPossible() 负责设置 granted_；notify_all() 还会唤醒因事务终止而需要清理自身的 waiter。
  GrantNewLocksIfPossible(request_queue.get());
  request_queue->cv_.notify_all();
  return true;
}

/**
 * 将事务已经持有的表锁升级为更强模式。
 *
 * 升级不能简单地在队尾再放一个新请求，否则会出现两类问题：
 * - 新请求会与事务自己的旧锁冲突，例如 S -> X 时，X 会一直被自己的 S 阻挡；
 * - 普通等待请求可能排在升级请求前面，违反项目要求的“升级优先”。
 *
 * 本实现复用原 LockRequest：先删除旧模式 bookkeeping，把请求改成未授予的新模式，然后插入到
 * “所有 granted 请求之后、所有普通 waiting 请求之前”。queue latch 覆盖整个转换过程，因此不存在
 * 其他线程趁旧锁移除、新请求尚未插入的空窗。upgrading_ 则保证同一资源最多只有一个等待升级者。
 */
auto LockManager::UpgradeLockTable(Transaction *txn, LockMode lock_mode, const table_oid_t &oid) -> bool {
  std::shared_ptr<LockRequestQueue> request_queue;
  {
    // 步骤 1：重新取得资源队列。LockTable() 调用本函数前已经释放 queue latch，避免重复加锁。
    std::lock_guard<std::mutex> map_guard(table_lock_map_latch_);
    const auto queue_it = table_lock_map_.find(oid);
    if (queue_it != table_lock_map_.end()) {
      request_queue = queue_it->second;
    }
  }

  // 本函数只由已经找到旧表锁的 LockTable() 调用；队列消失代表内部状态不一致。
  BUSTUB_ASSERT(request_queue != nullptr, "Cannot upgrade a table lock without its request queue");

  std::unique_lock<std::mutex> queue_guard(request_queue->latch_);
  const auto txn_id = txn->GetTransactionId();
  // 只有 granted 请求能作为升级起点。一个尚在等待的普通请求不能再次发起升级。
  auto request_it =
      std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                   [txn_id](const LockRequest *request) { return request->txn_id_ == txn_id && request->granted_; });
  if (request_it == request_queue->request_queue_.end()) {
    // 同一事务不应并发地升级和释放同一把锁；若真的发生，避免创建重复请求。
    return false;
  }

  auto *request = *request_it;
  const auto old_lock_mode = request->lock_mode_;
  if (old_lock_mode == lock_mode) {
    // 正常情况下相同模式已经由 LockTable() 提前处理；这里保留防御性判断。
    return true;
  }

  // 步骤 2：验证升级边。例如 S -> X 合法；X -> S 是降级，不在允许边中。
  // 非法升级只终止事务，不在这里删除旧锁。上层随后调用 TransactionManager::Abort() 时仍能找到并释放它。
  if (!CanLockUpgrade(old_lock_mode, lock_mode)) {
    txn->LockTxn();
    txn->SetState(TransactionState::ABORTED);
    txn->UnlockTxn();
    throw TransactionAbortException(txn_id, AbortReason::INCOMPATIBLE_UPGRADE);
  }

  // 步骤 3：登记唯一升级者。
  // 例如 T1、T2 都持有 S，又同时请求 X：如果二者都保留升级资格，就会互相等待对方的 S。
  // 项目规定后来的升级者直接以 UPGRADE_CONFLICT 终止，从而只留下一个升级者继续等待。
  if (request_queue->upgrading_ != INVALID_TXN_ID && request_queue->upgrading_ != txn_id) {
    txn->LockTxn();
    txn->SetState(TransactionState::ABORTED);
    txn->UnlockTxn();
    throw TransactionAbortException(txn_id, AbortReason::UPGRADE_CONFLICT);
  }

  txn->LockTxn();
  if (IsTransactionFinished(txn)) {
    txn->UnlockTxn();
    return false;
  }

  // 步骤 4：移除旧模式 bookkeeping。
  // 请求马上会被改成 granted_ = false，因此 Transaction 的集合也不能继续声称旧模式仍处于已授予状态。
  RemoveTableLockFromTxn(txn, old_lock_mode, oid);
  txn->UnlockTxn();

  request_queue->upgrading_ = txn_id;

  // 步骤 5：在同一个 queue latch 临界区中，把原 granted 请求改造成新模式的 waiting 请求。
  // first_waiting 指向第一个普通等待者；插在它前面即可同时满足：
  // - 已经授予的其他事务仍排在前面并继续阻挡不兼容升级；
  // - 升级者不会等待自己的旧请求；
  // - 升级请求优先于先前尚未授予的普通请求。
  request_queue->request_queue_.erase(request_it);
  request->lock_mode_ = lock_mode;
  request->granted_ = false;
  const auto first_waiting = std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                                          [](const LockRequest *queued_request) { return !queued_request->granted_; });
  request_queue->request_queue_.insert(first_waiting, request);

  // 步骤 6：如果升级模式与所有剩余 granted 锁兼容，就会立即成功；否则睡眠等待其他持有者解锁。
  GrantNewLocksIfPossible(request_queue.get());
  request_queue->cv_.wait(queue_guard, [request, txn] { return request->granted_ || IsTransactionFinished(txn); });

  // 步骤 7：升级成功时只加入新模式集合；旧模式集合已在步骤 4 删除，因此事务不会同时记录两种模式。
  // 若等待期间事务结束，则不登记新模式，下面直接删除升级请求。
  txn->LockTxn();
  const bool txn_finished = IsTransactionFinished(txn);
  if (!txn_finished) {
    AddTableLockToTxn(txn, lock_mode, oid);
  }
  txn->UnlockTxn();

  if (request_queue->upgrading_ == txn_id) {
    // 调度器通常会在授予升级请求时清空该字段；这里再次判断是为了覆盖“等待期间终止”等失败路径。
    request_queue->upgrading_ = INVALID_TXN_ID;
  }

  if (txn_finished) {
    // 失败的升级请求已经不再代表旧锁，所以无需恢复旧模式；事务已经结束，只需彻底移除请求并唤醒后继者。
    request_queue->request_queue_.remove(request);
    delete request;
    GrantNewLocksIfPossible(request_queue.get());
    request_queue->cv_.notify_all();
    return false;
  }

  // 唤醒其他线程重新检查 granted_ 或事务状态。升级后的请求本身继续留在队列，直到 UnlockTable() 删除。
  request_queue->cv_.notify_all();
  return true;
}

/**
 * 为事务申请行锁。其入队、等待和清理过程与 LockTable() 相同，但多出两项行级约束：
 *
 * 1. 行是本项目锁层次的最低一级，只支持 S/X，不能在行上申请 IS/IX/SIX；
 * 2. 申请行锁前必须持有对应表锁。行 S 需要 IS/IX/S/SIX/X，行 X 需要 IX/SIX/X。
 *
 * 表锁检查并不是重复工作：请求队列只回答“这个 RID 上的锁是否互相兼容”，它并不知道该事务
 * 是否遵守了多粒度锁协议。CheckAppropriateLockOnTable() 专门补上这层跨资源约束。
 */
auto LockManager::LockRow(Transaction *txn, LockMode lock_mode, const table_oid_t &oid, const RID &rid) -> bool {
  // 步骤 1：结束状态下不再创建行请求。
  if (IsTransactionFinished(txn)) {
    return false;
  }

  // 步骤 2：验证行锁模式。
  // IS/IX/SIX 只用于较高层级，表示“准备在子资源上加锁”；行已经没有需要继续声明的子层级。
  if (lock_mode != LockMode::SHARED && lock_mode != LockMode::EXCLUSIVE) {
    txn->SetState(TransactionState::ABORTED);
    throw TransactionAbortException(txn->GetTransactionId(), AbortReason::ATTEMPTED_INTENTION_LOCK_ON_ROW);
  }

  std::shared_ptr<LockRequestQueue> request_queue;
  {
    // 步骤 3：取得 RID 对应的唯一队列。
    // 与表锁相同，全局 row map latch 只负责哈希表查找，不能带入阻塞等待。
    std::lock_guard<std::mutex> map_guard(row_lock_map_latch_);
    auto &queue = row_lock_map_[rid];
    if (queue == nullptr) {
      queue = std::make_shared<LockRequestQueue>();
    }
    request_queue = queue;
  }

  std::unique_lock<std::mutex> queue_guard(request_queue->latch_);
  const auto txn_id = txn->GetTransactionId();
  // oid 也参与匹配，防止调用者用错误的表 ID 解读同一个 RID 请求。
  auto existing_it = std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                                  [txn_id, oid](const LockRequest *request) {
                                    return request->txn_id_ == txn_id && request->oid_ == oid && request->granted_;
                                  });

  if (existing_it != request_queue->request_queue_.end()) {
    if ((*existing_it)->lock_mode_ == lock_mode) {
      // 重复申请已持有的同模式行锁是幂等操作，不重复入队，也不重复修改 lock set。
      return !IsTransactionFinished(txn);
    }

    // 不同模式意味着行锁升级。先释放 queue latch，再完成状态和表锁前置条件检查。
    queue_guard.unlock();
    if (!CanTxnTakeLock(txn, lock_mode)) {
      return false;
    }

    txn->LockTxn();
    const bool has_table_lock = CheckAppropriateLockOnTable(txn, oid, lock_mode);
    txn->UnlockTxn();
    if (!has_table_lock) {
      // 例如只有表 IS 却请求行 X：IS 只允许在下层读取，不能保护写入，因此必须终止事务。
      txn->SetState(TransactionState::ABORTED);
      throw TransactionAbortException(txn_id, AbortReason::TABLE_LOCK_NOT_PRESENT);
    }

    return UpgradeLockRow(txn, lock_mode, oid, rid);
  }

  // 步骤 4：全新请求先检查隔离级别与 2PL 状态。
  if (!CanTxnTakeLock(txn, lock_mode)) {
    return false;
  }

  // 步骤 5：检查多粒度锁前置条件。
  // 行锁必须由相应表锁保护：行 S 需要读或更强权限，行 X 需要明确的下层写权限。
  // 读取 Transaction 的表锁集合时持有 txn latch，避免与表锁升级/释放并发修改这些集合。
  txn->LockTxn();
  const bool has_table_lock = CheckAppropriateLockOnTable(txn, oid, lock_mode);
  txn->UnlockTxn();
  if (!has_table_lock) {
    txn->SetState(TransactionState::ABORTED);
    throw TransactionAbortException(txn_id, AbortReason::TABLE_LOCK_NOT_PRESENT);
  }

  // 步骤 6：请求入队并尝试授予。LockRequest 同时记录 oid 和 rid，便于事务 bookkeeping 与异常检查。
  auto *request = new LockRequest(txn_id, lock_mode, oid, rid);
  request_queue->request_queue_.push_back(request);
  GrantNewLocksIfPossible(request_queue.get());
  request_queue->cv_.wait(queue_guard, [request, txn] { return request->granted_ || IsTransactionFinished(txn); });

  // 步骤 7：醒来后只有仍在运行的事务才能把 RID 登记到共享或独占行锁集合。
  txn->LockTxn();
  const bool txn_finished = IsTransactionFinished(txn);
  if (!txn_finished) {
    AddRowLockToTxn(txn, lock_mode, oid, rid);
  }
  txn->UnlockTxn();

  if (txn_finished) {
    // 被终止的 waiter 主动删除自己的请求。若它刚被授予，则删除也会释放资源并推动后继请求。
    request_queue->request_queue_.remove(request);
    delete request;
    GrantNewLocksIfPossible(request_queue.get());
    request_queue->cv_.notify_all();
    return false;
  }

  return true;
}

/**
 * 释放一个已授予的行锁。
 *
 * 与 UnlockTable() 相比，这里不需要检查子层级资源，因为行已经是叶子层级；只需找到 granted 请求、
 * 删除事务 bookkeeping、按需更新事务阶段，然后重新调度该 RID 的等待队列。
 *
 * force == true 时仍会真正释放请求和 lock set，但跳过 GROWING -> SHRINKING。这个开关供内部清理路径使用，
 * 避免一次非事务语义的提前释放意外改变后续锁行为。
 */
auto LockManager::UnlockRow(Transaction *txn, const table_oid_t &oid, const RID &rid, bool force) -> bool {
  std::shared_ptr<LockRequestQueue> request_queue;
  {
    // 步骤 1：查找 RID 队列。这里只短暂持有全局 map latch。
    std::lock_guard<std::mutex> map_guard(row_lock_map_latch_);
    const auto queue_it = row_lock_map_.find(rid);
    if (queue_it != row_lock_map_.end()) {
      request_queue = queue_it->second;
    }
  }

  if (request_queue == nullptr) {
    // 从未创建过队列意味着不可能持有该行锁，按协议错误终止事务。
    txn->SetState(TransactionState::ABORTED);
    throw TransactionAbortException(txn->GetTransactionId(), AbortReason::ATTEMPTED_UNLOCK_BUT_NO_LOCK_HELD);
  }

  std::unique_lock<std::mutex> queue_guard(request_queue->latch_);
  const auto txn_id = txn->GetTransactionId();
  // 步骤 2：只有同一事务、同一表且已经 granted 的请求才允许释放；waiting 请求不算已持有锁。
  auto request_it = std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                                 [txn_id, oid](const LockRequest *request) {
                                   return request->txn_id_ == txn_id && request->oid_ == oid && request->granted_;
                                 });
  if (request_it == request_queue->request_queue_.end()) {
    txn->SetState(TransactionState::ABORTED);
    throw TransactionAbortException(txn_id, AbortReason::ATTEMPTED_UNLOCK_BUT_NO_LOCK_HELD);
  }

  auto *request = *request_it;
  const auto released_lock_mode = request->lock_mode_;

  // 步骤 3：先从事务自己的集合中删除 RID，并根据 force/隔离级别决定是否进入 SHRINKING。
  txn->LockTxn();
  RemoveRowLockFromTxn(txn, released_lock_mode, oid, rid);
  UpdateTransactionStateOnUnlock(txn, released_lock_mode, force);
  txn->UnlockTxn();

  request_queue->request_queue_.erase(request_it);
  delete request;

  // 步骤 4：请求对象已经不再阻挡队列，重新授予队首兼容请求并唤醒所有 waiter。
  GrantNewLocksIfPossible(request_queue.get());
  request_queue->cv_.notify_all();
  return true;
}

/**
 * 行锁升级与表锁升级使用相同的队列算法：复用旧请求、移除旧 bookkeeping、将新模式请求放到普通
 * waiter 之前，再等待它与所有现有 granted 请求兼容。区别在于行只支持 S/X，所以唯一合法升级是 S -> X。
 *
 * 例如 T1、T2 都持有同一行的 S，T1 请求 X：T1 的请求会改成 waiting X 并获得升级优先级；
 * 在 T2 释放 S 前，T1 继续睡眠；释放后调度器把 T1 标为 granted X，T1 才把 RID 加入 exclusive row set。
 */
auto LockManager::UpgradeLockRow(Transaction *txn, LockMode lock_mode, const table_oid_t &oid, const RID &rid) -> bool {
  std::shared_ptr<LockRequestQueue> request_queue;
  {
    // 步骤 1：取得 RID 队列。调用者已经验证隔离级别和对应表锁，因此这里专注于升级队列本身。
    std::lock_guard<std::mutex> map_guard(row_lock_map_latch_);
    const auto queue_it = row_lock_map_.find(rid);
    if (queue_it != row_lock_map_.end()) {
      request_queue = queue_it->second;
    }
  }

  BUSTUB_ASSERT(request_queue != nullptr, "Cannot upgrade a row lock without its request queue");

  std::unique_lock<std::mutex> queue_guard(request_queue->latch_);
  const auto txn_id = txn->GetTransactionId();
  // 升级必须基于该事务当前真正持有的 granted 请求。
  auto request_it = std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                                 [txn_id, oid](const LockRequest *request) {
                                   return request->txn_id_ == txn_id && request->oid_ == oid && request->granted_;
                                 });
  if (request_it == request_queue->request_queue_.end()) {
    return false;
  }

  auto *request = *request_it;
  const auto old_lock_mode = request->lock_mode_;
  if (old_lock_mode == lock_mode) {
    return true;
  }

  // 步骤 2：行只允许 S/X，因此实际合法升级只有 S -> X；X -> S 会走不兼容升级异常。
  // 和表锁一样，异常路径保留旧的 granted 请求，方便随后由 Abort() 正常释放。
  if (!CanLockUpgrade(old_lock_mode, lock_mode)) {
    txn->LockTxn();
    txn->SetState(TransactionState::ABORTED);
    txn->UnlockTxn();
    throw TransactionAbortException(txn_id, AbortReason::INCOMPATIBLE_UPGRADE);
  }

  // 步骤 3：每个 RID 同时只能存在一个升级者，避免两个共享持有者同时等待对方释放。
  if (request_queue->upgrading_ != INVALID_TXN_ID && request_queue->upgrading_ != txn_id) {
    txn->LockTxn();
    txn->SetState(TransactionState::ABORTED);
    txn->UnlockTxn();
    throw TransactionAbortException(txn_id, AbortReason::UPGRADE_CONFLICT);
  }

  txn->LockTxn();
  if (IsTransactionFinished(txn)) {
    txn->UnlockTxn();
    return false;
  }
  // 步骤 4：请求即将从 granted S 变成 waiting X，事务的 shared row set 必须同步删除该 RID。
  RemoveRowLockFromTxn(txn, old_lock_mode, oid, rid);
  txn->UnlockTxn();

  request_queue->upgrading_ = txn_id;
  // 步骤 5：复用原对象并把它插到第一个普通 waiter 前。整个转换都在 queue latch 下，不存在可见空窗。
  request_queue->request_queue_.erase(request_it);
  request->lock_mode_ = lock_mode;
  request->granted_ = false;
  const auto first_waiting = std::find_if(request_queue->request_queue_.begin(), request_queue->request_queue_.end(),
                                          [](const LockRequest *queued_request) { return !queued_request->granted_; });
  request_queue->request_queue_.insert(first_waiting, request);

  // 步骤 6：若仍有其他 S 持有者，X 与它们不兼容，当前线程会在这里睡眠；最后一个 S 释放后才会被唤醒。
  GrantNewLocksIfPossible(request_queue.get());
  request_queue->cv_.wait(queue_guard, [request, txn] { return request->granted_ || IsTransactionFinished(txn); });

  // 步骤 7：成功才加入 exclusive row set；等待期间终止则保持两个 row set 中都没有该 RID。
  txn->LockTxn();
  const bool txn_finished = IsTransactionFinished(txn);
  if (!txn_finished) {
    AddRowLockToTxn(txn, lock_mode, oid, rid);
  }
  txn->UnlockTxn();

  if (request_queue->upgrading_ == txn_id) {
    // 成功授予时调度器通常已清空 upgrading_；这里负责失败/终止路径的兜底。
    request_queue->upgrading_ = INVALID_TXN_ID;
  }

  if (txn_finished) {
    // 事务已经结束，无需恢复旧 S，直接删除升级请求并推动后继请求。
    request_queue->request_queue_.remove(request);
    delete request;
    GrantNewLocksIfPossible(request_queue.get());
    request_queue->cv_.notify_all();
    return false;
  }

  request_queue->cv_.notify_all();
  return true;
}

/**
 * 判断同一资源上的两个锁模式能否同时处于 granted 状态。
 * l1/l2 分别可以理解为“待授予模式”和“已授予模式”；兼容关系是对称的，例如 S 与 IS、IS 与 S 都兼容。
 * 该函数只回答模式兼容性，不考虑 FIFO。FIFO 由 GrantNewLocksIfPossible() 的外层顺序扫描保证。
 */
auto LockManager::AreLocksCompatible(LockMode l1, LockMode l2) -> bool {
  // 多粒度锁兼容矩阵：只有两个锁模式可以同时作用在同一资源上时，才返回 true。
  // 这里使用显式分支，而不是依赖枚举下标，以免 LockMode 的声明顺序变化或填表时发生错位。
  switch (l1) {
    case LockMode::SHARED:
      // S 允许其他事务读取整个资源，或者声明将读取其下层资源。
      return l2 == LockMode::SHARED || l2 == LockMode::INTENTION_SHARED;
    case LockMode::EXCLUSIVE:
      // X 独占整个资源，与任何其他锁模式都不兼容。
      return false;
    case LockMode::INTENTION_SHARED:
      // IS 仅表示准备读取下层资源，因此除了 X 之外均兼容。
      return l2 != LockMode::EXCLUSIVE;
    case LockMode::INTENTION_EXCLUSIVE:
      // IX 只与另一个意向锁 IS/IX 兼容。
      return l2 == LockMode::INTENTION_SHARED || l2 == LockMode::INTENTION_EXCLUSIVE;
    case LockMode::SHARED_INTENTION_EXCLUSIVE:
      // SIX 已经包含当前资源上的 S，仅允许其他事务持有 IS。
      return l2 == LockMode::INTENTION_SHARED;
  }

  UNREACHABLE("Unsupported lock mode");
}

/**
 * 判断“已经持有 curr，想改成 requested”是否属于项目允许的升级边。
 * 合法边只有：IS -> S/X/IX/SIX、S -> X/SIX、IX -> X/SIX、SIX -> X。
 * 相同模式由 LockTable()/LockRow() 提前按幂等请求处理，因此这里不把 curr == requested 当作升级。
 */
auto LockManager::CanLockUpgrade(LockMode curr_lock_mode, LockMode requested_lock_mode) -> bool {
  // 相同模式不需要升级，应由 LockTable/LockRow 在调用本函数前直接返回成功。
  // 这里只接受项目说明中明确允许的锁升级边。
  switch (curr_lock_mode) {
    case LockMode::INTENTION_SHARED:
      return requested_lock_mode == LockMode::SHARED || requested_lock_mode == LockMode::EXCLUSIVE ||
             requested_lock_mode == LockMode::INTENTION_EXCLUSIVE ||
             requested_lock_mode == LockMode::SHARED_INTENTION_EXCLUSIVE;
    case LockMode::SHARED:
      return requested_lock_mode == LockMode::EXCLUSIVE || requested_lock_mode == LockMode::SHARED_INTENTION_EXCLUSIVE;
    case LockMode::INTENTION_EXCLUSIVE:
      return requested_lock_mode == LockMode::EXCLUSIVE || requested_lock_mode == LockMode::SHARED_INTENTION_EXCLUSIVE;
    case LockMode::SHARED_INTENTION_EXCLUSIVE:
      return requested_lock_mode == LockMode::EXCLUSIVE;
    case LockMode::EXCLUSIVE:
      // X 已经是最强锁模式，不存在更强的目标模式。
      return false;
  }

  UNREACHABLE("Unsupported lock mode");
}

/**
 * 检查事务是否“允许新取得”指定模式的锁。这里不判断资源兼容性，只实现隔离级别和 2PL 状态规则：
 *
 * - RR：GROWING 可取所有锁，SHRINKING 不可再取锁；
 * - RC：GROWING 可取所有锁，SHRINKING 只可取 S/IS；
 * - RU：只在 GROWING 允许 X/IX，S/IS/SIX 在任何阶段都非法。
 *
 * 返回 false 只用于事务已经 ABORTED/COMMITTED 的情况；主动违反规则时必须设置 ABORTED 并抛出准确异常。
 */
auto LockManager::CanTxnTakeLock(Transaction *txn, LockMode lock_mode) -> bool {
  const auto txn_state = txn->GetState();
  const auto isolation_level = txn->GetIsolationLevel();

  // 已经结束的事务不能再申请锁。ABORTED 事务可能是在等待锁期间被死锁检测线程终止的，
  // 此时调用者应停止等待并返回 false，而不是再次抛出异常。
  if (txn_state == TransactionState::ABORTED || txn_state == TransactionState::COMMITTED) {
    return false;
  }

  // READ_UNCOMMITTED 只需要写相关的 IX/X 锁。S、IS 和 SIX 都包含“共享读”的语义，
  // 因而无论事务处于哪个阶段，请求这些锁都属于非法操作。
  if (isolation_level == IsolationLevel::READ_UNCOMMITTED &&
      (lock_mode == LockMode::SHARED || lock_mode == LockMode::INTENTION_SHARED ||
       lock_mode == LockMode::SHARED_INTENTION_EXCLUSIVE)) {
    txn->SetState(TransactionState::ABORTED);
    throw TransactionAbortException(txn->GetTransactionId(), AbortReason::LOCK_SHARED_ON_READ_UNCOMMITTED);
  }

  // GROWING 阶段允许继续获得符合当前隔离级别的锁。
  if (txn_state == TransactionState::GROWING) {
    return true;
  }

  // 到这里事务必然处于 SHRINKING 阶段。READ_COMMITTED 为了支持逐条读取并及时释放行锁，
  // 允许在该阶段继续获取 S/IS；其余隔离级别和锁模式都违反两阶段锁规则。
  if (isolation_level == IsolationLevel::READ_COMMITTED &&
      (lock_mode == LockMode::SHARED || lock_mode == LockMode::INTENTION_SHARED)) {
    return true;
  }

  txn->SetState(TransactionState::ABORTED);
  throw TransactionAbortException(txn->GetTransactionId(), AbortReason::LOCK_ON_SHRINKING);
}

/**
 * 检查行锁是否有合法的上层表锁保护。
 *
 * 多粒度锁的含义可以这样理解：在某行上加锁之前，事务必须先在表上“声明”对子资源的访问意图。
 * - 行 S：IS 足够；S/X 已覆盖整表；IX/SIX 具有更强的下层写权限，也允许读取该行；
 * - 行 X：必须由 IX、SIX 或 X 保护，单独的 IS/S 都没有声明下层写操作。
 *
 * 调用者在读取 Transaction 的锁集合前负责持有 txn latch。
 */
auto LockManager::CheckAppropriateLockOnTable(Transaction *txn, const table_oid_t &oid,
                                              LockMode row_lock_mode) -> bool {
  // 获取行 S 锁前，事务必须已经声明会访问该表的下层资源，或者直接持有覆盖整张表的锁。
  // IX/SIX/X 也允许获取行 S：它们拥有或声明了更强的下层写权限，当然也能读取该行。
  if (row_lock_mode == LockMode::SHARED) {
    return txn->IsTableIntentionSharedLocked(oid) || txn->IsTableIntentionExclusiveLocked(oid) ||
           txn->IsTableSharedLocked(oid) || txn->IsTableSharedIntentionExclusiveLocked(oid) ||
           txn->IsTableExclusiveLocked(oid);
  }

  // 获取行 X 锁前必须持有能够覆盖或声明下层写操作的表锁。
  if (row_lock_mode == LockMode::EXCLUSIVE) {
    return txn->IsTableIntentionExclusiveLocked(oid) || txn->IsTableSharedIntentionExclusiveLocked(oid) ||
           txn->IsTableExclusiveLocked(oid);
  }

  // 行上不允许使用 IS、IX、SIX。具体的事务终止和异常由 LockRow() 统一处理。
  return false;
}

/**
 * 在一个资源队列中，按 FIFO 尽可能授予队首连续的一批兼容请求。
 *
 * 例子：队列为 [T1:S granted, T2:S waiting, T3:X waiting, T4:S waiting]：
 * - T2 与所有 granted S 兼容，因此可以变成 granted；
 * - 扫到 T3 时，X 与 T1/T2 的 S 冲突，于是立即停止；
 * - 虽然 T4:S 本身与已有 S 兼容，也不能越过排在前面的 T3，否则会破坏 FIFO，并可能让 T3 饥饿。
 *
 * 本函数只修改请求队列，不修改 Transaction lock set。每个被唤醒的 LockTable()/LockRow() 调用线程
 * 会在确认事务仍有效后自行完成 bookkeeping。调用者必须已经持有该队列的 latch。
 */
void LockManager::GrantNewLocksIfPossible(LockRequestQueue *lock_request_queue) {
  // 调用者必须已经持有 lock_request_queue->latch_。如果在这里重复加锁，
  // LockTable/UnlockTable 等持锁调用方会发生自锁死。
  bool granted_new_lock = false;

  // 外层循环按请求入队顺序扫描，保证等待请求不会绕过排在它前面的请求。
  for (auto *request : lock_request_queue->request_queue_) {
    if (request->granted_) {
      continue;
    }

    // 内层循环将当前 waiting 请求与所有 granted 请求比较。
    // 前面刚被本轮授予的请求也已经 granted，因此自然会参加后续请求的兼容性检查。
    bool compatible = true;
    for (auto *granted_request : lock_request_queue->request_queue_) {
      if (!granted_request->granted_) {
        continue;
      }

      if (!AreLocksCompatible(request->lock_mode_, granted_request->lock_mode_)) {
        compatible = false;
        break;
      }
    }

    if (!compatible) {
      // 最早的等待请求尚不能获得锁时，后面的请求即使兼容也不能越过它，
      // 否则会破坏项目要求的 FIFO 授予顺序。
      break;
    }

    // 走到这里说明该请求既位于当前可处理的 FIFO 位置，又与所有持有者兼容。
    request->granted_ = true;
    // upgrading_ 只表示“正在等待升级”的事务。升级请求一旦被授予，就应立即释放该标记，
    // 避免另一个线程在已完成升级、但原等待线程尚未被调度醒来的短暂窗口里误报升级冲突。
    if (lock_request_queue->upgrading_ == request->txn_id_) {
      lock_request_queue->upgrading_ = INVALID_TXN_ID;
    }
    granted_new_lock = true;
  }

  // 使用 notify_all 而不是 notify_one，因为一次扫描可能同时授予多个兼容请求。
  // 每个线程会在重新取得 queue latch 后检查自己的 granted_；没有被授予的线程会继续睡眠。
  if (granted_new_lock) {
    lock_request_queue->cv_.notify_all();
  }
}

/**
 * 回收仍留在所有资源队列中的 LockRequest。
 *
 * 正常锁会在 UnlockTable()/UnlockRow() 中逐个 delete；但测试可能直接销毁仍持锁的事务，或者程序在退出时
 * 还有尚未显式提交的请求。由于 request_queue_ 保存的是裸指针，LockManager 析构时必须提供最终兜底清理。
 *
 * 析构函数会先停止并 join 死锁检测线程，再调用本函数；外部也必须保证没有业务线程继续访问 LockManager。
 * 在这个前提下，可以依次锁住 map 和每个 queue，删除请求并清空容器，而不需要再更新已经可能销毁的事务对象。
 */
void LockManager::UnlockAll() {
  // LockRequestQueue 使用裸指针保存请求。正常运行时请求由 UnlockTable/UnlockRow 删除；
  // LockManager 析构时还可能存在测试或未提交事务留下的请求，因此这里统一回收。
  // 析构要求所有使用 LockManager 的工作线程已经结束，不能与新的 Lock/Unlock 并发执行。
  {
    // 表请求与行请求位于两个独立 map，分别清理，避免同时持有两把全局 map latch。
    std::lock_guard<std::mutex> table_map_guard(table_lock_map_latch_);
    for (auto &[oid, request_queue] : table_lock_map_) {
      static_cast<void>(oid);
      std::lock_guard<std::mutex> queue_guard(request_queue->latch_);
      for (auto *request : request_queue->request_queue_) {
        delete request;
      }
      request_queue->request_queue_.clear();
      request_queue->upgrading_ = INVALID_TXN_ID;
    }
    table_lock_map_.clear();
  }

  {
    // 每个请求只属于一个队列，因此遍历队列逐个 delete 不会发生重复释放。
    std::lock_guard<std::mutex> row_map_guard(row_lock_map_latch_);
    for (auto &[rid, request_queue] : row_lock_map_) {
      static_cast<void>(rid);
      std::lock_guard<std::mutex> queue_guard(request_queue->latch_);
      for (auto *request : request_queue->request_queue_) {
        delete request;
      }
      request_queue->request_queue_.clear();
      request_queue->upgrading_ = INVALID_TXN_ID;
    }
    row_lock_map_.clear();
  }
}

void LockManager::AddEdge(txn_id_t t1, txn_id_t t2) {
  // waits_for_ 可能同时被后台检测线程和图 API 测试访问，因此所有读写都必须由 waits_for_latch_ 保护。
  std::lock_guard<std::mutex> graph_guard(waits_for_latch_);
  auto &neighbors = waits_for_[t1];

  // 同一等待关系可能从多个扫描路径重复发现。邻接表必须保持幂等，否则既浪费空间，
  // 也会让 GetEdgeList() 和后续 DFS 重复处理完全相同的边。
  if (std::find(neighbors.begin(), neighbors.end(), t2) == neighbors.end()) {
    neighbors.push_back(t2);
  }
}

void LockManager::RemoveEdge(txn_id_t t1, txn_id_t t2) {
  std::lock_guard<std::mutex> graph_guard(waits_for_latch_);
  const auto source_it = waits_for_.find(t1);
  if (source_it == waits_for_.end()) {
    // 删除不存在的边是幂等操作，直接返回即可。
    return;
  }

  auto &neighbors = source_it->second;
  neighbors.erase(std::remove(neighbors.begin(), neighbors.end(), t2), neighbors.end());
  if (neighbors.empty()) {
    // 没有出边的节点无需作为 key 留在邻接表中；它作为其他边的终点时仍会被 HasCycle() 收集到。
    waits_for_.erase(source_it);
  }
}

auto LockManager::FindCycle(txn_id_t source_txn, std::vector<txn_id_t> &path, std::unordered_set<txn_id_t> &on_path,
                            std::unordered_set<txn_id_t> &visited, txn_id_t *abort_txn_id) -> bool {
  // visited 表示“这个节点已经被某次 DFS 搜索过”；on_path 只表示“这个节点仍在当前递归栈中”。
  // 两者不能合并：指向 visited 但不在 on_path 中的节点只是交叉边/前向边，不代表存在环。
  visited.insert(source_txn);
  on_path.insert(source_txn);
  path.push_back(source_txn);

  const auto graph_it = waits_for_.find(source_txn);
  if (graph_it != waits_for_.end()) {
    // waits_for_ 使用 unordered_map/vector，原始遍历顺序不稳定。复制并排序邻居，保证每次 DFS
    // 都以相同顺序找到第一个环，使测试结果和牺牲者选择具有确定性。
    auto neighbors = graph_it->second;
    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

    for (const auto neighbor : neighbors) {
      if (on_path.find(neighbor) != on_path.end()) {
        // neighbor 已在当前递归路径中，当前边 source_txn -> neighbor 是一条返祖边，因此形成环。
        // path 可能还带有“进入环之前”的前缀，例如 0 -> 1 -> 2 -> 3 -> 1：
        // 真正的环只有 [1,2,3]，事务 0 不能参与 victim 选择。
        const auto cycle_begin = std::find(path.begin(), path.end(), neighbor);
        BUSTUB_ASSERT(cycle_begin != path.end(), "A node in on_path must also be present in the DFS path");
        *abort_txn_id = *std::max_element(cycle_begin, path.end());
        return true;
      }

      if (visited.find(neighbor) == visited.end() && FindCycle(neighbor, path, on_path, visited, abort_txn_id)) {
        // 子递归已经找到环并写入 abort_txn_id，直接逐层向上传播成功结果。
        return true;
      }
    }
  }

  // source_txn 的所有出边均已探索且没有找到环，将它从当前递归路径移除。
  // 它仍保留在 visited 中，后续 DFS 遇到它时无需重复搜索其子图。
  path.pop_back();
  on_path.erase(source_txn);
  return false;
}

auto LockManager::HasCycle(txn_id_t *txn_id) -> bool {
  BUSTUB_ASSERT(txn_id != nullptr, "HasCycle requires a non-null output pointer");
  *txn_id = INVALID_TXN_ID;

  // FindCycle() 直接读取 waits_for_，因此 HasCycle() 在整个 DFS 期间持有图 latch，
  // 防止 AddEdge()/RemoveEdge()/RunCycleDetection() 同时改变邻接表并使迭代状态失效。
  std::lock_guard<std::mutex> graph_guard(waits_for_latch_);

  // 既收集邻接表的 source，也收集只作为 destination 出现的节点。
  // 后者虽然没有出边，仍应作为图节点参与统一、确定的 DFS 起点排序。
  std::vector<txn_id_t> nodes;
  for (const auto &[source, neighbors] : waits_for_) {
    nodes.push_back(source);
    nodes.insert(nodes.end(), neighbors.begin(), neighbors.end());
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

  std::vector<txn_id_t> path;
  std::unordered_set<txn_id_t> on_path;
  std::unordered_set<txn_id_t> visited;

  // 按事务 ID 升序选择 DFS 起点；FindCycle() 内部也按升序访问邻居。
  // 找到环时，FindCycle() 会把“该环中最大的事务 ID”写入 txn_id。
  for (const auto node : nodes) {
    if (visited.find(node) == visited.end() && FindCycle(node, path, on_path, visited, txn_id)) {
      return true;
    }
  }

  return false;
}

auto LockManager::GetEdgeList() -> std::vector<std::pair<txn_id_t, txn_id_t>> {
  std::lock_guard<std::mutex> graph_guard(waits_for_latch_);
  std::vector<std::pair<txn_id_t, txn_id_t>> edges;

  // 将邻接表拍平成边集合。例如 waits_for_[1] = {2, 3} 会产生 (1,2)、(1,3)。
  for (const auto &[source, neighbors] : waits_for_) {
    for (const auto destination : neighbors) {
      edges.emplace_back(source, destination);
    }
  }

  // unordered_map 和 vector 的插入顺序都不应影响对外结果；排序能让单元测试和调试输出保持确定。
  std::sort(edges.begin(), edges.end());
  return edges;
}

void LockManager::RunCycleDetection() {
  while (enable_cycle_detection_) {
    std::this_thread::sleep_for(cycle_detection_interval);

    // 析构函数可能在检测线程睡眠时把开关关闭。醒来后再次检查，避免关闭过程中多执行一轮扫描。
    if (!enable_cycle_detection_) {
      break;
    }

    // 先复制所有资源队列的 shared_ptr，然后释放全局 map latch。
    // 后续扫描单个队列时可能需要等待 queue latch，不能因此长期阻塞其他资源的 map 查找。
    std::vector<std::shared_ptr<LockRequestQueue>> request_queues;
    {
      std::lock_guard<std::mutex> table_map_guard(table_lock_map_latch_);
      request_queues.reserve(table_lock_map_.size());
      for (const auto &[oid, request_queue] : table_lock_map_) {
        static_cast<void>(oid);
        request_queues.push_back(request_queue);
      }
    }
    {
      std::lock_guard<std::mutex> row_map_guard(row_lock_map_latch_);
      request_queues.reserve(request_queues.size() + row_lock_map_.size());
      for (const auto &[rid, request_queue] : row_lock_map_) {
        static_cast<void>(rid);
        request_queues.push_back(request_queue);
      }
    }

    // 每轮都从当前锁队列全量重建图，不沿用上一轮的边。锁可能已经被释放或升级，增量维护很容易留下脏边。
    std::unordered_map<txn_id_t, std::vector<txn_id_t>> next_waits_for;
    auto add_edge_to_snapshot = [&next_waits_for](txn_id_t waiting_txn, txn_id_t blocking_txn) {
      if (waiting_txn == blocking_txn) {
        return;
      }
      auto &neighbors = next_waits_for[waiting_txn];
      if (std::find(neighbors.begin(), neighbors.end(), blocking_txn) == neighbors.end()) {
        neighbors.push_back(blocking_txn);
      }
    };

    auto transaction_is_aborted = [this](txn_id_t txn_id) {
      return txn_manager_->GetTransaction(txn_id)->GetState() == TransactionState::ABORTED;
    };

    for (const auto &request_queue : request_queues) {
      // request_queue_、granted_ 和 upgrading_ 都由队列自己的 latch 保护。
      std::lock_guard<std::mutex> queue_guard(request_queue->latch_);

      for (auto waiting_it = request_queue->request_queue_.begin(); waiting_it != request_queue->request_queue_.end();
           ++waiting_it) {
        auto *waiting_request = *waiting_it;
        if (waiting_request->granted_ || transaction_is_aborted(waiting_request->txn_id_)) {
          // 已授予请求不是等待者；已终止事务即将醒来清理请求，也不应再参与新一轮死锁选择。
          continue;
        }

        // 第一类阻塞者：当前资源上持有不兼容锁的事务。
        // 例如 waiting X 与 granted S 不兼容，就加入 waiting_txn -> shared_holder。
        for (auto *candidate : request_queue->request_queue_) {
          if (!candidate->granted_ || transaction_is_aborted(candidate->txn_id_)) {
            continue;
          }
          if (!AreLocksCompatible(waiting_request->lock_mode_, candidate->lock_mode_)) {
            add_edge_to_snapshot(waiting_request->txn_id_, candidate->txn_id_);
          }
        }

        // 第二类阻塞者：排在当前请求之前的普通 waiting 请求。
        // Task 1 严格遵守 FIFO，所以后来的兼容请求也不能越过前面的 waiter；升级请求同样会被插到普通
        // waiter 之前。把这类队列依赖加入图，才能检测由“资源冲突 + FIFO 顺序”共同形成的死锁。
        for (auto earlier_it = request_queue->request_queue_.begin(); earlier_it != waiting_it; ++earlier_it) {
          auto *earlier_request = *earlier_it;
          if (!earlier_request->granted_ && !transaction_is_aborted(earlier_request->txn_id_)) {
            add_edge_to_snapshot(waiting_request->txn_id_, earlier_request->txn_id_);
          }
        }
      }
    }

    {
      // 一次性替换正式图，使 HasCycle()/GetEdgeList() 不会看到只构建了一半的中间状态。
      std::lock_guard<std::mutex> graph_guard(waits_for_latch_);
      waits_for_ = std::move(next_waits_for);
    }

    // HasCycle() 将在下一步实现。完成后，这个循环会一次终止一个环中的最新事务，
    // 并从当前工作图删除它，再继续检查，直到本轮快照中的所有环都被打破。
    bool aborted_any_transaction = false;
    txn_id_t victim_txn_id = INVALID_TXN_ID;
    while (HasCycle(&victim_txn_id)) {
      auto *victim_txn = txn_manager_->GetTransaction(victim_txn_id);
      victim_txn->LockTxn();
      victim_txn->SetState(TransactionState::ABORTED);
      victim_txn->UnlockTxn();
      aborted_any_transaction = true;

      // 删除 victim 的全部出边和入边。否则下一次 HasCycle() 仍会在同一个环中选到已经终止的事务。
      std::lock_guard<std::mutex> graph_guard(waits_for_latch_);
      waits_for_.erase(victim_txn_id);
      for (auto graph_it = waits_for_.begin(); graph_it != waits_for_.end();) {
        auto &neighbors = graph_it->second;
        neighbors.erase(std::remove(neighbors.begin(), neighbors.end(), victim_txn_id), neighbors.end());
        if (neighbors.empty()) {
          graph_it = waits_for_.erase(graph_it);
        } else {
          ++graph_it;
        }
      }
    }

    if (aborted_any_transaction) {
      // 死锁牺牲者可能睡在任意一个表/行队列。统一唤醒所有队列最简单可靠；
      // 非牺牲者的 wait 谓词仍为 false，会自动继续睡眠，不会错误取得锁。
      for (const auto &request_queue : request_queues) {
        request_queue->cv_.notify_all();
      }
    }
  }
}

}  // namespace bustub
