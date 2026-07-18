//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// page_guard_test.cpp
//
// Identification: test/storage/page_guard_test.cpp
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <random>
#include <string>
#include <utility>

#include "buffer/buffer_pool_manager.h"
#include "storage/disk/disk_manager_memory.h"
#include "storage/page/page_guard.h"

#include "gtest/gtest.h"

namespace bustub {

// NOLINTNEXTLINE
TEST(PageGuardTest, DISABLED_SampleTest) {
  const std::string db_name = "test.db";
  const size_t buffer_pool_size = 5;
  const size_t k = 2;

  auto disk_manager = std::make_shared<DiskManagerUnlimitedMemory>();
  auto bpm = std::make_shared<BufferPoolManager>(buffer_pool_size, disk_manager.get(), k);

  page_id_t page_id_temp;
  auto *page0 = bpm->NewPage(&page_id_temp);

  auto guarded_page = BasicPageGuard(bpm.get(), page0);

  EXPECT_EQ(page0->GetData(), guarded_page.GetData());
  EXPECT_EQ(page0->GetPageId(), guarded_page.PageId());
  EXPECT_EQ(1, page0->GetPinCount());

  guarded_page.Drop();

  EXPECT_EQ(0, page0->GetPinCount());

  // Shutdown the disk manager and remove the temporary file we created.
  disk_manager->ShutDown();
}

// BasicPageGuard 离开作用域时应自动 Unpin，并把可写访问产生的 dirty 状态传给 BPM。
TEST(PageGuardTest, BasicGuardDestructorAndDirtyTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(1, &disk_manager, 2);

  page_id_t page_id = INVALID_PAGE_ID;
  Page *page = nullptr;
  {
    auto guard = bpm.NewPageGuarded(&page_id);
    ASSERT_EQ(page_id, 0);

    page = bpm.GetPages();
    ASSERT_EQ(page->GetPinCount(), 1);
    EXPECT_EQ(guard.PageId(), page_id);
    snprintf(guard.GetDataMut(), BUSTUB_PAGE_SIZE, "basic guard data");
  }

  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->GetPinCount(), 0);
  EXPECT_TRUE(page->IsDirty());
  EXPECT_FALSE(bpm.UnpinPage(page_id, false));
}

// 移动赋值应先释放目标的旧页面；移动后只有新 Guard 可以释放源页面。
TEST(PageGuardTest, BasicGuardMoveSemanticsTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(2, &disk_manager, 2);

  page_id_t page0_id;
  page_id_t page1_id;
  Page *page0 = bpm.NewPage(&page0_id);
  Page *page1 = bpm.NewPage(&page1_id);
  ASSERT_NE(page0, nullptr);
  ASSERT_NE(page1, nullptr);

  BasicPageGuard guard0(&bpm, page0);
  BasicPageGuard guard1(&bpm, page1);
  guard0.GetDataMut()[0] = 'x';

  // 自移动赋值不能释放或改变当前资源。
  BasicPageGuard &basic_guard_alias = guard0;
  guard0 = std::move(basic_guard_alias);
  EXPECT_EQ(page0->GetPinCount(), 1);

  guard0 = std::move(guard1);
  EXPECT_EQ(page0->GetPinCount(), 0);
  EXPECT_TRUE(page0->IsDirty());
  EXPECT_EQ(page1->GetPinCount(), 1);

  // guard1 已被移动，Drop 不应再次减少 pin_count。
  guard1.Drop();
  EXPECT_EQ(page1->GetPinCount(), 1);

  BasicPageGuard guard2(std::move(guard0));
  guard0.Drop();
  EXPECT_EQ(page1->GetPinCount(), 1);

  guard2.Drop();
  EXPECT_EQ(page1->GetPinCount(), 0);
  guard2.Drop();
  EXPECT_EQ(page1->GetPinCount(), 0);
}

// ReadPageGuard 的移动赋值应释放目标页面的读锁，并且不能重复 Unpin 源页面。
TEST(PageGuardTest, ReadGuardMoveAndUnlockTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(2, &disk_manager, 2);

  page_id_t page0_id;
  page_id_t page1_id;
  Page *page0 = bpm.NewPage(&page0_id);
  Page *page1 = bpm.NewPage(&page1_id);
  ASSERT_NE(page0, nullptr);
  ASSERT_NE(page1, nullptr);
  ASSERT_TRUE(bpm.UnpinPage(page0_id, false));
  ASSERT_TRUE(bpm.UnpinPage(page1_id, false));

  auto guard0 = bpm.FetchPageRead(page0_id);
  auto guard1 = bpm.FetchPageRead(page1_id);
  ASSERT_EQ(page0->GetPinCount(), 1);
  ASSERT_EQ(page1->GetPinCount(), 1);

  ReadPageGuard &read_guard_alias = guard0;
  guard0 = std::move(read_guard_alias);
  EXPECT_EQ(page0->GetPinCount(), 1);

  guard0 = std::move(guard1);
  EXPECT_EQ(page0->GetPinCount(), 0);
  EXPECT_EQ(page1->GetPinCount(), 1);
  guard1.Drop();
  EXPECT_EQ(page1->GetPinCount(), 1);

  // 如果移动赋值没有释放 page0 的读锁，这里获取写锁会阻塞。
  auto page0_writer = bpm.FetchPageWrite(page0_id);
  EXPECT_EQ(page0->GetPinCount(), 1);
  page0_writer.Drop();
  EXPECT_EQ(page0->GetPinCount(), 0);

  ReadPageGuard guard2(std::move(guard0));
  guard0.Drop();
  EXPECT_EQ(page1->GetPinCount(), 1);
  guard2.Drop();
  EXPECT_EQ(page1->GetPinCount(), 0);

  // 如果 guard2 没有释放 page1 的读锁，这里获取写锁会阻塞。
  auto page1_writer = bpm.FetchPageWrite(page1_id);
  page1_writer.Drop();
  EXPECT_EQ(page1->GetPinCount(), 0);
}

// WritePageGuard 应转移写锁和 dirty 状态，并在移动赋值时释放目标页面的旧写锁。
TEST(PageGuardTest, WriteGuardMoveDirtyAndUnlockTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(2, &disk_manager, 2);

  page_id_t page0_id;
  page_id_t page1_id;
  Page *page0 = bpm.NewPage(&page0_id);
  Page *page1 = bpm.NewPage(&page1_id);
  ASSERT_NE(page0, nullptr);
  ASSERT_NE(page1, nullptr);
  ASSERT_TRUE(bpm.UnpinPage(page0_id, false));
  ASSERT_TRUE(bpm.UnpinPage(page1_id, false));

  auto guard0 = bpm.FetchPageWrite(page0_id);
  auto guard1 = bpm.FetchPageWrite(page1_id);
  snprintf(guard0.GetDataMut(), BUSTUB_PAGE_SIZE, "page 0 data");
  snprintf(guard1.GetDataMut(), BUSTUB_PAGE_SIZE, "page 1 data");

  guard0 = std::move(guard1);
  EXPECT_EQ(page0->GetPinCount(), 0);
  EXPECT_TRUE(page0->IsDirty());
  EXPECT_EQ(page1->GetPinCount(), 1);
  guard1.Drop();
  EXPECT_EQ(page1->GetPinCount(), 1);

  WritePageGuard guard2(std::move(guard0));
  guard0.Drop();
  EXPECT_EQ(page1->GetPinCount(), 1);

  WritePageGuard &write_guard_alias = guard2;
  guard2 = std::move(write_guard_alias);
  EXPECT_EQ(page1->GetPinCount(), 1);
  guard2.Drop();
  EXPECT_EQ(page1->GetPinCount(), 0);
  EXPECT_TRUE(page1->IsDirty());

  // 两个写锁均应释放，现在可以重新获取读锁并检查数据。
  auto page0_reader = bpm.FetchPageRead(page0_id);
  EXPECT_STREQ(page0_reader.GetData(), "page 0 data");
  page0_reader.Drop();

  auto page1_reader = bpm.FetchPageRead(page1_id);
  EXPECT_STREQ(page1_reader.GetData(), "page 1 data");
  page1_reader.Drop();
}

// WritePageGuard 析构产生的 dirty 状态应在淘汰时写回，并能重新读取。
TEST(PageGuardTest, WriteGuardDirtyEvictionTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(1, &disk_manager, 2);

  page_id_t page0_id;
  Page *page0 = bpm.NewPage(&page0_id);
  ASSERT_NE(page0, nullptr);
  ASSERT_TRUE(bpm.UnpinPage(page0_id, false));

  {
    auto guard = bpm.FetchPageWrite(page0_id);
    snprintf(guard.GetDataMut(), BUSTUB_PAGE_SIZE, "persisted by guard");
  }
  EXPECT_EQ(page0->GetPinCount(), 0);
  EXPECT_TRUE(page0->IsDirty());

  // pool 只有一个 frame，新页会淘汰 page0，并将 Guard 标记的脏数据写回。
  page_id_t page1_id;
  ASSERT_NE(bpm.NewPage(&page1_id), nullptr);
  ASSERT_TRUE(bpm.UnpinPage(page1_id, false));

  auto reader = bpm.FetchPageRead(page0_id);
  EXPECT_STREQ(reader.GetData(), "persisted by guard");
  reader.Drop();
}
}  // namespace bustub
