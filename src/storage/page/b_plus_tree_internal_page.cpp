//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/page/b_plus_tree_internal_page.cpp
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <sstream>
#include <vector>

#include "common/exception.h"
#include "storage/page/b_plus_tree_internal_page.h"

namespace bustub {
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, and set max page size
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Init(int max_size) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetSize(0);
  SetMaxSize(max_size);
}
/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array offset)
 */
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::KeyAt(int index) const -> KeyType {
  // replace with your own code
  return array_[index].first;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::SetKeyAt(int index, const KeyType &key) { array_[index].first = key; }

/*
 * Helper method to get the value associated with input "index"(a.k.a array
 * offset)
 */
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueAt(int index) const -> ValueType { return array_[index].second; }

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueIndex(const ValueType &value) const -> int {
  for (int index = 0; index < GetSize(); ++index) {
    if (array_[index].second == value) {
      return index;
    }
  }
  return -1;
}
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::Lookup(const KeyType &key, const KeyComparator &comparator) const -> ValueType {
  BUSTUB_ASSERT(GetSize() > 0, "Cannot lookup an empty internal page");

  int left = 0;
  int right = GetSize();

  while (left + 1 < right) {
    int middle = left + (right - left) / 2;
    if (comparator(KeyAt(middle), key) <= 0) {
      left = middle;
    } else {
      right = middle;
    }
  }
  return ValueAt(left);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::PopulateNewRoot(const ValueType &old_value, const KeyType &new_key,
                                                     const ValueType &new_value) {
  // 下标 0 的 key 无效，但 child 有效。
  array_[0].second = old_value;

  // 下标 1 保存第一个有效分隔 key 和右 child。
  array_[1] = {new_key, new_value};

  // 两个 child 对应两个数组项。
  SetSize(2);
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::InsertNodeAfter(const ValueType &old_value, const KeyType &new_key,
                                                     const ValueType &new_value) -> int {
  // 找到发生分裂的旧child
  int old_index = ValueIndex(old_value);
  BUSTUB_ASSERT(old_index != -1, "Old child does not exist in internal page");

  int insert_index = old_index + 1;
  BUSTUB_ASSERT(GetSize() < GetMaxSize(), "Cannot insert into a full internal page");

  for (int current = GetSize(); current > insert_index; --current) {
    array_[current] = array_[current - 1];
  }
  array_[insert_index] = {new_key, new_value};
  IncreaseSize(1);
  return GetSize();
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Remove(int index) {
  // array_[0] 的 key 无效，但它保存最左侧 child；合并节点时应删除另一个 child 对应的有效分隔项。
  BUSTUB_ASSERT(index > 0 && index < GetSize(), "Internal page remove index is out of range");

  for (int current = index; current < GetSize() - 1; ++current) {
    array_[current] = array_[current + 1];
  }

  IncreaseSize(-1);
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveHalfTo(BPlusTreeInternalPage *recipient) -> KeyType {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient->GetSize() == 0, "Recipient internal page must initially be empty");

  const int split_index = GetMinSize();
  BUSTUB_ASSERT(split_index > 0 && split_index < GetSize(), "Internal page cannot be split at this index");

  const int move_count = GetSize() - split_index;
  const KeyType middle_key = KeyAt(split_index);

  // recipient 的 array_[0].first 在查找时会被忽略，但 array_[0].second 必须保留右节点最左侧 child。
  for (int index = 0; index < move_count; ++index) {
    recipient->array_[index] = array_[split_index + index];
  }

  recipient->SetSize(move_count);
  SetSize(split_index);
  return middle_key;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::InsertAndSplit(const ValueType &old_value, const KeyType &new_key,
                                                    const ValueType &new_value,
                                                    BPlusTreeInternalPage *recipient) -> KeyType {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient != this, "Source and recipient must be different internal pages");
  BUSTUB_ASSERT(GetSize() == GetMaxSize(), "InsertAndSplit should only split a full internal page");
  BUSTUB_ASSERT(recipient->GetSize() == 0, "Recipient internal page must initially be empty");

  const int old_index = ValueIndex(old_value);
  BUSTUB_ASSERT(old_index != -1, "The old child must exist before splitting its parent");
  const int insert_index = old_index + 1;

  // 先构造“原有 child + 待插入 child”的完整有序序列，再一次均分，避免先拆后插产生单 child 节点。
  std::vector<MappingType> entries;
  entries.reserve(static_cast<size_t>(GetSize() + 1));
  for (int index = 0; index <= GetSize(); ++index) {
    if (index == insert_index) {
      entries.emplace_back(new_key, new_value);
    }
    if (index < GetSize()) {
      entries.push_back(array_[index]);
    }
  }

  BUSTUB_ASSERT(static_cast<int>(entries.size()) == GetSize() + 1, "The split sequence should contain every child");
  const int total_size = static_cast<int>(entries.size());
  const int split_index = total_size / 2;
  BUSTUB_ASSERT(split_index >= 2, "A non-root internal page must retain at least two children after splitting");

  // recipient 的第一项 key 会在查找时忽略，但该 key 正是需要上推给父节点的分隔 key。
  const KeyType middle_key = entries[split_index].first;
  for (int index = 0; index < split_index; ++index) {
    array_[index] = entries[index];
  }
  for (int index = split_index; index < total_size; ++index) {
    recipient->array_[index - split_index] = entries[index];
  }

  SetSize(split_index);
  recipient->SetSize(total_size - split_index);
  return middle_key;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveLastToFrontOf(BPlusTreeInternalPage *recipient,
                                                       const KeyType &middle_key) -> KeyType {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient != this, "Donor and recipient must be different internal pages");
  BUSTUB_ASSERT(GetSize() > 1, "The left donor must keep at least one child");
  BUSTUB_ASSERT(recipient->GetSize() < recipient->GetMaxSize(), "Recipient internal page has no free slot");

  const int donor_last_index = GetSize() - 1;
  const KeyType new_parent_key = KeyAt(donor_last_index);
  const ValueType moved_child = ValueAt(donor_last_index);

  // recipient 原来的 array_[0] child 要变成下标 1，并使用父分隔 key 作为它的新有效 key。
  for (int index = recipient->GetSize(); index > 0; --index) {
    recipient->array_[index] = recipient->array_[index - 1];
  }
  recipient->array_[0].second = moved_child;
  recipient->array_[1].first = middle_key;

  IncreaseSize(-1);
  recipient->IncreaseSize(1);
  return new_parent_key;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveFirstToEndOf(BPlusTreeInternalPage *recipient,
                                                      const KeyType &middle_key) -> KeyType {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient != this, "Donor and recipient must be different internal pages");
  BUSTUB_ASSERT(GetSize() > 1, "The right donor must provide a new parent separator");
  BUSTUB_ASSERT(recipient->GetSize() < recipient->GetMaxSize(), "Recipient internal page has no free slot");

  // 右页 array_[0] 的 child 追加到左页时，父分隔 key 会下沉并成为它的有效 key。
  recipient->array_[recipient->GetSize()] = {middle_key, ValueAt(0)};
  recipient->IncreaseSize(1);

  // 右页原来的 key[1] 上升为新的父分隔 key，其 child 则成为右页新的最左 child。
  const KeyType new_parent_key = KeyAt(1);
  for (int index = 0; index < GetSize() - 1; ++index) {
    array_[index] = array_[index + 1];
  }
  IncreaseSize(-1);
  return new_parent_key;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  BUSTUB_ASSERT(recipient != nullptr, "Recipient cannot be null");
  BUSTUB_ASSERT(recipient != this, "Donor and recipient must be different internal pages");
  BUSTUB_ASSERT(recipient->GetSize() + GetSize() <= recipient->GetMaxSize(),
                "Recipient internal page cannot hold all donor children");

  const int recipient_size = recipient->GetSize();
  const int move_count = GetSize();

  // 当前页是右兄弟。其最左 child 原本没有有效 key，合并后使用父分隔 key 将左右两页连接起来。
  recipient->array_[recipient_size] = {middle_key, ValueAt(0)};
  for (int index = 1; index < move_count; ++index) {
    recipient->array_[recipient_size + index] = array_[index];
  }

  recipient->IncreaseSize(move_count);
  SetSize(0);
}

// valuetype for internalNode should be page id_t
template class BPlusTreeInternalPage<GenericKey<4>, page_id_t, GenericComparator<4>>;
template class BPlusTreeInternalPage<GenericKey<8>, page_id_t, GenericComparator<8>>;
template class BPlusTreeInternalPage<GenericKey<16>, page_id_t, GenericComparator<16>>;
template class BPlusTreeInternalPage<GenericKey<32>, page_id_t, GenericComparator<32>>;
template class BPlusTreeInternalPage<GenericKey<64>, page_id_t, GenericComparator<64>>;
}  // namespace bustub
