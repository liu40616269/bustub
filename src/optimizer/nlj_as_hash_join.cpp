#include <algorithm>
#include <memory>
#include <vector>
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "common/macros.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "optimizer/optimizer.h"
#include "type/type_id.h"

namespace bustub {

namespace {

/**
 * 从一个 Join 谓词中递归提取 Hash Join 的左右连接键。
 *
 * 支持的叶子节点是“一列 = 另一列”，支持的内部节点是 AND。
 * 只有整个谓词都能被表示成 Hash Join Key 时才返回 true，避免遗漏任何过滤条件。
 */
auto ExtractHashJoinKeys(const AbstractExpressionRef &expression, std::vector<AbstractExpressionRef> *left_keys,
                         std::vector<AbstractExpressionRef> *right_keys) -> bool {
  // 复合等值条件会形成 LogicExpression，例如：
  // (left.a = right.x) AND (left.b = right.y)。
  if (const auto *logic = dynamic_cast<const LogicExpression *>(expression.get()); logic != nullptr) {
    // OR 无法表示为“所有 Key 同时相等”，因此只能递归处理 AND。
    if (logic->logic_type_ != LogicType::And) {
      return false;
    }

    // 记录进入当前 AND 前的长度。如果任意一侧提取失败，就撤销本层已经追加的 Key，
    // 保证调用者永远不会拿到只覆盖部分谓词的结果。
    const auto original_left_size = left_keys->size();
    const auto original_right_size = right_keys->size();
    if (!ExtractHashJoinKeys(logic->GetChildAt(0), left_keys, right_keys) ||
        !ExtractHashJoinKeys(logic->GetChildAt(1), left_keys, right_keys)) {
      left_keys->resize(original_left_size);
      right_keys->resize(original_right_size);
      return false;
    }
    return true;
  }

  // 递归到叶子后，只接受单个等值比较；>、!= 等条件不能直接作为 Hash Join Key。
  const auto *comparison = dynamic_cast<const ComparisonExpression *>(expression.get());
  if (comparison == nullptr || comparison->comp_type_ != ComparisonType::Equal) {
    return false;
  }

  // 等号两侧必须都是简单列引用。常量和算术表达式暂不在本规则的转换范围内。
  const auto *first_column = dynamic_cast<const ColumnValueExpression *>(comparison->GetChildAt(0).get());
  const auto *second_column = dynamic_cast<const ColumnValueExpression *>(comparison->GetChildAt(1).get());
  if (first_column == nullptr || second_column == nullptr) {
    return false;
  }

  // 等号两侧的书写顺序不一定等于 Join child 的顺序，必须通过 tuple_idx 归一化。
  const ColumnValueExpression *left_column = nullptr;
  const ColumnValueExpression *right_column = nullptr;
  if (first_column->GetTupleIdx() == 0 && second_column->GetTupleIdx() == 1) {
    left_column = first_column;
    right_column = second_column;
  } else if (first_column->GetTupleIdx() == 1 && second_column->GetTupleIdx() == 0) {
    left_column = second_column;
    right_column = first_column;
  } else {
    // 两列来自同一个 child，无法组成一对左右 Hash Join Key。
    return false;
  }

  // HashJoin 会分别对左右 child 调用 Evaluate，因此两个 Key 表达式都处在只有
  // 一个输入 Tuple 的局部环境中，tuple_idx 均应重写为 0。
  left_keys->emplace_back(
      std::make_shared<ColumnValueExpression>(0, left_column->GetColIdx(), left_column->GetReturnType()));
  right_keys->emplace_back(
      std::make_shared<ColumnValueExpression>(0, right_column->GetColIdx(), right_column->GetReturnType()));
  return true;
}

}  // namespace

auto Optimizer::OptimizeNLJAsHashJoin(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  // 第一步：先递归优化所有 child。多表 Join 的子树中可能还包含 NestedLoopJoin，
  // 必须自底向上处理，才能让每一层符合条件的 Join 都有机会转换。
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeNLJAsHashJoin(child));
  }

  // 使用已经优化过的 children 克隆当前 Plan。后续即使当前节点不能转换，
  // 也要返回这个副本，从而保留对子计划完成的优化。
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  // HashJoin 只能替换 NestedLoopJoin；其他类型的 Plan 保持原样。
  if (optimized_plan->GetType() != PlanType::NestedLoopJoin) {
    return optimized_plan;
  }

  const auto &nlj_plan = dynamic_cast<const NestedLoopJoinPlanNode &>(*optimized_plan);
  BUSTUB_ENSURE(nlj_plan.GetChildren().size() == 2, "NestedLoopJoin must have exactly two children");

  // 单个等值条件会提取一对 Key；AND 连接的多个等值条件会按表达式顺序提取多对 Key。
  // 任意一个子条件不受支持时，必须保留完整 NLJ，不能丢掉剩余谓词后只做部分 Hash Join。
  std::vector<AbstractExpressionRef> left_keys;
  std::vector<AbstractExpressionRef> right_keys;
  if (!ExtractHashJoinKeys(nlj_plan.Predicate(), &left_keys, &right_keys)) {
    return optimized_plan;
  }

  BUSTUB_ENSURE(!left_keys.empty() && left_keys.size() == right_keys.size(),
                "HashJoin must have the same non-zero number of left and right keys");

  // 输出 Schema、已经优化过的两个 child 以及 Join 类型都沿用原 NestedLoopJoin，
  // 只把执行算法和谓词表示改为 HashJoin + 左右 Key。
  return std::make_shared<HashJoinPlanNode>(nlj_plan.output_schema_, nlj_plan.GetLeftPlan(), nlj_plan.GetRightPlan(),
                                            std::move(left_keys), std::move(right_keys), nlj_plan.GetJoinType());
}

}  // namespace bustub
