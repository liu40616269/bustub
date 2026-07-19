#include "execution/executors/topn_executor.h"

#include <algorithm>
#include <utility>

#include "common/macros.h"

namespace bustub {

auto TopNExecutor::TopNComparator::operator()(const TopNEntry &left, const TopNEntry &right) const -> bool {
  const auto &child_schema = plan_->GetChildPlan()->OutputSchema();

  // 与 SortExecutor 相同，多个 ORDER BY 表达式按照字典序比较。
  // 该函数返回 true 表示 left 在最终结果中应排在 right 前面；priority_queue
  // 因而会把最终顺序中更靠后的“较差记录”放在堆顶。
  for (const auto &[order_type, expression] : plan_->GetOrderBy()) {
    const auto left_value = expression->Evaluate(&left.tuple_, child_schema);
    const auto right_value = expression->Evaluate(&right.tuple_, child_schema);

    BUSTUB_ASSERT(order_type != OrderByType::INVALID, "TopN order must be DEFAULT, ASC, or DESC");
    const bool descending = order_type == OrderByType::DESC;

    // 与 SortExecutor 保持相同的 NULL 顺序：ASC/DEFAULT 时 NULL 在末尾，DESC 时在开头。
    if (left_value.IsNull() || right_value.IsNull()) {
      if (left_value.IsNull() && right_value.IsNull()) {
        continue;
      }
      if (descending) {
        return left_value.IsNull();
      }
      return !left_value.IsNull();
    }

    if (left_value.CompareEquals(right_value) == CmpBool::CmpTrue) {
      continue;
    }

    if (descending) {
      return left_value.CompareGreaterThan(right_value) == CmpBool::CmpTrue;
    }
    return left_value.CompareLessThan(right_value) == CmpBool::CmpTrue;
  }

  // 所有排序 Key 都相等时，两条记录在排序意义上等价。
  return false;
}

TopNExecutor::TopNExecutor(ExecutorContext *exec_ctx, const TopNPlanNode *plan,
                           std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      child_executor_(std::move(child_executor)),
      top_entries_(TopNComparator{plan}) {
  BUSTUB_ASSERT(plan_ != nullptr, "TopN requires a valid plan");
  // 测试框架会先用 nullptr 构造 TopNExecutor，再通过 SetChildExecutor() 注入检查器，
  // 因此这里不能断言 child_executor_ 非空；Init() 时再检查。
}

void TopNExecutor::Init() {
  BUSTUB_ASSERT(child_executor_ != nullptr, "TopN requires a child executor before initialization");

  // Executor 可能被重复 Init，重新创建带有相同比较器的空堆，并清除旧输出状态。
  top_entries_ = TopNHeap{TopNComparator{plan_}};
  output_entries_.clear();
  next_tuple_idx_ = 0;

  child_executor_->Init();

  // LIMIT 0 不需要读取 child，也不会在堆中保存任何记录。
  if (plan_->GetN() == 0) {
    return;
  }

  Tuple input_tuple;
  RID input_rid;
  const TopNComparator comparator{plan_};

  while (child_executor_->Next(&input_tuple, &input_rid)) {
    TopNEntry candidate{std::move(input_tuple), input_rid};

    // 堆未满时，每条记录都属于当前 Top N，直接加入。
    if (top_entries_.size() < plan_->GetN()) {
      top_entries_.push(std::move(candidate));
      continue;
    }

    // 堆已满时，只有 candidate 在最终顺序中优于堆顶的“最差记录”才值得保留。
    // 先 pop 再 push，保证 top_entries_ 在任何可观察时刻都不会超过 N 条。
    if (comparator(candidate, top_entries_.top())) {
      top_entries_.pop();
      top_entries_.push(std::move(candidate));
    }
  }

  // priority_queue 依次弹出的是“最差 -> 最好”。先按该顺序取出，再整体反转，
  // 得到 SQL ORDER BY 所要求的“最好 -> 最差”最终输出顺序。
  output_entries_.reserve(top_entries_.size());
  while (!top_entries_.empty()) {
    output_entries_.push_back(top_entries_.top());
    top_entries_.pop();
  }
  std::reverse(output_entries_.begin(), output_entries_.end());
}

auto TopNExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  if (next_tuple_idx_ >= output_entries_.size()) {
    return false;
  }

  // TopN 产生的逻辑结果仍对应 child 的原 Tuple，因此同时向上传递保存下来的 RID。
  *tuple = output_entries_[next_tuple_idx_].tuple_;
  *rid = output_entries_[next_tuple_idx_].rid_;
  next_tuple_idx_++;
  return true;
}

auto TopNExecutor::GetNumInHeap() -> size_t { return top_entries_.size(); };

}  // namespace bustub
