//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/page/b_plus_tree_leaf_page.cpp
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <sstream>

#include "common/exception.h"
#include "common/rid.h"
#include "storage/page/b_plus_tree_leaf_page.h"

namespace bustub {

/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/

/**
 * Init method after creating a new leaf page
 * Including set page type, set current size to zero, set next page id and set max size
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::Init(int max_size) {
  SetPageType(IndexPageType::LEAF_PAGE);
  SetSize(0);
  SetMaxSize(max_size);
  SetNextPageId(INVALID_PAGE_ID);
}

/**
 * Helper methods to set/get next page id
 */
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::GetNextPageId() const -> page_id_t { return next_page_id_; }

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

/*
 * Helper method to find and return the key associated with input "index"(a.k.a
 * array offset)
 */
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::KeyAt(int index) const -> KeyType {
  // replace with your own code
  return array_[index].first;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::ValueAt(int index) const -> ValueType { return array_[index].second; }

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::GetItem(int index) const -> const MappingType & {
  BUSTUB_ASSERT(index >= 0 && index < GetSize(), "Leaf item index is out of range");

  // 不创建 key/RID 副本，直接暴露 Page 内 array_ 的 const 引用；IndexIterator 的 Guard 负责其生命周期。
  return array_[index];
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::KeyIndex(const KeyType &key, const KeyComparator &comparator) const -> int {
  // 实现
  int left = -1;
  int right = GetSize();

  while (left + 1 < right) {
    int middle = left + (right - left) / 2;
    if (comparator(KeyAt(middle), key) < 0) {
      left = middle;
    } else {
      right = middle;
    }
  }
  return right;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::Lookup(const KeyType &key, ValueType *value,
                                        const KeyComparator &comparator) const -> bool {
  // 实现
  int index = KeyIndex(key, comparator);
  // index == size()表示现有key都小于目标key
  if (index == GetSize()) {
    return false;
  }
  if (comparator(KeyAt(index), key) != 0) {
    return false;
  }
  *value = ValueAt(index);
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::Insert(const KeyType &key, const ValueType &value,
                                        const KeyComparator &comparator) -> bool {
  // 实现
  const int index = KeyIndex(key, comparator);
  // KeyIndex返回第一个>=key的位置，因此先检查是不是重复了
  if (index < GetSize() && comparator(KeyAt(index), key) == 0) {
    return false;
  }
  // 调用insert前，页面必须至少还有一个位置
  BUSTUB_ASSERT(GetSize() < GetMaxSize(), "Cannot insert into a full leaf page");

  // 从后往前移动
  for (int current = GetSize(); current > index; --current) {
    array_[current] = array_[current - 1];
  }
  array_[index] = {key, value};
  IncreaseSize(1);
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::Remove(const KeyType &key, const KeyComparator &comparator) -> bool {
  const int index = KeyIndex(key, comparator);

  // KeyIndex 返回第一个 >= key 的位置；越过末尾或 key 不相等都表示记录不存在。
  if (index == GetSize() || comparator(KeyAt(index), key) != 0) {
    return false;
  }

  // 覆盖被删除项，并将它后面的所有记录依次向左移动一格。
  for (int current = index; current < GetSize() - 1; ++current) {
    array_[current] = array_[current + 1];
  }

  IncreaseSize(-1);
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveHalfTo(BPlusTreeLeafPage *recipient) {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient->GetSize() == 0, "Recipient leaf must initially be empty");
  const int split_index = GetMinSize();
  const int move_count = GetSize() - split_index;

  for (int i = 0; i < move_count; ++i) {
    recipient->array_[i] = array_[split_index + i];
  }

  recipient->SetSize(move_count);
  SetSize(split_index);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveLastToFrontOf(BPlusTreeLeafPage *recipient) {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient != this, "Donor and recipient must be different leaf pages");
  BUSTUB_ASSERT(GetSize() > 0, "Cannot move a record from an empty leaf page");
  BUSTUB_ASSERT(recipient->GetSize() < recipient->GetMaxSize(), "Recipient leaf page has no free slot");

  // 先保存左兄弟的最后一项；它在有序关系上小于 recipient 中的所有记录。
  const MappingType moved_item = array_[GetSize() - 1];

  // recipient 需要在下标 0 腾出位置，因此必须从后向前移动，避免覆盖尚未搬走的数据。
  for (int index = recipient->GetSize(); index > 0; --index) {
    recipient->array_[index] = recipient->array_[index - 1];
  }
  recipient->array_[0] = moved_item;

  IncreaseSize(-1);
  recipient->IncreaseSize(1);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveFirstToEndOf(BPlusTreeLeafPage *recipient) {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient != this, "Donor and recipient must be different leaf pages");
  BUSTUB_ASSERT(GetSize() > 0, "Cannot move a record from an empty leaf page");
  BUSTUB_ASSERT(recipient->GetSize() < recipient->GetMaxSize(), "Recipient leaf page has no free slot");

  // 右兄弟的第一项大于 recipient 的所有记录，可以直接追加到 recipient 末尾。
  recipient->array_[recipient->GetSize()] = array_[0];
  recipient->IncreaseSize(1);

  // donor 删除第一项后，其余记录整体向左移动一格。
  for (int index = 0; index < GetSize() - 1; ++index) {
    array_[index] = array_[index + 1];
  }
  IncreaseSize(-1);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveAllTo(BPlusTreeLeafPage *recipient) {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient != this, "Donor and recipient must be different leaf pages");
  BUSTUB_ASSERT(recipient->GetSize() + GetSize() <= recipient->GetMaxSize(),
                "Recipient leaf page cannot hold all donor records");

  // 当前页是右兄弟，其全部记录都大于 recipient 的记录，因此按原顺序追加即可。
  const int recipient_size = recipient->GetSize();
  const int move_count = GetSize();
  for (int index = 0; index < move_count; ++index) {
    recipient->array_[recipient_size + index] = array_[index];
  }

  recipient->IncreaseSize(move_count);
  SetSize(0);

  // 当前右页稍后会被删除，让左页直接连接当前页原来的后继叶子。
  recipient->SetNextPageId(GetNextPageId());
}

template class BPlusTreeLeafPage<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTreeLeafPage<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTreeLeafPage<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTreeLeafPage<GenericKey<64>, RID, GenericComparator<64>>;
}  // namespace bustub
