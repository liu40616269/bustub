//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// nested_loop_join_executor.cpp
//
// Identification: src/execution/nested_loop_join_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/nested_loop_join_executor.h"

#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "common/exception.h"
#include "type/value_factory.h"

namespace bustub {

NestedLoopJoinExecutor::NestedLoopJoinExecutor(ExecutorContext *exec_ctx, const NestedLoopJoinPlanNode *plan,
                                               std::unique_ptr<AbstractExecutor> &&left_executor,
                                               std::unique_ptr<AbstractExecutor> &&right_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      left_executor_(std::move(left_executor)),
      right_executor_(std::move(right_executor)) {
  BUSTUB_ASSERT(left_executor_ != nullptr, "Nested loop join requires a left child executor");
  BUSTUB_ASSERT(right_executor_ != nullptr, "Nested loop join requires a right child executor");

  if (!(plan->GetJoinType() == JoinType::LEFT || plan->GetJoinType() == JoinType::INNER)) {
    // Note for 2023 Spring: You ONLY need to implement left join and inner join.
    throw bustub::NotImplementedException(fmt::format("join type {} not supported", plan->GetJoinType()));
  }
}

void NestedLoopJoinExecutor::Init() {
  // 左 child 只需顺序向前读取；右 child 会在 Next() 中针对每一条新左 Tuple 重新 Init，
  // 从而实现“每条左记录都与完整右侧输入比较”的双层循环。
  left_executor_->Init();
  right_executor_->Init();

  // Init() 可能被重复调用，必须丢弃上一次执行暂停时保存的左 Tuple 和匹配状态。
  left_tuple_ = Tuple{};
  has_left_tuple_ = false;
  left_tuple_matched_ = false;
}

auto NestedLoopJoinExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  // Nested Loop Join 可以看成下面的双层循环：
  // for (left_tuple : left_child) {
  //   for (right_tuple : right_child) {
  //     检查 Join 谓词并输出匹配结果；
  //   }
  // }
  //
  // 但 Executor::Next() 每次最多只能输出一条结果，所以一次调用可能会在内层循环中途返回。
  // left_tuple_、has_left_tuple_ 和 right child 当前的迭代位置共同保存了这个“暂停点”。
  while (true) {
    // 第一步：如果当前没有正在处理的左 Tuple，就从左 child 取下一条。
    if (!has_left_tuple_) {
      RID left_rid;
      if (!left_executor_->Next(&left_tuple_, &left_rid)) {
        // 左侧输入已经耗尽，不可能再产生任何 Join 结果。
        return false;
      }

      has_left_tuple_ = true;
      left_tuple_matched_ = false;

      // 每一条左 Tuple 都必须和“完整的”右侧输入比较，因此要把右 child 重置到开头。
      right_executor_->Init();
    }

    // 第二步：从右 child 当前的位置继续扫描，寻找与 left_tuple_ 匹配的 Tuple。
    Tuple right_tuple;
    RID right_rid;
    while (right_executor_->Next(&right_tuple, &right_rid)) {
      const auto predicate_value = plan_->Predicate()->EvaluateJoin(&left_tuple_, left_executor_->GetOutputSchema(),
                                                                    &right_tuple, right_executor_->GetOutputSchema());

      // SQL 中谓词结果为 NULL（UNKNOWN）也不算匹配，只有明确为 TRUE 才输出。
      if (predicate_value.IsNull() || !predicate_value.GetAs<bool>()) {
        continue;
      }

      left_tuple_matched_ = true;

      // 输出 Schema 的列顺序是：左 child 的所有列，随后是右 child 的所有列。
      std::vector<Value> values;
      const auto &left_schema = left_executor_->GetOutputSchema();
      const auto &right_schema = right_executor_->GetOutputSchema();
      values.reserve(left_schema.GetColumnCount() + right_schema.GetColumnCount());

      for (uint32_t column_idx = 0; column_idx < left_schema.GetColumnCount(); column_idx++) {
        values.push_back(left_tuple_.GetValue(&left_schema, column_idx));
      }
      for (uint32_t column_idx = 0; column_idx < right_schema.GetColumnCount(); column_idx++) {
        values.push_back(right_tuple.GetValue(&right_schema, column_idx));
      }

      *tuple = Tuple{values, &GetOutputSchema()};

      // 此时不能清除 has_left_tuple_：当前左 Tuple 后面可能还有其他匹配项。
      // 下一次调用 Next() 时，right child 会从当前位置继续向后扫描。
      return true;
    }

    // 第三步：右侧已经扫描完。对于 LEFT JOIN，如果当前左 Tuple 一次都没有匹配，
    // 仍需输出一行，并将右侧的每一列补成相应类型的 SQL NULL。
    if (plan_->GetJoinType() == JoinType::LEFT && !left_tuple_matched_) {
      std::vector<Value> values;
      const auto &left_schema = left_executor_->GetOutputSchema();
      const auto &right_schema = right_executor_->GetOutputSchema();
      values.reserve(left_schema.GetColumnCount() + right_schema.GetColumnCount());

      for (uint32_t column_idx = 0; column_idx < left_schema.GetColumnCount(); column_idx++) {
        values.push_back(left_tuple_.GetValue(&left_schema, column_idx));
      }
      for (uint32_t column_idx = 0; column_idx < right_schema.GetColumnCount(); column_idx++) {
        values.push_back(ValueFactory::GetNullValueByType(right_schema.GetColumn(column_idx).GetType()));
      }

      *tuple = Tuple{values, &GetOutputSchema()};

      // 这条左 Tuple 已经处理完成。先清除状态，再返回补 NULL 的结果；
      // 下一次调用会读取下一条左 Tuple，而不会重复输出当前结果。
      has_left_tuple_ = false;
      return true;
    }

    // INNER JOIN 扫描完右侧却没有更多匹配，或者 LEFT JOIN 当前左 Tuple 已经匹配过，
    // 都无需额外输出。清除状态并通过外层 while 继续处理下一条左 Tuple。
    has_left_tuple_ = false;
  }
}

}  // namespace bustub
