#include "execution/executors/sort_executor.h"

#include <algorithm>
#include <utility>

#include "common/macros.h"

namespace bustub {

SortExecutor::SortExecutor(ExecutorContext *exec_ctx, const SortPlanNode *plan,
                           std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {
  BUSTUB_ASSERT(plan_ != nullptr, "Sort requires a valid plan");
  BUSTUB_ASSERT(child_executor_ != nullptr, "Sort requires a child executor");
}

auto SortExecutor::CompareTuples(const Tuple &left, const Tuple &right) const -> bool {
  const auto &child_schema = child_executor_->GetOutputSchema();

  // 多列 ORDER BY 使用字典序比较：只有当前排序列相等时，才继续比较下一列。
  for (const auto &[order_type, expression] : plan_->GetOrderBy()) {
    const auto left_value = expression->Evaluate(&left, child_schema);
    const auto right_value = expression->Evaluate(&right, child_schema);

    BUSTUB_ASSERT(order_type != OrderByType::INVALID, "Sort order must be DEFAULT, ASC, or DESC");
    const bool descending = order_type == OrderByType::DESC;

    // 为 NULL 明确定义严格弱序，避免把 NULL 与所有非 NULL 值都错误地视为相等。
    // 这里采用 PostgreSQL 的默认行为：ASC/DEFAULT 时 NULL 在末尾，DESC 时 NULL 在开头。
    if (left_value.IsNull() || right_value.IsNull()) {
      if (left_value.IsNull() && right_value.IsNull()) {
        continue;
      }
      if (descending) {
        return left_value.IsNull();
      }
      return !left_value.IsNull();
    }

    // 当前列相等，无法决定两个 Tuple 的先后顺序，继续比较下一个 ORDER BY 表达式。
    if (left_value.CompareEquals(right_value) == CmpBool::CmpTrue) {
      continue;
    }

    // 当前列是第一个不同的排序列，它就决定最终顺序。
    if (descending) {
      return left_value.CompareGreaterThan(right_value) == CmpBool::CmpTrue;
    }
    return left_value.CompareLessThan(right_value) == CmpBool::CmpTrue;
  }

  // 所有排序 Key 都相等时，两条记录在排序意义上等价；std::sort 要求此时返回 false。
  return false;
}

void SortExecutor::Init() {
  // Executor 可能被重复 Init，必须丢弃上一次保存的 Tuple 和输出进度。
  sorted_entries_.clear();
  next_tuple_idx_ = 0;

  child_executor_->Init();

  Tuple input_tuple;
  RID input_rid;

  // Sort 是阻塞算子：必须先读取全部输入，才能知道哪一条 Tuple 应当最先输出。
  while (child_executor_->Next(&input_tuple, &input_rid)) {
    sorted_entries_.emplace_back(std::move(input_tuple), input_rid);
  }

  // std::sort 通常使用 Introsort；比较器按 Plan 中的多个 ORDER BY 表达式执行字典序比较。
  std::sort(sorted_entries_.begin(), sorted_entries_.end(),
            [this](const auto &left, const auto &right) { return CompareTuples(left.first, right.first); });
}

auto SortExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  if (next_tuple_idx_ >= sorted_entries_.size()) {
    // 排序后的所有 Tuple 都已经向上层输出。
    return false;
  }

  // 返回当前位置的 Tuple 和它原先由 child 产生的 RID，然后推进到下一条。
  *tuple = sorted_entries_[next_tuple_idx_].first;
  *rid = sorted_entries_[next_tuple_idx_].second;
  next_tuple_idx_++;
  return true;
}

}  // namespace bustub
