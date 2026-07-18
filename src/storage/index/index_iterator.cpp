/**
 * index_iterator.cpp
 */
#include <utility>

#include "common/macros.h"
#include "storage/index/index_iterator.h"

namespace bustub {

// 默认状态下 optional guard 为空，因此默认构造的 Iterator 自然就是 End。
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::IndexIterator() = default;

INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::IndexIterator(BufferPoolManager *bpm, ReadPageGuard guard, int index)
    : bpm_(bpm), page_id_(guard.PageId()), guard_(std::move(guard)), index_(index) {
  // 有效 Iterator 后续可能沿 next_page_id Fetch 新页面，所以必须保存 BPM。
  BUSTUB_ASSERT(bpm_ != nullptr, "An active index iterator requires a buffer pool manager");
  BUSTUB_ASSERT(index_ >= 0, "Iterator index cannot be negative");
}

INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::IndexIterator(IndexIterator &&that) noexcept
    : bpm_(that.bpm_), page_id_(that.page_id_), guard_(std::move(that.guard_)), index_(that.index_) {
  // std::optional 移动后，源 optional 仍可能处于 has_value()==true，只是其中的 Guard 已被移空。
  // 因此必须显式 reset 并清理其余状态，让 moved-from Iterator 成为一致且可安全析构的 End。
  that.guard_.reset();
  that.bpm_ = nullptr;
  that.page_id_ = INVALID_PAGE_ID;
  that.index_ = 0;
}

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::operator=(IndexIterator &&that) noexcept -> INDEXITERATOR_TYPE & {
  if (this == &that) {
    return *this;
  }

  // 移动赋值和移动构造不同：this 可能已经指向另一个叶子。
  // 所以必须先 reset，释放旧叶子的读锁和 pin，再接管 that 的 Guard，不能直接覆盖造成资源泄漏。
  guard_.reset();
  bpm_ = that.bpm_;
  page_id_ = that.page_id_;
  guard_ = std::move(that.guard_);
  index_ = that.index_;

  that.guard_.reset();
  that.bpm_ = nullptr;
  that.page_id_ = INVALID_PAGE_ID;
  that.index_ = 0;
  return *this;
}

INDEX_TEMPLATE_ARGUMENTS
// optional<ReadPageGuard> 析构时会自动析构 Guard，从而解读锁并 Unpin，无需手写释放代码。
INDEXITERATOR_TYPE::~IndexIterator() = default;  // NOLINT

INDEX_TEMPLATE_ARGUMENTS
// End 不是一个特殊页面或特殊数组下标，而是“完全不持有页面”。
auto INDEXITERATOR_TYPE::IsEnd() -> bool { return !guard_.has_value(); }

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::operator*() -> const MappingType & {
  // End 不指向记录，解引用属于调用错误。
  BUSTUB_ASSERT(!IsEnd(), "Cannot dereference the end iterator");

  // Guard 管理的 Page 本质上是一块原始字节；这里按 LeafPage 的布局只读解释它。
  const auto *leaf_page = guard_->template As<B_PLUS_TREE_LEAF_PAGE_TYPE>();
  BUSTUB_ASSERT(index_ >= 0 && index_ < leaf_page->GetSize(), "Iterator index is outside the current leaf");

  // 返回页面内部 array_[index_] 的引用而不是副本。
  // 该引用只在当前 Iterator 尚未 ++ 跨页、移动或析构之前有效，因为这些操作可能释放当前 Guard。
  return leaf_page->GetItem(index_);
}

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::operator++() -> INDEXITERATOR_TYPE & {
  BUSTUB_ASSERT(!IsEnd(), "Cannot increment the end iterator");
  const auto *leaf_page = guard_->template As<B_PLUS_TREE_LEAF_PAGE_TYPE>();

  // 第一步永远先尝试移动页内游标。例如 size=3 时，合法下标是 0、1、2。
  ++index_;

  // index_ 仍小于 size，说明下一条记录还在同一叶子；Guard 和 page_id 都不需要变化。
  if (index_ < leaf_page->GetSize()) {
    return *this;
  }

  // index_ == size 表示当前叶子扫描完毕。叶子天生按 key 有序且组成单向链表，所以不必返回父节点，
  // 直接读取 next_page_id 就能到达全局顺序中的下一批记录。
  page_id_t next_page_id = leaf_page->GetNextPageId();
  while (next_page_id != INVALID_PAGE_ID) {
    // 先取得下一页的 ReadPageGuard，再移动赋值给 guard_。移动赋值会释放旧叶子的锁和 pin，
    // 最终 Iterator 在任意时刻只负责一个叶子页面。
    auto next_guard = bpm_->FetchPageRead(next_page_id);
    page_id_ = next_page_id;
    guard_ = std::move(next_guard);
    index_ = 0;

    leaf_page = guard_->template As<B_PLUS_TREE_LEAF_PAGE_TYPE>();
    if (leaf_page->GetSize() > 0) {
      return *this;
    }

    // 正常删除逻辑不会在链中保留空的非根叶子；继续跳过空页是防御性处理。
    next_page_id = leaf_page->GetNextPageId();
  }

  // next_page_id 无效，说明当前记录原本位于最右叶子的末尾。清空 Guard 后，Iterator 与 End() 相等。
  guard_.reset();
  page_id_ = INVALID_PAGE_ID;
  index_ = 0;
  return *this;
}

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::operator==(const IndexIterator &itr) const -> bool {
  const bool this_is_end = !guard_.has_value();
  const bool other_is_end = !itr.guard_.has_value();

  // 所有 End 都表示“越过最后一条记录”，不需要比较 page_id 或 index。
  if (this_is_end || other_is_end) {
    return this_is_end && other_is_end;
  }

  // 有效逻辑位置由 (leaf page id, slot index) 唯一确定；Guard 对象本身的地址不参与比较。
  return page_id_ == itr.page_id_ && index_ == itr.index_;
}

INDEX_TEMPLATE_ARGUMENTS
auto INDEXITERATOR_TYPE::operator!=(const IndexIterator &itr) const -> bool { return !(*this == itr); }

// Iterator 的模板实现位于 .cpp 中，因此需要为项目支持的各类 GenericKey 显式实例化。
template class IndexIterator<GenericKey<4>, RID, GenericComparator<4>>;

template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>>;

template class IndexIterator<GenericKey<16>, RID, GenericComparator<16>>;

template class IndexIterator<GenericKey<32>, RID, GenericComparator<32>>;

template class IndexIterator<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
