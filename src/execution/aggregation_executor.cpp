//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// aggregation_executor.cpp
//
// Identification: src/execution/aggregation_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#include <memory>
#include <vector>

#include "execution/executors/aggregation_executor.h"

namespace bustub {

AggregationExecutor::AggregationExecutor(ExecutorContext *exec_ctx, const AggregationPlanNode *plan,
                                         std::unique_ptr<AbstractExecutor> &&child)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      child_(std::move(child)),
      aht_(plan_->GetAggregates(), plan_->GetAggregateTypes()),
      aht_iterator_(aht_.End()) {}

void AggregationExecutor::Init() {
  // Executor 可能被重复 Init（例如作为 NestedLoopJoin 的右 child）。必须先清除上一次执行留下的分组结果。
  aht_.Clear();

  // 将 child 重置到输入起点；随后在本函数中消费它产生的全部 Tuple。
  child_->Init();

  // 没有 GROUP BY 时，全部输入都属于 AggregateKey{} 这一全局分组。
  // 提前创建初始状态后，即使 child 为空，COUNT(*) 仍能输出 0，其他聚合则输出 NULL。
  // 有 GROUP BY 时不能提前创建分组：空输入应当产生零行结果。
  if (plan_->GetGroupBys().empty()) {
    aht_.InsertInitialAggregateValue(AggregateKey{});
  }

  Tuple input_tuple;
  RID input_rid;

  // Aggregation 是阻塞算子。Init() 必须先读取 child 的全部输入，构建完整哈希表后才能输出任何分组。
  while (child_->Next(&input_tuple, &input_rid)) {
    // GROUP BY 表达式的计算结果组成 AggregateKey，决定当前 Tuple 应合并到哪个分组。
    auto aggregate_key = MakeAggregateKey(&input_tuple);

    // COUNT/SUM/MIN/MAX 的输入表达式计算结果组成 AggregateValue，表示当前 Tuple 对各聚合项的贡献。
    auto aggregate_value = MakeAggregateValue(&input_tuple);

    // 分组首次出现时创建初始状态，之后按照每个 AggregationType 合并当前输入。
    aht_.InsertCombine(aggregate_key, aggregate_value);
  }

  // unordered_map 插入过程中可能发生 rehash，使旧迭代器失效。因此必须在哈希表构建完成后
  // 才把输出迭代器设置到第一个分组，供 Next() 逐组输出结果。
  aht_iterator_ = aht_.Begin();
}

auto AggregationExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  // Init() 已经消费 child 的全部输入并构建完聚合哈希表。
  // 当迭代器到达 End 时，说明所有分组都已输出。
  if (aht_iterator_ == aht_.End()) {
    return false;
  }

  // 哈希表中的 Key 保存当前分组的所有 GROUP BY 值，Val 保存该分组的最终聚合状态。
  const auto &aggregate_key = aht_iterator_.Key();
  const auto &aggregate_value = aht_iterator_.Val();

  std::vector<Value> values;
  values.reserve(aggregate_key.group_bys_.size() + aggregate_value.aggregates_.size());

  // AggregationPlan 的固定输出布局是：先放所有 GROUP BY 列。
  for (const auto &group_by : aggregate_key.group_bys_) {
    values.push_back(group_by);
  }

  // GROUP BY 列之后，再按 Plan 中的顺序放 COUNT/SUM/MIN/MAX 等最终结果。
  // SQL SELECT 列表中的重排和额外表达式由上层 ProjectionExecutor 处理。
  for (const auto &aggregate : aggregate_value.aggregates_) {
    values.push_back(aggregate);
  }

  *tuple = Tuple{values, &GetOutputSchema()};

  // 当前分组已经物化到输出 Tuple 中，可以安全推进到下一个哈希表条目。
  ++aht_iterator_;

  // 聚合结果是计算产生的逻辑 Tuple，不对应 TableHeap 中的物理记录，因此不需要填写输出 RID。
  return true;
}

auto AggregationExecutor::GetChildExecutor() const -> const AbstractExecutor * { return child_.get(); }

}  // namespace bustub
