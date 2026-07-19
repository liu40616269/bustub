//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_scan_executor.cpp
//
// Identification: src/execution/index_scan_executor.cpp
//
// Copyright (c) 2015-19, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <utility>

#include "execution/executors/index_scan_executor.h"

namespace bustub {

IndexScanExecutor::IndexScanExecutor(ExecutorContext *exec_ctx, const IndexScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan) {
  // IndexScanPlan 只记录索引 OID，先通过 Catalog 找到索引元数据和实际 Index 对象。
  index_info_ = exec_ctx_->GetCatalog()->GetIndex(plan_->GetIndexOid());
  BUSTUB_ASSERT(index_info_ != nullptr, "Index does not exist");

  // B+ 树叶子只保存“索引 Key -> RID”，读取完整 Tuple 时还需要根据索引所属表找到 TableHeap。
  table_info_ = exec_ctx_->GetCatalog()->GetTable(index_info_->table_name_);
  BUSTUB_ASSERT(table_info_ != nullptr, "Table does not exist");

  // Catalog 用抽象 Index 指针保存索引，而顺序迭代接口是 BPlusTreeIndex 特有的，
  // 因此需要转换成本项目创建索引时实际使用的具体 B+ 树类型。
  tree_ = dynamic_cast<BPlusTreeIndexForTwoIntegerColumn *>(index_info_->index_.get());
  BUSTUB_ASSERT(tree_ != nullptr, "Index is not a supported B+ tree index");
}

void IndexScanExecutor::Init() {
  // Begin() 指向最小索引 Key；随后沿 B+ 树叶子链表即可按 Key 升序扫描全部索引项。
  // 如果索引为空，GetBeginIterator() 会直接返回 End Iterator，Next() 随即返回 false。
  iterator_ = tree_->GetBeginIterator();
}

auto IndexScanExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  while (!iterator_.IsEnd()) {
    // B+ 树叶子中的每个 MappingType 是 (key, RID)。SQL 需要的是完整表记录，
    // 因此这里只复制 RID，随后使用它回到 TableHeap 读取 Tuple。
    // 必须复制而不能保存引用：++iterator_ 可能切换叶子并释放旧 PageGuard，使页面内引用失效。
    const auto current_rid = (*iterator_).second;

    // 提前推进扫描位置，保证下一次 Next() 不会重复返回当前索引项。
    ++iterator_;

    // 二级索引不保存完整记录，只保存 Key 和 RID；根据 RID 回表取得 TupleMeta 和完整 Tuple。
    auto [meta, current_tuple] = table_info_->table_->GetTuple(current_rid);

    // DeleteExecutor 正常会同步删除索引项。这里仍检查逻辑删除标志，防御性地跳过陈旧索引项；
    // 例如在已经存在逻辑删除记录的表上新建索引时，Catalog 的建索引过程可能仍遍历到该 Slot。
    if (meta.is_deleted_) {
      continue;
    }

    *tuple = std::move(current_tuple);
    *rid = current_rid;
    return true;
  }

  // End Iterator 表示所有叶子中的索引项都已经扫描完毕。
  return false;
}

}  // namespace bustub
