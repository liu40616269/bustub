//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// limit_executor.cpp
//
// Identification: src/execution/limit_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/limit_executor.h"

#include <utility>

#include "common/macros.h"

namespace bustub {

LimitExecutor::LimitExecutor(ExecutorContext *exec_ctx, const LimitPlanNode *plan,
                             std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {
  BUSTUB_ASSERT(plan_ != nullptr, "Limit requires a valid plan");
  BUSTUB_ASSERT(child_executor_ != nullptr, "Limit requires a child executor");
}

void LimitExecutor::Init() {
  // Executor 可能被重复 Init。每次新执行都要从 child 开头读取，并重新计算输出数量。
  child_executor_->Init();
  num_emitted_ = 0;
}

auto LimitExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  // 达到 LIMIT 后立即停止，不再向 child 多取一条无用数据。
  if (num_emitted_ >= plan_->GetLimit()) {
    return false;
  }

  // Limit 是流式算子，不需要缓存 Tuple；child 产生什么 Tuple 和 RID，就直接向上传递。
  if (!child_executor_->Next(tuple, rid)) {
    // child 的记录数可能少于 LIMIT，此时以 child 耗尽为准。
    return false;
  }

  // 只有 child 真正产生一条记录后才增加计数，避免失败的 Next() 占用 LIMIT 名额。
  num_emitted_++;
  return true;
}

}  // namespace bustub
