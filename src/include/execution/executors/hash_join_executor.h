//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// hash_join_executor.h
//
// Identification: src/include/execution/executors/hash_join_executor.h
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/util/hash_util.h"
#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/hash_join_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/** Hash Join 使用的连接键；values_ 可以同时保存多个等值连接列。 */
struct HashJoinKey {
  std::vector<Value> values_;

  /** unordered_map 使用该函数确认哈希值相同的两个 Key 是否真的相等。 */
  auto operator==(const HashJoinKey &other) const -> bool {
    if (values_.size() != other.values_.size()) {
      return false;
    }

    for (uint32_t i = 0; i < values_.size(); i++) {
      const auto &left_value = values_[i];
      const auto &right_value = other.values_[i];

      // 让相等判断满足 unordered_map 要求的自反性；真正执行 Join 时，
      // 含 NULL 的 Key 会在插入或查找哈希表之前被 HasNull() 排除。
      if (left_value.IsNull() || right_value.IsNull()) {
        if (left_value.IsNull() != right_value.IsNull()) {
          return false;
        }
        continue;
      }

      if (left_value.CompareEquals(right_value) != CmpBool::CmpTrue) {
        return false;
      }
    }

    return true;
  }

  /** SQL 等值连接中，只要一个连接列为 NULL，整条复合 Key 就不能匹配。 */
  auto HasNull() const -> bool {
    for (const auto &value : values_) {
      if (value.IsNull()) {
        return true;
      }
    }
    return false;
  }
};

/** 依次合并复合连接键中每一个 Value 的哈希值。 */
struct HashJoinKeyHasher {
  auto operator()(const HashJoinKey &key) const -> std::size_t {
    std::size_t current_hash = 0;

    for (const auto &value : key.values_) {
      const auto value_hash = value.IsNull() ? 0 : HashUtil::HashValue(&value);
      current_hash = HashUtil::CombineHashes(current_hash, value_hash);
    }

    return current_hash;
  }
};

/**
 * HashJoinExecutor executes a hash JOIN on two tables.
 */
class HashJoinExecutor : public AbstractExecutor {
 public:
  /**
   * Construct a new HashJoinExecutor instance.
   * @param exec_ctx The executor context
   * @param plan The HashJoin join plan to be executed
   * @param left_child The child executor that produces tuples for the left side of join
   * @param right_child The child executor that produces tuples for the right side of join
   */
  HashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                   std::unique_ptr<AbstractExecutor> &&left_child, std::unique_ptr<AbstractExecutor> &&right_child);

  /** Initialize the join */
  void Init() override;

  /**
   * Yield the next tuple from the join.
   * @param[out] tuple The next tuple produced by the join.
   * @param[out] rid The next tuple RID, not used by hash join.
   * @return `true` if a tuple was produced, `false` if there are no more tuples.
   */
  auto Next(Tuple *tuple, RID *rid) -> bool override;

  /** @return The output schema for the join */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); };

 private:
  /** 根据指定表达式，从一条 Tuple 中计算单列或多列连接键。 */
  auto MakeJoinKey(const Tuple &tuple, const Schema &schema,
                   const std::vector<AbstractExpressionRef> &expressions) const -> HashJoinKey;

  /** 按“左侧所有列 + 右侧所有列”的布局构造结果；right_tuple 为空时为右侧补 NULL。 */
  auto MakeOutputTuple(const Tuple &left_tuple, const Tuple *right_tuple) const -> Tuple;

  /** The HashJoin plan node to be executed. */
  const HashJoinPlanNode *plan_;

  /** 产生 Join 左侧 Tuple 的 child；Next() 会逐条读取它并探测哈希表。 */
  std::unique_ptr<AbstractExecutor> left_child_;

  /** 产生 Join 右侧 Tuple 的 child；Init() 会完整读取它来构建哈希表。 */
  std::unique_ptr<AbstractExecutor> right_child_;

  /** 一个连接键可能对应多条右 Tuple，因此哈希表的 Value 必须是 vector。 */
  std::unordered_map<HashJoinKey, std::vector<Tuple>, HashJoinKeyHasher> hash_table_;

  /** 当前正在输出匹配结果的左 Tuple。 */
  Tuple left_tuple_;

  /** 当前左 Tuple 在哈希表中找到的右 Tuple 桶；哈希表构建完后不再修改，因此指针稳定。 */
  const std::vector<Tuple> *current_matches_{nullptr};

  /** 下一次应输出 current_matches_ 中的哪个右 Tuple。 */
  std::size_t match_index_{0};
};

}  // namespace bustub
