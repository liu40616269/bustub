//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree_page_test.cpp
//
// Identification: test/storage/b_plus_tree_page_test.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <atomic>
#include <memory>
#include <numeric>
#include <random>
#include <thread>  // NOLINT
#include <utility>

#include "buffer/buffer_pool_manager.h"
#include "common/rid.h"
#include "storage/disk/disk_manager_memory.h"
#include "storage/index/b_plus_tree.h"
#include "storage/index/generic_key.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/page.h"
#include "test_util.h"  // NOLINT

#include "gtest/gtest.h"

namespace bustub {

using Key = GenericKey<8>;
using LeafPage = BPlusTreeLeafPage<Key, RID, GenericComparator<8>>;
using InternalPage = BPlusTreeInternalPage<Key, page_id_t, GenericComparator<8>>;
using Tree = BPlusTree<Key, RID, GenericComparator<8>>;
using LeafMapping = std::pair<Key, RID>;
using InternalMapping = std::pair<Key, page_id_t>;

TEST(BPlusTreePageTest, BasePageMetadataTest) {
  Page raw_page;
  auto *page = reinterpret_cast<BPlusTreePage *>(raw_page.GetData());

  page->SetPageType(IndexPageType::LEAF_PAGE);
  EXPECT_TRUE(page->IsLeafPage());
  page->SetPageType(IndexPageType::INTERNAL_PAGE);
  EXPECT_FALSE(page->IsLeafPage());

  page->SetSize(3);
  EXPECT_EQ(page->GetSize(), 3);
  page->IncreaseSize(2);
  EXPECT_EQ(page->GetSize(), 5);
  page->IncreaseSize(-4);
  EXPECT_EQ(page->GetSize(), 1);

  page->SetMaxSize(5);
  EXPECT_EQ(page->GetMaxSize(), 5);
  EXPECT_EQ(page->GetMinSize(), 2);
}

TEST(BPlusTreePageTest, LeafPageInitAndAccessTest) {
  Page raw_page;
  auto *leaf = reinterpret_cast<LeafPage *>(raw_page.GetData());

  leaf->Init(5);
  EXPECT_TRUE(leaf->IsLeafPage());
  EXPECT_EQ(leaf->GetSize(), 0);
  EXPECT_EQ(leaf->GetMaxSize(), 5);
  EXPECT_EQ(leaf->GetMinSize(), 2);
  EXPECT_EQ(leaf->GetNextPageId(), INVALID_PAGE_ID);

  leaf->SetNextPageId(42);
  EXPECT_EQ(leaf->GetNextPageId(), 42);

  Key key10;
  Key key20;
  key10.SetFromInteger(10);
  key20.SetFromInteger(20);

  auto *array = reinterpret_cast<LeafMapping *>(raw_page.GetData() + LEAF_PAGE_HEADER_SIZE);
  array[0] = {key10, RID(1, 10)};
  array[1] = {key20, RID(2, 20)};
  leaf->SetSize(2);

  EXPECT_EQ(leaf->KeyAt(0).ToString(), 10);
  EXPECT_EQ(leaf->KeyAt(1).ToString(), 20);
}

TEST(BPlusTreePageTest, LeafPageSearchAndInsertTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page raw_page;
  auto *leaf = reinterpret_cast<LeafPage *>(raw_page.GetData());
  leaf->Init(5);

  Key key10;
  Key key20;
  Key key30;
  Key key25;
  Key key40;
  key10.SetFromInteger(10);
  key20.SetFromInteger(20);
  key30.SetFromInteger(30);
  key25.SetFromInteger(25);
  key40.SetFromInteger(40);

  EXPECT_TRUE(leaf->Insert(key30, RID(3, 30), comparator));
  EXPECT_TRUE(leaf->Insert(key10, RID(1, 10), comparator));
  EXPECT_TRUE(leaf->Insert(key20, RID(2, 20), comparator));
  EXPECT_FALSE(leaf->Insert(key20, RID(9, 99), comparator));

  ASSERT_EQ(leaf->GetSize(), 3);
  EXPECT_EQ(leaf->KeyAt(0).ToString(), 10);
  EXPECT_EQ(leaf->KeyAt(1).ToString(), 20);
  EXPECT_EQ(leaf->KeyAt(2).ToString(), 30);
  EXPECT_EQ(leaf->KeyIndex(key10, comparator), 0);
  EXPECT_EQ(leaf->KeyIndex(key20, comparator), 1);
  EXPECT_EQ(leaf->KeyIndex(key25, comparator), 2);
  EXPECT_EQ(leaf->KeyIndex(key40, comparator), 3);

  RID value;
  EXPECT_TRUE(leaf->Lookup(key20, &value, comparator));
  EXPECT_EQ(value, RID(2, 20));
  EXPECT_FALSE(leaf->Lookup(key25, &value, comparator));
  EXPECT_FALSE(leaf->Lookup(key40, &value, comparator));
}

TEST(BPlusTreePageTest, LeafPageRemoveTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page raw_page;
  auto *leaf = reinterpret_cast<LeafPage *>(raw_page.GetData());
  leaf->Init(6);

  for (int64_t value : {10, 20, 30, 40}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(leaf->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }

  Key key10;
  Key key20;
  Key key25;
  Key key30;
  Key key40;
  key10.SetFromInteger(10);
  key20.SetFromInteger(20);
  key25.SetFromInteger(25);
  key30.SetFromInteger(30);
  key40.SetFromInteger(40);

  // 删除中间项后，右侧记录应连续左移。
  ASSERT_TRUE(leaf->Remove(key20, comparator));
  ASSERT_EQ(leaf->GetSize(), 3);
  EXPECT_EQ(leaf->KeyAt(0).ToString(), 10);
  EXPECT_EQ(leaf->KeyAt(1).ToString(), 30);
  EXPECT_EQ(leaf->KeyAt(2).ToString(), 40);

  // 删除不存在的 key 不应改变 size 或已有记录。
  EXPECT_FALSE(leaf->Remove(key25, comparator));
  ASSERT_EQ(leaf->GetSize(), 3);
  EXPECT_EQ(leaf->KeyAt(1).ToString(), 30);

  // 删除首项和尾项。
  EXPECT_TRUE(leaf->Remove(key10, comparator));
  EXPECT_TRUE(leaf->Remove(key40, comparator));
  ASSERT_EQ(leaf->GetSize(), 1);
  EXPECT_EQ(leaf->KeyAt(0).ToString(), 30);

  // 删除最后一项后页面为空，再次删除必须失败且 size 不下溢。
  EXPECT_TRUE(leaf->Remove(key30, comparator));
  EXPECT_EQ(leaf->GetSize(), 0);
  EXPECT_FALSE(leaf->Remove(key30, comparator));
  EXPECT_EQ(leaf->GetSize(), 0);
}

TEST(BPlusTreePageTest, LeafPageMoveHalfToTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page source_raw_page;
  Page recipient_raw_page;
  auto *source = reinterpret_cast<LeafPage *>(source_raw_page.GetData());
  auto *recipient = reinterpret_cast<LeafPage *>(recipient_raw_page.GetData());
  source->Init(5);
  recipient->Init(5);
  source->SetNextPageId(77);

  for (int64_t value = 10; value <= 50; value += 10) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(source->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }

  source->MoveHalfTo(recipient);

  ASSERT_EQ(source->GetSize(), 2);
  EXPECT_EQ(source->KeyAt(0).ToString(), 10);
  EXPECT_EQ(source->KeyAt(1).ToString(), 20);

  ASSERT_EQ(recipient->GetSize(), 3);
  EXPECT_EQ(recipient->KeyAt(0).ToString(), 30);
  EXPECT_EQ(recipient->KeyAt(1).ToString(), 40);
  EXPECT_EQ(recipient->KeyAt(2).ToString(), 50);
  EXPECT_EQ(recipient->ValueAt(0), RID(30, 1));

  // MoveHalfTo 只移动记录，叶子链表由 BPlusTree::Insert 负责连接。
  EXPECT_EQ(source->GetNextPageId(), 77);
  EXPECT_EQ(recipient->GetNextPageId(), INVALID_PAGE_ID);
}

TEST(BPlusTreePageTest, LeafPageMoveLastToFrontOfTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page left_raw_page;
  Page current_raw_page;
  auto *left = reinterpret_cast<LeafPage *>(left_raw_page.GetData());
  auto *current = reinterpret_cast<LeafPage *>(current_raw_page.GetData());
  left->Init(5);
  current->Init(5);
  left->SetNextPageId(101);
  current->SetNextPageId(202);

  for (int64_t value : {10, 20}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(left->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }
  for (int64_t value : {30, 40}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(current->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }

  left->MoveLastToFrontOf(current);

  ASSERT_EQ(left->GetSize(), 1);
  EXPECT_EQ(left->KeyAt(0).ToString(), 10);
  ASSERT_EQ(current->GetSize(), 3);
  EXPECT_EQ(current->KeyAt(0).ToString(), 20);
  EXPECT_EQ(current->KeyAt(1).ToString(), 30);
  EXPECT_EQ(current->KeyAt(2).ToString(), 40);
  EXPECT_EQ(current->ValueAt(0), RID(20, 1));

  // 重分配记录不会改变叶子链表。
  EXPECT_EQ(left->GetNextPageId(), 101);
  EXPECT_EQ(current->GetNextPageId(), 202);
}

TEST(BPlusTreePageTest, LeafPageMoveFirstToEndOfTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page current_raw_page;
  Page right_raw_page;
  auto *current = reinterpret_cast<LeafPage *>(current_raw_page.GetData());
  auto *right = reinterpret_cast<LeafPage *>(right_raw_page.GetData());
  current->Init(5);
  right->Init(5);
  current->SetNextPageId(101);
  right->SetNextPageId(202);

  for (int64_t value : {10, 20}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(current->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }
  for (int64_t value : {30, 40}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(right->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }

  right->MoveFirstToEndOf(current);

  ASSERT_EQ(current->GetSize(), 3);
  EXPECT_EQ(current->KeyAt(0).ToString(), 10);
  EXPECT_EQ(current->KeyAt(1).ToString(), 20);
  EXPECT_EQ(current->KeyAt(2).ToString(), 30);
  EXPECT_EQ(current->ValueAt(2), RID(30, 1));
  ASSERT_EQ(right->GetSize(), 1);
  EXPECT_EQ(right->KeyAt(0).ToString(), 40);

  // 重分配记录不会改变叶子链表。
  EXPECT_EQ(current->GetNextPageId(), 101);
  EXPECT_EQ(right->GetNextPageId(), 202);
}

TEST(BPlusTreePageTest, LeafPageMoveAllToTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page left_raw_page;
  Page right_raw_page;
  auto *left = reinterpret_cast<LeafPage *>(left_raw_page.GetData());
  auto *right = reinterpret_cast<LeafPage *>(right_raw_page.GetData());
  left->Init(5);
  right->Init(5);
  left->SetNextPageId(101);
  right->SetNextPageId(202);

  for (int64_t value : {10, 20}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(left->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }
  for (int64_t value : {30, 40}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(right->Insert(key, RID(static_cast<page_id_t>(value), 1), comparator));
  }

  right->MoveAllTo(left);

  ASSERT_EQ(left->GetSize(), 4);
  EXPECT_EQ(left->KeyAt(0).ToString(), 10);
  EXPECT_EQ(left->KeyAt(1).ToString(), 20);
  EXPECT_EQ(left->KeyAt(2).ToString(), 30);
  EXPECT_EQ(left->KeyAt(3).ToString(), 40);
  EXPECT_EQ(left->ValueAt(3), RID(40, 1));
  EXPECT_EQ(right->GetSize(), 0);

  // 右页将被回收，左页必须直接连接右页原来的后继。
  EXPECT_EQ(left->GetNextPageId(), 202);
}

TEST(BPlusTreePageTest, InternalPageInitAndAccessTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page raw_page;
  auto *internal = reinterpret_cast<InternalPage *>(raw_page.GetData());

  internal->Init(6);
  EXPECT_FALSE(internal->IsLeafPage());
  EXPECT_EQ(internal->GetSize(), 0);
  EXPECT_EQ(internal->GetMaxSize(), 6);
  EXPECT_EQ(internal->GetMinSize(), 3);

  Key key20;
  Key key40;
  Key key30;
  Key key19;
  Key key50;
  key20.SetFromInteger(20);
  key40.SetFromInteger(40);
  key30.SetFromInteger(30);
  key19.SetFromInteger(19);
  key50.SetFromInteger(50);

  auto *array = reinterpret_cast<InternalMapping *>(raw_page.GetData() + INTERNAL_PAGE_HEADER_SIZE);
  array[0].second = 10;
  array[1] = {key20, 20};
  array[2] = {key40, 40};
  array[3].second = 99;
  internal->SetSize(3);

  EXPECT_EQ(internal->ValueAt(0), 10);
  EXPECT_EQ(internal->ValueAt(1), 20);
  EXPECT_EQ(internal->ValueAt(2), 40);
  EXPECT_EQ(internal->ValueIndex(10), 0);
  EXPECT_EQ(internal->ValueIndex(40), 2);
  EXPECT_EQ(internal->ValueIndex(99), -1);

  EXPECT_EQ(internal->Lookup(key19, comparator), 10);
  EXPECT_EQ(internal->Lookup(key20, comparator), 20);
  EXPECT_EQ(internal->Lookup(key30, comparator), 20);
  EXPECT_EQ(internal->Lookup(key40, comparator), 40);
  EXPECT_EQ(internal->Lookup(key50, comparator), 40);

  EXPECT_EQ(internal->KeyAt(1).ToString(), 20);
  EXPECT_EQ(internal->KeyAt(2).ToString(), 40);
  internal->SetKeyAt(2, key30);
  EXPECT_EQ(internal->KeyAt(2).ToString(), 30);
}

TEST(BPlusTreePageTest, InternalPagePopulateAndInsertTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page raw_page;
  auto *internal = reinterpret_cast<InternalPage *>(raw_page.GetData());
  internal->Init(6);

  Key key20;
  Key key25;
  Key key30;
  Key key40;
  key20.SetFromInteger(20);
  key25.SetFromInteger(25);
  key30.SetFromInteger(30);
  key40.SetFromInteger(40);

  internal->PopulateNewRoot(10, key20, 20);
  ASSERT_EQ(internal->GetSize(), 2);
  EXPECT_EQ(internal->ValueAt(0), 10);
  EXPECT_EQ(internal->KeyAt(1).ToString(), 20);
  EXPECT_EQ(internal->ValueAt(1), 20);

  // 先在末尾插入，再在中间插入，验证右移过程不会覆盖已有数据。
  EXPECT_EQ(internal->InsertNodeAfter(20, key40, 40), 3);
  EXPECT_EQ(internal->InsertNodeAfter(20, key30, 30), 4);

  EXPECT_EQ(internal->ValueAt(0), 10);
  EXPECT_EQ(internal->ValueAt(1), 20);
  EXPECT_EQ(internal->ValueAt(2), 30);
  EXPECT_EQ(internal->ValueAt(3), 40);
  EXPECT_EQ(internal->KeyAt(1).ToString(), 20);
  EXPECT_EQ(internal->KeyAt(2).ToString(), 30);
  EXPECT_EQ(internal->KeyAt(3).ToString(), 40);

  EXPECT_EQ(internal->Lookup(key25, comparator), 20);
  EXPECT_EQ(internal->Lookup(key30, comparator), 30);
  EXPECT_EQ(internal->Lookup(key40, comparator), 40);
}

TEST(BPlusTreePageTest, InternalPageRemoveTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page raw_page;
  auto *internal = reinterpret_cast<InternalPage *>(raw_page.GetData());
  internal->Init(6);

  Key key20;
  Key key25;
  Key key30;
  Key key40;
  key20.SetFromInteger(20);
  key25.SetFromInteger(25);
  key30.SetFromInteger(30);
  key40.SetFromInteger(40);

  internal->PopulateNewRoot(10, key20, 20);
  ASSERT_EQ(internal->InsertNodeAfter(20, key30, 30), 3);
  ASSERT_EQ(internal->InsertNodeAfter(30, key40, 40), 4);

  // 删除中间分隔项 (30, child 30)，后面的 (40, child 40) 应向左移动。
  internal->Remove(2);
  ASSERT_EQ(internal->GetSize(), 3);
  EXPECT_EQ(internal->ValueAt(0), 10);
  EXPECT_EQ(internal->KeyAt(1).ToString(), 20);
  EXPECT_EQ(internal->ValueAt(1), 20);
  EXPECT_EQ(internal->KeyAt(2).ToString(), 40);
  EXPECT_EQ(internal->ValueAt(2), 40);
  EXPECT_EQ(internal->Lookup(key25, comparator), 20);
  EXPECT_EQ(internal->ValueIndex(30), -1);

  // 删除末尾项，再删除下标 1，最终只保留最左 child。
  internal->Remove(2);
  ASSERT_EQ(internal->GetSize(), 2);
  EXPECT_EQ(internal->ValueIndex(40), -1);

  internal->Remove(1);
  ASSERT_EQ(internal->GetSize(), 1);
  EXPECT_EQ(internal->ValueAt(0), 10);
  EXPECT_EQ(internal->ValueIndex(20), -1);
}

TEST(BPlusTreePageTest, InternalPageMoveHalfToTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Page source_raw_page;
  Page recipient_raw_page;
  auto *source = reinterpret_cast<InternalPage *>(source_raw_page.GetData());
  auto *recipient = reinterpret_cast<InternalPage *>(recipient_raw_page.GetData());
  source->Init(5);
  recipient->Init(5);

  Key key20;
  Key key30;
  Key key35;
  Key key40;
  Key key50;
  key20.SetFromInteger(20);
  key30.SetFromInteger(30);
  key35.SetFromInteger(35);
  key40.SetFromInteger(40);
  key50.SetFromInteger(50);

  source->PopulateNewRoot(10, key20, 20);
  ASSERT_EQ(source->InsertNodeAfter(20, key30, 30), 3);
  ASSERT_EQ(source->InsertNodeAfter(30, key40, 40), 4);
  ASSERT_EQ(source->InsertNodeAfter(40, key50, 50), 5);

  const Key promoted_key = source->MoveHalfTo(recipient);

  EXPECT_EQ(promoted_key.ToString(), 30);
  ASSERT_EQ(source->GetSize(), 2);
  EXPECT_EQ(source->ValueAt(0), 10);
  EXPECT_EQ(source->KeyAt(1).ToString(), 20);
  EXPECT_EQ(source->ValueAt(1), 20);

  ASSERT_EQ(recipient->GetSize(), 3);
  EXPECT_EQ(recipient->ValueAt(0), 30);
  EXPECT_EQ(recipient->KeyAt(1).ToString(), 40);
  EXPECT_EQ(recipient->ValueAt(1), 40);
  EXPECT_EQ(recipient->KeyAt(2).ToString(), 50);
  EXPECT_EQ(recipient->ValueAt(2), 50);

  // recipient 的下标 0 key 被忽略，但其 child 应负责 [30, 40) 的范围。
  EXPECT_EQ(recipient->Lookup(key30, comparator), 30);
  EXPECT_EQ(recipient->Lookup(key35, comparator), 30);
  EXPECT_EQ(recipient->Lookup(key40, comparator), 40);
}

TEST(BPlusTreePageTest, InternalPageRedistributeAndMergeTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());

  Key key20;
  Key key30;
  Key key40;
  Key key45;
  Key key50;
  Key key60;
  key20.SetFromInteger(20);
  key30.SetFromInteger(30);
  key40.SetFromInteger(40);
  key45.SetFromInteger(45);
  key50.SetFromInteger(50);
  key60.SetFromInteger(60);

  {
    Page left_raw_page;
    Page current_raw_page;
    auto *left = reinterpret_cast<InternalPage *>(left_raw_page.GetData());
    auto *current = reinterpret_cast<InternalPage *>(current_raw_page.GetData());
    left->Init(6);
    current->Init(6);

    left->PopulateNewRoot(10, key20, 20);
    ASSERT_EQ(left->InsertNodeAfter(20, key30, 30), 3);
    ASSERT_EQ(left->InsertNodeAfter(30, key40, 40), 4);
    current->PopulateNewRoot(50, key60, 60);

    // 父分隔 key=50 下沉到 current，左兄弟最后一个 key=40 上升成为新的父分隔 key。
    const Key new_parent_key = left->MoveLastToFrontOf(current, key50);
    EXPECT_EQ(new_parent_key.ToString(), 40);
    ASSERT_EQ(left->GetSize(), 3);
    EXPECT_EQ(left->ValueAt(2), 30);
    ASSERT_EQ(current->GetSize(), 3);
    EXPECT_EQ(current->ValueAt(0), 40);
    EXPECT_EQ(current->KeyAt(1).ToString(), 50);
    EXPECT_EQ(current->ValueAt(1), 50);
    EXPECT_EQ(current->KeyAt(2).ToString(), 60);
    EXPECT_EQ(current->Lookup(key45, comparator), 40);
    EXPECT_EQ(current->Lookup(key50, comparator), 50);
  }

  {
    Page current_raw_page;
    Page right_raw_page;
    auto *current = reinterpret_cast<InternalPage *>(current_raw_page.GetData());
    auto *right = reinterpret_cast<InternalPage *>(right_raw_page.GetData());
    current->Init(6);
    right->Init(6);

    current->PopulateNewRoot(10, key20, 20);
    right->PopulateNewRoot(30, key40, 40);
    ASSERT_EQ(right->InsertNodeAfter(40, key50, 50), 3);

    // 父分隔 key=30 下沉到 current，右兄弟 key[1]=40 上升成为新的父分隔 key。
    const Key new_parent_key = right->MoveFirstToEndOf(current, key30);
    EXPECT_EQ(new_parent_key.ToString(), 40);
    ASSERT_EQ(current->GetSize(), 3);
    EXPECT_EQ(current->KeyAt(2).ToString(), 30);
    EXPECT_EQ(current->ValueAt(2), 30);
    ASSERT_EQ(right->GetSize(), 2);
    EXPECT_EQ(right->ValueAt(0), 40);
    EXPECT_EQ(right->KeyAt(1).ToString(), 50);
    EXPECT_EQ(right->ValueAt(1), 50);
    EXPECT_EQ(right->Lookup(key45, comparator), 40);
  }

  {
    Page left_raw_page;
    Page right_raw_page;
    auto *left = reinterpret_cast<InternalPage *>(left_raw_page.GetData());
    auto *right = reinterpret_cast<InternalPage *>(right_raw_page.GetData());
    left->Init(6);
    right->Init(6);

    left->PopulateNewRoot(10, key20, 20);
    right->PopulateNewRoot(30, key40, 40);
    ASSERT_EQ(right->InsertNodeAfter(40, key50, 50), 3);

    // 右页最左 child 原本没有有效 key，合并时使用父分隔 key=30 将左右页连接起来。
    right->MoveAllTo(left, key30);
    EXPECT_EQ(right->GetSize(), 0);
    ASSERT_EQ(left->GetSize(), 5);
    EXPECT_EQ(left->ValueAt(0), 10);
    EXPECT_EQ(left->KeyAt(1).ToString(), 20);
    EXPECT_EQ(left->ValueAt(1), 20);
    EXPECT_EQ(left->KeyAt(2).ToString(), 30);
    EXPECT_EQ(left->ValueAt(2), 30);
    EXPECT_EQ(left->KeyAt(3).ToString(), 40);
    EXPECT_EQ(left->ValueAt(3), 40);
    EXPECT_EQ(left->KeyAt(4).ToString(), 50);
    EXPECT_EQ(left->ValueAt(4), 50);
  }
}

TEST(BPlusTreePageTest, HeaderPageRootStateTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(10, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 5, 6);
  EXPECT_TRUE(tree.IsEmpty());
  EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

  {
    auto header_guard = bpm.FetchPageWrite(header_page_id);
    auto *header_page = header_guard.AsMut<BPlusTreeHeaderPage>();
    header_page->root_page_id_ = 42;
  }

  EXPECT_FALSE(tree.IsEmpty());
  EXPECT_EQ(tree.GetRootPageId(), 42);
}

TEST(BPlusTreePageTest, BasicRootLeafRemoveTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(10, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 5, 6);
  Key key10;
  Key key20;
  Key key25;
  Key key30;
  Key key40;
  key10.SetFromInteger(10);
  key20.SetFromInteger(20);
  key25.SetFromInteger(25);
  key30.SetFromInteger(30);
  key40.SetFromInteger(40);

  // 空树删除应直接返回。
  tree.Remove(key10, nullptr);
  EXPECT_TRUE(tree.IsEmpty());

  ASSERT_TRUE(tree.Insert(key10, RID(10, 1)));
  ASSERT_TRUE(tree.Insert(key20, RID(20, 1)));
  ASSERT_TRUE(tree.Insert(key30, RID(30, 1)));
  const page_id_t original_root_page_id = tree.GetRootPageId();

  // 不存在的 key 不应影响根叶子内容。
  tree.Remove(key25, nullptr);
  std::vector<RID> result;
  EXPECT_FALSE(tree.GetValue(key25, &result));
  result.clear();
  EXPECT_TRUE(tree.GetValue(key20, &result));

  // 删除部分记录时根页面保持不变。
  tree.Remove(key20, nullptr);
  result.clear();
  EXPECT_FALSE(tree.GetValue(key20, &result));
  EXPECT_EQ(tree.GetRootPageId(), original_root_page_id);
  EXPECT_FALSE(tree.IsEmpty());

  tree.Remove(key10, nullptr);
  tree.Remove(key30, nullptr);
  EXPECT_TRUE(tree.IsEmpty());
  EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);

  // 删除旧根后应能重新创建一棵只有根叶子的树。
  ASSERT_TRUE(tree.Insert(key40, RID(40, 1)));
  result.clear();
  EXPECT_TRUE(tree.GetValue(key40, &result));
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], RID(40, 1));
}

TEST(BPlusTreePageTest, LeafRedistributeFromLeftTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(10, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 4, 4);
  for (int64_t value : {10, 20, 30, 40, 15}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  Key key40;
  key40.SetFromInteger(40);
  const page_id_t root_page_id = tree.GetRootPageId();
  tree.Remove(key40, nullptr);

  // 右叶子删除 40 后欠载，从左叶子借 20；父分隔 key 应随右叶子的最小 key 更新为 20。
  auto root_guard = bpm.FetchPageRead(root_page_id);
  const auto *root = root_guard.As<InternalPage>();
  ASSERT_FALSE(root->IsLeafPage());
  ASSERT_EQ(root->GetSize(), 2);
  EXPECT_EQ(root->KeyAt(1).ToString(), 20);

  auto left_guard = bpm.FetchPageRead(root->ValueAt(0));
  auto right_guard = bpm.FetchPageRead(root->ValueAt(1));
  const auto *left = left_guard.As<LeafPage>();
  const auto *right = right_guard.As<LeafPage>();
  ASSERT_EQ(left->GetSize(), 2);
  EXPECT_EQ(left->KeyAt(0).ToString(), 10);
  EXPECT_EQ(left->KeyAt(1).ToString(), 15);
  ASSERT_EQ(right->GetSize(), 2);
  EXPECT_EQ(right->KeyAt(0).ToString(), 20);
  EXPECT_EQ(right->KeyAt(1).ToString(), 30);
}

TEST(BPlusTreePageTest, LeafRedistributeFromRightTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(10, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 4, 4);
  for (int64_t value : {10, 20, 30, 40, 35}) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  Key key10;
  key10.SetFromInteger(10);
  const page_id_t root_page_id = tree.GetRootPageId();
  tree.Remove(key10, nullptr);

  // 左叶子删除 10 后欠载，从右叶子借 30；右叶子的新最小 key 变为 35。
  auto root_guard = bpm.FetchPageRead(root_page_id);
  const auto *root = root_guard.As<InternalPage>();
  ASSERT_FALSE(root->IsLeafPage());
  ASSERT_EQ(root->GetSize(), 2);
  EXPECT_EQ(root->KeyAt(1).ToString(), 35);

  auto left_guard = bpm.FetchPageRead(root->ValueAt(0));
  auto right_guard = bpm.FetchPageRead(root->ValueAt(1));
  const auto *left = left_guard.As<LeafPage>();
  const auto *right = right_guard.As<LeafPage>();
  ASSERT_EQ(left->GetSize(), 2);
  EXPECT_EQ(left->KeyAt(0).ToString(), 20);
  EXPECT_EQ(left->KeyAt(1).ToString(), 30);
  ASSERT_EQ(right->GetSize(), 2);
  EXPECT_EQ(right->KeyAt(0).ToString(), 35);
  EXPECT_EQ(right->KeyAt(1).ToString(), 40);
}

TEST(BPlusTreePageTest, LeafMergeAndRootShrinkTest) {
  // 分别删除最左叶子和最右叶子的记录，覆盖“右页并入当前页”和“当前页并入左页”两个分支。
  for (int64_t removed_key : {10, 40}) {
    SCOPED_TRACE(removed_key);
    auto key_schema = ParseCreateStatement("a bigint");
    GenericComparator<8> comparator(key_schema.get());
    auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
    BufferPoolManager bpm(10, disk_manager.get());

    page_id_t header_page_id = INVALID_PAGE_ID;
    {
      auto header_guard = bpm.NewPageGuarded(&header_page_id);
      ASSERT_NE(header_page_id, INVALID_PAGE_ID);
    }

    Tree tree("test", header_page_id, &bpm, comparator, 4, 4);
    for (int64_t value : {10, 20, 30, 40}) {
      Key key;
      key.SetFromInteger(value);
      ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
    }

    const page_id_t old_root_page_id = tree.GetRootPageId();
    page_id_t left_page_id = INVALID_PAGE_ID;
    {
      auto old_root_guard = bpm.FetchPageRead(old_root_page_id);
      const auto *old_root = old_root_guard.As<InternalPage>();
      ASSERT_FALSE(old_root->IsLeafPage());
      left_page_id = old_root->ValueAt(0);
    }

    Key key_to_remove;
    key_to_remove.SetFromInteger(removed_key);
    tree.Remove(key_to_remove, nullptr);

    // 两个最小容量叶子无法互借，只能合并；旧根仅剩一个 child 后应被该叶子替代。
    EXPECT_EQ(tree.GetRootPageId(), left_page_id);
    EXPECT_NE(tree.GetRootPageId(), old_root_page_id);
    auto new_root_guard = bpm.FetchPageRead(tree.GetRootPageId());
    const auto *new_root = new_root_guard.As<LeafPage>();
    ASSERT_TRUE(new_root->IsLeafPage());
    ASSERT_EQ(new_root->GetSize(), 3);

    int expected_index = 0;
    for (int64_t value : {10, 20, 30, 40}) {
      if (value == removed_key) {
        continue;
      }
      EXPECT_EQ(new_root->KeyAt(expected_index).ToString(), value);
      ++expected_index;
    }
    EXPECT_EQ(new_root->GetNextPageId(), INVALID_PAGE_ID);
  }
}

TEST(BPlusTreePageTest, RootLeafSplitTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(10, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 2, 3);
  Key key10;
  Key key15;
  Key key20;
  key10.SetFromInteger(10);
  key15.SetFromInteger(15);
  key20.SetFromInteger(20);

  ASSERT_TRUE(tree.Insert(key10, RID(10, 1)));
  ASSERT_TRUE(tree.Insert(key20, RID(20, 2)));

  page_id_t left_page_id = INVALID_PAGE_ID;
  page_id_t right_page_id = INVALID_PAGE_ID;
  {
    auto root_guard = bpm.FetchPageRead(tree.GetRootPageId());
    const auto *root = root_guard.As<InternalPage>();
    ASSERT_FALSE(root->IsLeafPage());
    ASSERT_EQ(root->GetSize(), 2);
    EXPECT_EQ(root->KeyAt(1).ToString(), 20);
    left_page_id = root->ValueAt(0);
    right_page_id = root->ValueAt(1);
  }

  {
    auto left_guard = bpm.FetchPageRead(left_page_id);
    const auto *left = left_guard.As<LeafPage>();
    ASSERT_EQ(left->GetSize(), 1);
    EXPECT_EQ(left->KeyAt(0).ToString(), 10);
    EXPECT_EQ(left->GetNextPageId(), right_page_id);
  }

  {
    auto right_guard = bpm.FetchPageRead(right_page_id);
    const auto *right = right_guard.As<LeafPage>();
    ASSERT_EQ(right->GetSize(), 1);
    EXPECT_EQ(right->KeyAt(0).ToString(), 20);
    EXPECT_EQ(right->GetNextPageId(), INVALID_PAGE_ID);
  }

  std::vector<RID> result;
  EXPECT_TRUE(tree.GetValue(key10, &result));
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], RID(10, 1));

  result.clear();
  EXPECT_TRUE(tree.GetValue(key20, &result));
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], RID(20, 2));

  result.clear();
  EXPECT_FALSE(tree.GetValue(key15, &result));
  EXPECT_TRUE(result.empty());
  EXPECT_FALSE(tree.Insert(key20, RID(99, 99)));
}

TEST(BPlusTreePageTest, NonRootLeafSplitWithParentSpaceTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(10, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 2, 3);
  Key key10;
  Key key20;
  Key key30;
  key10.SetFromInteger(10);
  key20.SetFromInteger(20);
  key30.SetFromInteger(30);

  ASSERT_TRUE(tree.Insert(key10, RID(10, 1)));
  ASSERT_TRUE(tree.Insert(key20, RID(20, 1)));
  const page_id_t root_page_id = tree.GetRootPageId();

  ASSERT_TRUE(tree.Insert(key30, RID(30, 1)));
  EXPECT_EQ(tree.GetRootPageId(), root_page_id);

  page_id_t leaf_page_ids[3] = {INVALID_PAGE_ID, INVALID_PAGE_ID, INVALID_PAGE_ID};
  {
    auto root_guard = bpm.FetchPageRead(root_page_id);
    const auto *root = root_guard.As<InternalPage>();
    ASSERT_EQ(root->GetSize(), 3);
    EXPECT_EQ(root->KeyAt(1).ToString(), 20);
    EXPECT_EQ(root->KeyAt(2).ToString(), 30);
    for (int index = 0; index < 3; ++index) {
      leaf_page_ids[index] = root->ValueAt(index);
    }
  }

  for (int index = 0; index < 3; ++index) {
    auto leaf_guard = bpm.FetchPageRead(leaf_page_ids[index]);
    const auto *leaf = leaf_guard.As<LeafPage>();
    ASSERT_EQ(leaf->GetSize(), 1);
    EXPECT_EQ(leaf->KeyAt(0).ToString(), (index + 1) * 10);
    const page_id_t expected_next = index == 2 ? INVALID_PAGE_ID : leaf_page_ids[index + 1];
    EXPECT_EQ(leaf->GetNextPageId(), expected_next);
  }

  for (int64_t value : {10, 20, 30}) {
    Key key;
    key.SetFromInteger(value);
    std::vector<RID> result;
    EXPECT_TRUE(tree.GetValue(key, &result));
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], RID(static_cast<page_id_t>(value), 1));
  }
}

TEST(BPlusTreePageTest, MultiLevelInsertAndSearchTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(50, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 2, 3);
  for (int64_t value = 1; value <= 3; ++value) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  const page_id_t root_before_internal_split = tree.GetRootPageId();
  Key key4;
  key4.SetFromInteger(4);
  ASSERT_TRUE(tree.Insert(key4, RID(4, 1)));
  const page_id_t root_after_internal_split = tree.GetRootPageId();
  EXPECT_NE(root_after_internal_split, root_before_internal_split);

  page_id_t root_children[2] = {INVALID_PAGE_ID, INVALID_PAGE_ID};
  {
    auto root_guard = bpm.FetchPageRead(root_after_internal_split);
    const auto *root = root_guard.As<InternalPage>();
    ASSERT_FALSE(root->IsLeafPage());
    ASSERT_EQ(root->GetSize(), 2);
    root_children[0] = root->ValueAt(0);
    root_children[1] = root->ValueAt(1);
  }

  for (page_id_t child_page_id : root_children) {
    auto child_guard = bpm.FetchPageRead(child_page_id);
    EXPECT_FALSE(child_guard.As<BPlusTreePage>()->IsLeafPage());
  }

  for (int64_t value = 5; value <= 12; ++value) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  for (int64_t value = 1; value <= 12; ++value) {
    Key key;
    key.SetFromInteger(value);
    std::vector<RID> result;
    EXPECT_TRUE(tree.GetValue(key, &result));
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], RID(static_cast<page_id_t>(value), 1));
    EXPECT_FALSE(tree.Insert(key, RID(99, 99)));
  }

  Key missing_key;
  missing_key.SetFromInteger(13);
  std::vector<RID> result;
  EXPECT_FALSE(tree.GetValue(missing_key, &result));
  EXPECT_TRUE(result.empty());
}

TEST(BPlusTreePageTest, MultiLevelDescendingInsertAndSearchTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(50, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 2, 3);
  for (int64_t value = 12; value >= 1; --value) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  for (int64_t value = 1; value <= 12; ++value) {
    Key key;
    key.SetFromInteger(value);
    std::vector<RID> result;
    EXPECT_TRUE(tree.GetValue(key, &result));
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0], RID(static_cast<page_id_t>(value), 1));
  }
}

TEST(BPlusTreePageTest, MultiLevelDeleteAndRootShrinkTest) {
  // 升序和降序删除分别覆盖最左路径、最右路径，并迫使欠载沿多层 InternalPage 向上传播。
  for (bool delete_ascending : {true, false}) {
    SCOPED_TRACE(delete_ascending ? "ascending" : "descending");
    auto key_schema = ParseCreateStatement("a bigint");
    GenericComparator<8> comparator(key_schema.get());
    auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
    BufferPoolManager bpm(50, disk_manager.get());

    page_id_t header_page_id = INVALID_PAGE_ID;
    {
      auto header_guard = bpm.NewPageGuarded(&header_page_id);
      ASSERT_NE(header_page_id, INVALID_PAGE_ID);
    }

    Tree tree("test", header_page_id, &bpm, comparator, 4, 4);
    constexpr int64_t key_count = 32;
    for (int64_t value = 1; value <= key_count; ++value) {
      Key key;
      key.SetFromInteger(value);
      ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
    }

    for (int64_t step = 0; step < key_count; ++step) {
      const int64_t value = delete_ascending ? step + 1 : key_count - step;
      Key key;
      key.SetFromInteger(value);
      tree.Remove(key, nullptr);

      std::vector<RID> result;
      EXPECT_FALSE(tree.GetValue(key, &result));
      EXPECT_TRUE(result.empty());

      // 每次删除后验证相邻的未删除 key，及时发现分隔 key 或 child 搬运错误。
      if (step + 1 < key_count) {
        const int64_t remaining_value = delete_ascending ? value + 1 : value - 1;
        Key remaining_key;
        remaining_key.SetFromInteger(remaining_value);
        result.clear();
        EXPECT_TRUE(tree.GetValue(remaining_key, &result));
        ASSERT_EQ(result.size(), 1);
        EXPECT_EQ(result[0], RID(static_cast<page_id_t>(remaining_value), 1));
      }
    }

    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);
  }
}

TEST(BPlusTreePageTest, RandomizedMultiLevelInsertDeleteTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(50, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 4, 4);
  constexpr int64_t key_count = 96;
  std::vector<int64_t> keys(key_count);
  std::iota(keys.begin(), keys.end(), 1);

  std::mt19937 generator(15445);
  std::vector<int64_t> insert_order = keys;
  std::shuffle(insert_order.begin(), insert_order.end(), generator);
  for (int64_t value : insert_order) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  std::vector<int64_t> delete_order = keys;
  std::shuffle(delete_order.begin(), delete_order.end(), generator);
  std::vector<bool> present(static_cast<size_t>(key_count + 1), true);
  for (size_t step = 0; step < delete_order.size(); ++step) {
    const int64_t value = delete_order[step];
    Key key;
    key.SetFromInteger(value);
    tree.Remove(key, nullptr);
    present[static_cast<size_t>(value)] = false;

    std::vector<RID> result;
    EXPECT_FALSE(tree.GetValue(key, &result));

    // 每八次删除全量核对一次，验证重分配或合并没有让其他 subtree 丢失。
    if (step % 8 == 0 || step + 1 == delete_order.size()) {
      for (int64_t candidate : keys) {
        Key candidate_key;
        candidate_key.SetFromInteger(candidate);
        result.clear();
        const bool found = tree.GetValue(candidate_key, &result);
        EXPECT_EQ(found, present[static_cast<size_t>(candidate)]);
        if (found) {
          ASSERT_EQ(result.size(), 1);
          EXPECT_EQ(result[0], RID(static_cast<page_id_t>(candidate), 1));
        }
      }
    }
  }

  EXPECT_TRUE(tree.IsEmpty());
  EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);
}

TEST(BPlusTreePageTest, SmallCapacityMultiLevelDeleteTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(50, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 2, 3);
  constexpr int64_t key_count = 12;
  for (int64_t value = 1; value <= key_count; ++value) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  for (int64_t value = 1; value <= key_count; ++value) {
    Key key;
    key.SetFromInteger(value);
    tree.Remove(key, nullptr);
    std::vector<RID> result;
    EXPECT_FALSE(tree.GetValue(key, &result));
  }

  EXPECT_TRUE(tree.IsEmpty());
  EXPECT_EQ(tree.GetRootPageId(), INVALID_PAGE_ID);
}

TEST(BPlusTreePageTest, ConcurrentSmallCapacityInsertDeleteTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(128, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  // 极小容量会频繁触发叶子分裂、InternalPage 逐层分裂、叶子合并和根收缩，
  // 比默认大页面更容易覆盖 latch crabbing 中“必须保留祖先”的不安全路径。
  Tree tree("concurrent-test", header_page_id, &bpm, comparator, 2, 3);
  constexpr int64_t key_count = 240;
  constexpr int thread_count = 6;
  std::atomic<bool> all_succeeded{true};

  // 每个线程负责互不重叠的 key，但这些 key 会交错落入同一批叶子，能够制造真实的页面锁竞争。
  std::vector<std::thread> workers;
  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    workers.emplace_back([&tree, &all_succeeded, thread_id]() {
      for (int64_t value = thread_id + 1; value <= key_count; value += thread_count) {
        Key key;
        key.SetFromInteger(value);
        if (!tree.Insert(key, RID(static_cast<page_id_t>(value), 1))) {
          all_succeeded.store(false, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  ASSERT_TRUE(all_succeeded.load(std::memory_order_relaxed));

  // 再并发删除四分之三的 key。删除线程同样会交错访问相邻叶子，迫使多个层级执行借位或合并。
  workers.clear();
  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    workers.emplace_back([&tree, thread_id]() {
      for (int64_t value = thread_id + 1; value <= key_count; value += thread_count) {
        if (value % 4 != 0) {
          Key key;
          key.SetFromInteger(value);
          tree.Remove(key, nullptr);
        }
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }

  // 并发阶段结束后同时核对点查和叶子链，确保没有因过早释放祖先而丢失 child 或破坏 next_page_id。
  for (int64_t value = 1; value <= key_count; ++value) {
    Key key;
    key.SetFromInteger(value);
    std::vector<RID> result;
    const bool found = tree.GetValue(key, &result);
    EXPECT_EQ(found, value % 4 == 0);
    if (found) {
      ASSERT_EQ(result.size(), 1);
      EXPECT_EQ(result[0], RID(static_cast<page_id_t>(value), 1));
    }
  }

  int64_t expected = 4;
  for (auto iterator = tree.Begin(); iterator != tree.End(); ++iterator) {
    EXPECT_EQ((*iterator).first.ToString(), expected);
    expected += 4;
  }
  EXPECT_EQ(expected, key_count + 4);
}

TEST(BPlusTreePageTest, IndexIteratorEmptyAndLowerBoundTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(10, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 5, 5);
  Key key5;
  Key key10;
  Key key20;
  Key key25;
  Key key30;
  Key key31;
  key5.SetFromInteger(5);
  key10.SetFromInteger(10);
  key20.SetFromInteger(20);
  key25.SetFromInteger(25);
  key30.SetFromInteger(30);
  key31.SetFromInteger(31);

  auto empty_begin = tree.Begin();
  auto empty_lower_bound = tree.Begin(key20);
  auto empty_end = tree.End();
  EXPECT_TRUE(empty_begin.IsEnd());
  EXPECT_TRUE(empty_lower_bound.IsEnd());
  EXPECT_EQ(empty_begin, empty_end);

  ASSERT_TRUE(tree.Insert(key10, RID(10, 1)));
  ASSERT_TRUE(tree.Insert(key20, RID(20, 1)));
  ASSERT_TRUE(tree.Insert(key30, RID(30, 1)));

  // Begin(key) 的语义是 lower_bound：返回第一个 >= key 的记录，不要求 key 必须存在。
  auto before_first = tree.Begin(key5);
  ASSERT_FALSE(before_first.IsEnd());
  EXPECT_EQ((*before_first).first.ToString(), 10);

  auto exact = tree.Begin(key20);
  ASSERT_FALSE(exact.IsEnd());
  EXPECT_EQ((*exact).first.ToString(), 20);
  EXPECT_EQ((*exact).second, RID(20, 1));

  auto between = tree.Begin(key25);
  ASSERT_FALSE(between.IsEnd());
  EXPECT_EQ((*between).first.ToString(), 30);

  auto after_last = tree.Begin(key31);
  auto end = tree.End();
  EXPECT_TRUE(after_last.IsEnd());
  EXPECT_EQ(after_last, end);
}

TEST(BPlusTreePageTest, IndexIteratorCrossLeafAndMoveTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(20, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  // leaf_max_size=2 会让每个稳定叶子只保存很少记录，可频繁验证跨页递增。
  Tree tree("test", header_page_id, &bpm, comparator, 2, 3);
  constexpr int64_t key_count = 24;
  for (int64_t value = key_count; value >= 1; --value) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }

  // Iterator 接管 ReadPageGuard，因此移动后源对象应安全变成 End，且不会重复 unpin。
  auto source = tree.Begin();
  auto moved = std::move(source);
  EXPECT_TRUE(source.IsEnd());
  ASSERT_FALSE(moved.IsEnd());
  EXPECT_EQ((*moved).first.ToString(), 1);

  auto assigned = tree.End();
  assigned = std::move(moved);
  EXPECT_TRUE(moved.IsEnd());
  ASSERT_FALSE(assigned.IsEnd());
  EXPECT_EQ((*assigned).first.ToString(), 1);

  int64_t expected = 1;
  for (auto iterator = tree.Begin(); iterator != tree.End(); ++iterator) {
    EXPECT_EQ((*iterator).first.ToString(), expected);
    EXPECT_EQ((*iterator).second, RID(static_cast<page_id_t>(expected), 1));
    ++expected;
  }
  EXPECT_EQ(expected, key_count + 1);
}

TEST(BPlusTreePageTest, IndexIteratorAfterDeleteTest) {
  auto key_schema = ParseCreateStatement("a bigint");
  GenericComparator<8> comparator(key_schema.get());
  auto disk_manager = std::make_unique<DiskManagerUnlimitedMemory>();
  BufferPoolManager bpm(30, disk_manager.get());

  page_id_t header_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = bpm.NewPageGuarded(&header_page_id);
    ASSERT_NE(header_page_id, INVALID_PAGE_ID);
  }

  Tree tree("test", header_page_id, &bpm, comparator, 4, 4);
  constexpr int64_t key_count = 32;
  for (int64_t value = 1; value <= key_count; ++value) {
    Key key;
    key.SetFromInteger(value);
    ASSERT_TRUE(tree.Insert(key, RID(static_cast<page_id_t>(value), 1)));
  }
  for (int64_t value = 1; value <= key_count; value += 2) {
    Key key;
    key.SetFromInteger(value);
    tree.Remove(key, nullptr);
  }

  // 删除触发叶子合并后，next_page_id 链仍应让 Iterator 只扫描到所有偶数 key。
  int64_t expected = 2;
  for (auto iterator = tree.Begin(); iterator != tree.End(); ++iterator) {
    EXPECT_EQ((*iterator).first.ToString(), expected);
    expected += 2;
  }
  EXPECT_EQ(expected, key_count + 2);

  Key deleted_key;
  deleted_key.SetFromInteger(15);
  auto lower_bound = tree.Begin(deleted_key);
  ASSERT_FALSE(lower_bound.IsEnd());
  EXPECT_EQ((*lower_bound).first.ToString(), 16);

  Key last_key;
  last_key.SetFromInteger(key_count);
  auto last = tree.Begin(last_key);
  ASSERT_FALSE(last.IsEnd());
  EXPECT_EQ((*last).first.ToString(), key_count);
  ++last;
  EXPECT_TRUE(last.IsEnd());
  EXPECT_EQ(last, tree.End());
}

}  // namespace bustub
