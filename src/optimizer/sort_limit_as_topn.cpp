#include <memory>
#include <vector>

#include "common/macros.h"
#include "execution/plans/limit_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/topn_plan.h"
#include "optimizer/optimizer.h"

namespace bustub {

auto Optimizer::OptimizeSortLimitAsTopN(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  // 第一步：自底向上递归优化所有 child。这样子查询中的 Sort + Limit
  // 也可以先于外层 Plan 被转换成 TopN。
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeSortLimitAsTopN(child));
  }

  // 使用已经优化过的 children 克隆当前节点。即使当前节点不能转换，
  // 也必须返回该副本，以保留对子计划完成的优化。
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  // 目标模式的根节点必须是 Limit；其他节点无需改写。
  if (optimized_plan->GetType() != PlanType::Limit) {
    return optimized_plan;
  }

  const auto &limit_plan = dynamic_cast<const LimitPlanNode &>(*optimized_plan);
  BUSTUB_ENSURE(limit_plan.GetChildren().size() == 1, "Limit must have exactly one child");

  // 只有 Limit 的直接 child 是 Sort 时，二者才能合并成一个 TopN。
  // 普通 Limit(child) 仍应继续使用 LimitExecutor。
  const auto &limit_child = limit_plan.GetChildPlan();
  if (limit_child->GetType() != PlanType::Sort) {
    return optimized_plan;
  }

  const auto &sort_plan = dynamic_cast<const SortPlanNode &>(*limit_child);
  BUSTUB_ENSURE(sort_plan.GetChildren().size() == 1, "Sort must have exactly one child");

  // TopN 同时接管 Sort 的 ORDER BY 规则和 Limit 的 N。
  // 它必须直接连接 Sort 原来的 child；如果继续保留 Sort，就仍会排序全部输入，失去优化意义。
  return std::make_shared<TopNPlanNode>(limit_plan.output_schema_, sort_plan.GetChildPlan(), sort_plan.GetOrderBy(),
                                        limit_plan.GetLimit());
}

}  // namespace bustub
