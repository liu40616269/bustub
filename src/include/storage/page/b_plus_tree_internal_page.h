//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/include/page/b_plus_tree_internal_page.h
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#pragma once

#include <queue>
#include <string>

#include "storage/page/b_plus_tree_page.h"

namespace bustub {

#define B_PLUS_TREE_INTERNAL_PAGE_TYPE BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>
#define INTERNAL_PAGE_HEADER_SIZE 12
#define INTERNAL_PAGE_SIZE ((BUSTUB_PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (sizeof(MappingType)))
/**
 * Store n indexed keys and n+1 child pointers (page_id) within internal page.
 * Pointer PAGE_ID(i) points to a subtree in which all keys K satisfy:
 * K(i) <= K < K(i+1).
 * NOTE: since the number of keys does not equal to number of child pointers,
 * the first key always remains invalid. That is to say, any search/lookup
 * should ignore the first key.
 *
 * Internal page format (keys are stored in increasing order):
 *  --------------------------------------------------------------------------
 * | HEADER | KEY(1)+PAGE_ID(1) | KEY(2)+PAGE_ID(2) | ... | KEY(n)+PAGE_ID(n) |
 *  --------------------------------------------------------------------------
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTreeInternalPage : public BPlusTreePage {
 public:
  // Deleted to disallow initialization
  BPlusTreeInternalPage() = delete;
  BPlusTreeInternalPage(const BPlusTreeInternalPage &other) = delete;

  /**
   * Writes the necessary header information to a newly created page, must be called after
   * the creation of a new page to make a valid BPlusTreeInternalPage
   * @param max_size Maximal size of the page
   */
  void Init(int max_size = INTERNAL_PAGE_SIZE);

  /**
   * @param index The index of the key to get. Index must be non-zero.
   * @return Key at index
   */
  auto KeyAt(int index) const -> KeyType;

  /**
   *
   * @param index The index of the key to set. Index must be non-zero.
   * @param key The new value for key
   */
  void SetKeyAt(int index, const KeyType &key);

  /**
   *
   * @param value the value to search for
   */
  auto ValueIndex(const ValueType &value) const -> int;

  /**
   *
   * @param index the index
   * @return the value at the index
   */
  auto ValueAt(int index) const -> ValueType;

  auto Lookup(const KeyType &key, const KeyComparator &comparator) const -> ValueType;

  void PopulateNewRoot(const ValueType &old_value, const KeyType &new_key, const ValueType &new_value);

  auto InsertNodeAfter(const ValueType &old_value, const KeyType &new_key, const ValueType &new_value) -> int;

  // 删除下标 index 对应的分隔 key 和 child；index 0 的最左 child 不能通过该接口删除。
  void Remove(int index);

  // 将当前 InternalPage 的后半部分移动到 recipient，并返回需要上推到父节点的分隔 key。
  auto MoveHalfTo(BPlusTreeInternalPage *recipient) -> KeyType;

  // 将新 child 与当前满页的已有项一起均分到当前页和 recipient，返回需要上推的分隔 key。
  auto InsertAndSplit(const ValueType &old_value, const KeyType &new_key, const ValueType &new_value,
                      BPlusTreeInternalPage *recipient) -> KeyType;

  // 从左兄弟借 child：将当前页最后一个 child 移到 recipient 最前面，并返回新的父分隔 key。
  auto MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key) -> KeyType;

  // 从右兄弟借 child：将当前页第一个 child 移到 recipient 末尾，并返回新的父分隔 key。
  auto MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key) -> KeyType;

  // 合并 InternalPage：通过父节点 middle_key 连接两页，并将当前右页全部追加到 recipient 左页。
  void MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key);

  /**
   * @brief For test only, return a string representing all keys in
   * this internal page, formatted as "(key1,key2,key3,...)"
   *
   * @return std::string
   */
  auto ToString() const -> std::string {
    std::string kstr = "(";
    bool first = true;

    // first key of internal page is always invalid
    for (int i = 1; i < GetSize(); i++) {
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
  // Flexible array member for page data.
  MappingType array_[0];
};
}  // namespace bustub
