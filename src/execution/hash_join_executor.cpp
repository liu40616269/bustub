//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// hash_join_executor.cpp
//
// Identification: src/execution/hash_join_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/hash_join_executor.h"

#include <vector>

#include "common/exception.h"
#include "type/value_factory.h"

namespace bustub {

HashJoinExecutor::HashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                                   std::unique_ptr<AbstractExecutor> &&left_child,
                                   std::unique_ptr<AbstractExecutor> &&right_child)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      left_child_(std::move(left_child)),
      right_child_(std::move(right_child)) {
  BUSTUB_ASSERT(plan_ != nullptr, "Hash join requires a valid plan");
  BUSTUB_ASSERT(left_child_ != nullptr, "Hash join requires a left child executor");
  BUSTUB_ASSERT(right_child_ != nullptr, "Hash join requires a right child executor");
  BUSTUB_ASSERT(plan_->LeftJoinKeyExpressions().size() == plan_->RightJoinKeyExpressions().size(),
                "The left and right hash join keys must have the same number of columns");

  if (!(plan_->GetJoinType() == JoinType::LEFT || plan_->GetJoinType() == JoinType::INNER)) {
    // Note for 2023 Spring: You ONLY need to implement left join and inner join.
    throw bustub::NotImplementedException(fmt::format("join type {} not supported", plan_->GetJoinType()));
  }
}

auto HashJoinExecutor::MakeJoinKey(const Tuple &tuple, const Schema &schema,
                                   const std::vector<AbstractExpressionRef> &expressions) const -> HashJoinKey {
  std::vector<Value> values;
  values.reserve(expressions.size());

  // 每个表达式从 Tuple 中取出一个连接列；多个结果按顺序共同组成复合 Key。
  for (const auto &expression : expressions) {
    values.push_back(expression->Evaluate(&tuple, schema));
  }

  return HashJoinKey{std::move(values)};
}

auto HashJoinExecutor::MakeOutputTuple(const Tuple &left_tuple, const Tuple *right_tuple) const -> Tuple {
  const auto &left_schema = left_child_->GetOutputSchema();
  const auto &right_schema = right_child_->GetOutputSchema();

  std::vector<Value> values;
  values.reserve(left_schema.GetColumnCount() + right_schema.GetColumnCount());

  // HashJoinPlan 的固定输出布局是：先放左 child 的全部列。
  for (uint32_t column_idx = 0; column_idx < left_schema.GetColumnCount(); column_idx++) {
    values.push_back(left_tuple.GetValue(&left_schema, column_idx));
  }

  // 有匹配项时复制右 Tuple 的列；LEFT JOIN 没有匹配项时，按每列类型创建 SQL NULL。
  for (uint32_t column_idx = 0; column_idx < right_schema.GetColumnCount(); column_idx++) {
    if (right_tuple != nullptr) {
      values.push_back(right_tuple->GetValue(&right_schema, column_idx));
    } else {
      values.push_back(ValueFactory::GetNullValueByType(right_schema.GetColumn(column_idx).GetType()));
    }
  }

  return Tuple{values, &GetOutputSchema()};
}

void HashJoinExecutor::Init() {
  // Executor 可能被重复 Init。先清除指向旧哈希表元素的指针，再清空上一次执行的数据。
  current_matches_ = nullptr;
  match_index_ = 0;
  left_tuple_ = Tuple{};
  hash_table_.clear();

  // 左 child 在 Next() 中作为探测端逐条读取；右 child 在这里作为构建端一次性读完。
  left_child_->Init();
  right_child_->Init();

  Tuple right_tuple;
  RID right_rid;

  // Hash Join 的构建阶段是阻塞的：必须先把整个右侧输入组织成哈希表，才能开始输出结果。
  while (right_child_->Next(&right_tuple, &right_rid)) {
    auto key = MakeJoinKey(right_tuple, right_child_->GetOutputSchema(), plan_->RightJoinKeyExpressions());

    // SQL 中 NULL = NULL 的结果是 UNKNOWN，而不是 TRUE。含 NULL 的右 Key 永远不会匹配，
    // 所以无需把它存入哈希表。
    if (key.HasNull()) {
      continue;
    }

    // 相同 Key 可能对应多条右 Tuple。全部保存下来，探测时才能输出完整的多对多结果。
    hash_table_[std::move(key)].push_back(right_tuple);
  }
}

auto HashJoinExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  while (true) {
    // 第一步：如果当前左 Tuple 的匹配桶中还有右 Tuple，就继续输出下一条组合。
    // Next() 每次只能返回一条，因此 current_matches_ 和 match_index_ 保存了暂停位置。
    if (current_matches_ != nullptr && match_index_ < current_matches_->size()) {
      const auto &right_tuple = (*current_matches_)[match_index_];
      *tuple = MakeOutputTuple(left_tuple_, &right_tuple);
      match_index_++;

      // 当前桶已经输出完。哈希表本身仍然保留，但下一次调用应读取新的左 Tuple。
      if (match_index_ == current_matches_->size()) {
        current_matches_ = nullptr;
        match_index_ = 0;
      }

      return true;
    }

    // 第二步：当前没有尚未输出的匹配项，从左 child 取得下一条 Tuple 作为探测端。
    RID left_rid;
    if (!left_child_->Next(&left_tuple_, &left_rid)) {
      // 左侧全部耗尽后，不可能再产生 Join 结果。
      return false;
    }

    auto key = MakeJoinKey(left_tuple_, left_child_->GetOutputSchema(), plan_->LeftJoinKeyExpressions());

    // 含 NULL 的左 Key 不参与查找，因为 SQL 等值连接不会将 NULL 与任何值匹配。
    if (!key.HasNull()) {
      const auto match = hash_table_.find(key);
      if (match != hash_table_.end() && !match->second.empty()) {
        // 保存桶而不立刻在这里复制输出。回到 while 顶部后统一走第一步，
        // 并在之后的 Next() 调用中继续输出该桶剩余的右 Tuple。
        current_matches_ = &match->second;
        match_index_ = 0;
        continue;
      }
    }

    // 第三步：当前左 Tuple 没有匹配项。
    // LEFT JOIN 必须保留它，并把全部右侧列补成 NULL；INNER JOIN 则直接跳过它。
    if (plan_->GetJoinType() == JoinType::LEFT) {
      *tuple = MakeOutputTuple(left_tuple_, nullptr);
      return true;
    }

    // INNER JOIN 没有结果时不能 return false，因为左 child 后面仍可能存在可匹配的 Tuple。
    // 继续外层 while，在本次调用中寻找下一条能够产生输出的左 Tuple。
  }
}

}  // namespace bustub
