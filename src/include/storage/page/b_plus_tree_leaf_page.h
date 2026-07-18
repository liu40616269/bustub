//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/include/page/b_plus_tree_leaf_page.h
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "storage/page/b_plus_tree_page.h"

namespace bustub {

#define B_PLUS_TREE_LEAF_PAGE_TYPE BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>
#define LEAF_PAGE_HEADER_SIZE 16
#define LEAF_PAGE_SIZE ((BUSTUB_PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / sizeof(MappingType))

/**
 * Store indexed key and record id(record id = page id combined with slot id,
 * see include/common/rid.h for detailed implementation) together within leaf
 * page. Only support unique key.
 *
 * Leaf page format (keys are stored in order):
 *  ----------------------------------------------------------------------
 * | HEADER | KEY(1) + RID(1) | KEY(2) + RID(2) | ... | KEY(n) + RID(n)
 *  ----------------------------------------------------------------------
 *
 *  Header format (size in byte, 16 bytes in total):
 *  ---------------------------------------------------------------------
 * | PageType (4) | CurrentSize (4) | MaxSize (4) |
 *  ---------------------------------------------------------------------
 *  -----------------------------------------------
 * |  NextPageId (4)
 *  -----------------------------------------------
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTreeLeafPage : public BPlusTreePage {
 public:
  // Delete all constructor / destructor to ensure memory safety
  BPlusTreeLeafPage() = delete;
  BPlusTreeLeafPage(const BPlusTreeLeafPage &other) = delete;

  /**
   * After creating a new leaf page from buffer pool, must call initialize
   * method to set default values
   * @param max_size Max size of the leaf node
   */
  void Init(int max_size = LEAF_PAGE_SIZE);

  // helper methods
  auto GetNextPageId() const -> page_id_t;
  void SetNextPageId(page_id_t next_page_id);
  auto KeyAt(int index) const -> KeyType;
  auto ValueAt(int index) const -> ValueType;

  // 返回页面中指定 key/value 对的只读引用。引用直接指向 Page 内存，调用方必须保证页面 Guard 仍然存活。
  auto GetItem(int index) const -> const MappingType &;

  // 返回第一个大于等于 key 的数组下标。
  auto KeyIndex(const KeyType &key, const KeyComparator &comparator) const -> int;

  // 查找 key，找到后通过 value 返回 RID。
  auto Lookup(const KeyType &key, ValueType *value, const KeyComparator &comparator) const -> bool;

  // 插入唯一的 key/value，重复 key 返回 false。
  auto Insert(const KeyType &key, const ValueType &value, const KeyComparator &comparator) -> bool;

  // 删除指定 key；找到并删除返回 true，不存在返回 false。
  auto Remove(const KeyType &key, const KeyComparator &comparator) -> bool;

  // 将当前叶子节点的后半部分数据移动到 recipient。
  void MoveHalfTo(BPlusTreeLeafPage *recipient);

  // 将当前叶子的最后一条记录移动到 recipient 的最前面（从左兄弟借记录）。
  void MoveLastToFrontOf(BPlusTreeLeafPage *recipient);

  // 将当前叶子的第一条记录移动到 recipient 的末尾（从右兄弟借记录）。
  void MoveFirstToEndOf(BPlusTreeLeafPage *recipient);

  // 将当前叶子的全部记录追加到 recipient，并让 recipient 跳过当前页连接后继叶子。
  void MoveAllTo(BPlusTreeLeafPage *recipient);

  /**
   * @brief for test only return a string representing all keys in
   * this leaf page formatted as "(key1,key2,key3,...)"
   *
   * @return std::string
   */
  auto ToString() const -> std::string {
    std::string kstr = "(";
    bool first = true;

    for (int i = 0; i < GetSize(); i++) {
      KeyType key = KeyAt(i);
      if (first) {
        first = false;
      } else {
        kstr.append(",");
      }

      kstr.append(std::to_string(key.ToString()));
    }
    kstr.append(")");

    return kstr;
  }

 private:
  page_id_t next_page_id_;
  // Flexible array member for page data.
  MappingType array_[0];
};
}  // namespace bustub
