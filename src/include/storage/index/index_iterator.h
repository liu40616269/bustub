//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/include/index/index_iterator.h
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
/**
 * index_iterator.h
 * For range scan of b+ tree
 */
#pragma once

#include <optional>

#include "buffer/buffer_pool_manager.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/page_guard.h"

namespace bustub {

#define INDEXITERATOR_TYPE IndexIterator<KeyType, ValueType, KeyComparator>

/**
 * B+ 树的顺序扫描迭代器。
 *
 * InternalPage 只用于寻找扫描起点；真正的 (key, RID) 全部位于 LeafPage 中，且叶子通过 next_page_id
 * 从左到右连接。因此一个“有效迭代位置”可以用下面三项表示：
 *
 *   (当前 LeafPage 的 ReadPageGuard, 当前页内下标 index, 用于读取下一页的 BufferPoolManager)
 *
 * guard_ 不仅表示“当前在哪一页”，还负责让页面保持 pinned 并持有读锁。这样 operator* 返回页面内记录的引用时，
 * 对应内存仍然有效。End 不对应任何真实记录，统一用 guard_ == std::nullopt 表示。
 *
 * ReadPageGuard 是独占所有权的 RAII 对象，不能复制，所以 IndexIterator 同样只允许移动，不能复制。
 */
INDEX_TEMPLATE_ARGUMENTS
class IndexIterator {
 public:
  /** 默认构造时不持有任何页面，得到 End Iterator。 */
  IndexIterator();

  /**
   * 构造有效 Iterator：接管 guard 对应叶子的读锁与 pin，并指向该页的 index 位置。
   * 参数按值接收 guard，调用方必须通过 std::move 转移所有权。
   */
  IndexIterator(BufferPoolManager *bpm, ReadPageGuard guard, int index);

  // 禁止复制，避免两个 Iterator 同时认为自己拥有同一个 ReadPageGuard，最终重复解锁和 Unpin。
  IndexIterator(const IndexIterator &) = delete;
  auto operator=(const IndexIterator &) -> IndexIterator & = delete;

  // 允许移动：资源交给目标 Iterator，源 Iterator 会被清成 End。
  IndexIterator(IndexIterator &&that) noexcept;
  auto operator=(IndexIterator &&that) noexcept -> IndexIterator &;

  ~IndexIterator();  // NOLINT

  /** guard_ 为空即表示扫描结束。 */
  auto IsEnd() -> bool;

  /** 返回当前叶子 array_[index_] 中 (key, RID) 的只读引用。 */
  auto operator*() -> const MappingType &;

  /** 前置递增：优先移动页内下标；越过页尾时沿 next_page_id 切换叶子。 */
  auto operator++() -> IndexIterator &;

  /** 两个 End 相等；两个有效 Iterator 只有 page_id 和页内 index 都相同才相等。 */
  auto operator==(const IndexIterator &itr) const -> bool;

  auto operator!=(const IndexIterator &itr) const -> bool;

 private:
  // Iterator 自己不拥有 BPM；仅借用 BPlusTree 的 BPM 来 Fetch 下一个叶子页面。
  BufferPoolManager *bpm_{nullptr};

  // 缓存当前叶子的 page id，便于比较两个 const Iterator；End 使用 INVALID_PAGE_ID。
  page_id_t page_id_{INVALID_PAGE_ID};

  // 有值时负责当前叶子的读锁与 pin；无值时 Iterator 就是 End。
  std::optional<ReadPageGuard> guard_{std::nullopt};

  // 当前记录在 LeafPage::array_ 中的下标，只在 guard_ 有值时有意义。
  int index_{0};
};

}  // namespace bustub
