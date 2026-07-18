//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager_test.cpp
//
// Identification: test/buffer/buffer_pool_manager_test.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/buffer_pool_manager.h"

#include <array>
#include <cstdio>
#include <random>
#include <string>

#include "storage/disk/disk_manager_memory.h"

#include "gtest/gtest.h"

namespace bustub {

// NOLINTNEXTLINE
// Check whether pages containing terminal characters can be recovered
TEST(BufferPoolManagerTest, DISABLED_BinaryDataTest) {
  const std::string db_name = "test.db";
  const size_t buffer_pool_size = 10;
  const size_t k = 5;

  std::random_device r;
  std::default_random_engine rng(r());
  std::uniform_int_distribution<char> uniform_dist(0);

  auto *disk_manager = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(buffer_pool_size, disk_manager, k);

  page_id_t page_id_temp;
  auto *page0 = bpm->NewPage(&page_id_temp);

  // Scenario: The buffer pool is empty. We should be able to create a new page.
  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(0, page_id_temp);

  char random_binary_data[BUSTUB_PAGE_SIZE];
  // Generate random binary data
  for (char &i : random_binary_data) {
    i = uniform_dist(rng);
  }

  // Insert terminal characters both in the middle and at end
  random_binary_data[BUSTUB_PAGE_SIZE / 2] = '\0';
  random_binary_data[BUSTUB_PAGE_SIZE - 1] = '\0';

  // Scenario: Once we have a page, we should be able to read and write content.
  std::memcpy(page0->GetData(), random_binary_data, BUSTUB_PAGE_SIZE);
  EXPECT_EQ(0, std::memcmp(page0->GetData(), random_binary_data, BUSTUB_PAGE_SIZE));

  // Scenario: We should be able to create new pages until we fill up the buffer pool.
  for (size_t i = 1; i < buffer_pool_size; ++i) {
    EXPECT_NE(nullptr, bpm->NewPage(&page_id_temp));
  }

  // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
  for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
    EXPECT_EQ(nullptr, bpm->NewPage(&page_id_temp));
  }

  // Scenario: After unpinning pages {0, 1, 2, 3, 4} we should be able to create 5 new pages
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(true, bpm->UnpinPage(i, true));
    bpm->FlushPage(i);
  }
  for (int i = 0; i < 5; ++i) {
    EXPECT_NE(nullptr, bpm->NewPage(&page_id_temp));
    bpm->UnpinPage(page_id_temp, false);
  }
  // Scenario: We should be able to fetch the data we wrote a while ago.
  page0 = bpm->FetchPage(0);
  EXPECT_EQ(0, memcmp(page0->GetData(), random_binary_data, BUSTUB_PAGE_SIZE));
  EXPECT_EQ(true, bpm->UnpinPage(0, true));

  // Shutdown the disk manager and remove the temporary file we created.
  disk_manager->ShutDown();
  remove("test.db");

  delete bpm;
  delete disk_manager;
}

// NOLINTNEXTLINE
TEST(BufferPoolManagerTest, DISABLED_SampleTest) {
  const std::string db_name = "test.db";
  const size_t buffer_pool_size = 10;
  const size_t k = 5;

  auto *disk_manager = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(buffer_pool_size, disk_manager, k);

  page_id_t page_id_temp;
  auto *page0 = bpm->NewPage(&page_id_temp);

  // Scenario: The buffer pool is empty. We should be able to create a new page.
  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(0, page_id_temp);

  // Scenario: Once we have a page, we should be able to read and write content.
  snprintf(page0->GetData(), BUSTUB_PAGE_SIZE, "Hello");
  EXPECT_EQ(0, strcmp(page0->GetData(), "Hello"));

  // Scenario: We should be able to create new pages until we fill up the buffer pool.
  for (size_t i = 1; i < buffer_pool_size; ++i) {
    EXPECT_NE(nullptr, bpm->NewPage(&page_id_temp));
  }

  // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
  for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
    EXPECT_EQ(nullptr, bpm->NewPage(&page_id_temp));
  }

  // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new pages,
  // there would still be one buffer page left for reading page 0.
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(true, bpm->UnpinPage(i, true));
  }
  for (int i = 0; i < 4; ++i) {
    EXPECT_NE(nullptr, bpm->NewPage(&page_id_temp));
  }

  // Scenario: We should be able to fetch the data we wrote a while ago.
  page0 = bpm->FetchPage(0);
  EXPECT_EQ(0, strcmp(page0->GetData(), "Hello"));

  // Scenario: If we unpin page 0 and then make a new page, all the buffer pages should
  // now be pinned. Fetching page 0 should fail.
  EXPECT_EQ(true, bpm->UnpinPage(0, true));
  EXPECT_NE(nullptr, bpm->NewPage(&page_id_temp));
  EXPECT_EQ(nullptr, bpm->FetchPage(0));

  // Shutdown the disk manager and remove the temporary file we created.
  disk_manager->ShutDown();
  remove("test.db");

  delete bpm;
  delete disk_manager;
}

// 检查缓存命中时的重复 pin，以及 dirty 标记不会被后续的 clean unpin 清除。
TEST(BufferPoolManagerTest, PinUnpinAndDirtyStateTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(2, &disk_manager, 2);

  page_id_t page_id;
  Page *page = bpm.NewPage(&page_id);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->GetPinCount(), 1);

  Page *same_page = bpm.FetchPage(page_id);
  ASSERT_EQ(same_page, page);
  EXPECT_EQ(page->GetPinCount(), 2);

  EXPECT_TRUE(bpm.UnpinPage(page_id, true));
  EXPECT_EQ(page->GetPinCount(), 1);
  EXPECT_TRUE(page->IsDirty());

  EXPECT_TRUE(bpm.UnpinPage(page_id, false));
  EXPECT_EQ(page->GetPinCount(), 0);
  EXPECT_TRUE(page->IsDirty());

  // pin_count 已经为 0，重复 Unpin 必须失败且不能下溢。
  EXPECT_FALSE(bpm.UnpinPage(page_id, false));
  EXPECT_EQ(page->GetPinCount(), 0);
  EXPECT_FALSE(bpm.UnpinPage(page_id + 100, false));
}

// NewPage 失败时不应提前分配 page id。
TEST(BufferPoolManagerTest, FailedNewPageDoesNotConsumePageIdTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(1, &disk_manager, 2);

  page_id_t page0_id;
  ASSERT_NE(bpm.NewPage(&page0_id), nullptr);
  ASSERT_EQ(page0_id, 0);

  page_id_t failed_page_id = INVALID_PAGE_ID;
  EXPECT_EQ(bpm.NewPage(&failed_page_id), nullptr);

  ASSERT_TRUE(bpm.UnpinPage(page0_id, false));
  page_id_t page1_id;
  ASSERT_NE(bpm.NewPage(&page1_id), nullptr);
  EXPECT_EQ(page1_id, 1);
}

// 脏页被淘汰时必须先写回磁盘，之后 FetchPage 应能恢复原始内容。
TEST(BufferPoolManagerTest, DirtyEvictionPersistsDataTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(1, &disk_manager, 2);

  page_id_t page0_id;
  Page *page0 = bpm.NewPage(&page0_id);
  ASSERT_NE(page0, nullptr);
  snprintf(page0->GetData(), BUSTUB_PAGE_SIZE, "dirty page data");
  ASSERT_TRUE(bpm.UnpinPage(page0_id, true));

  // pool 只有一个 frame，新页面会淘汰 page0 并触发脏页写回。
  page_id_t page1_id;
  ASSERT_NE(bpm.NewPage(&page1_id), nullptr);
  ASSERT_TRUE(bpm.UnpinPage(page1_id, false));

  page0 = bpm.FetchPage(page0_id);
  ASSERT_NE(page0, nullptr);
  EXPECT_STREQ(page0->GetData(), "dirty page data");
  EXPECT_TRUE(bpm.UnpinPage(page0_id, false));
}

// FlushPage 和 FlushAllPages 都应写回页面并清除 dirty 标记。
TEST(BufferPoolManagerTest, FlushPageAndFlushAllPagesTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(2, &disk_manager, 2);

  page_id_t page0_id;
  Page *page0 = bpm.NewPage(&page0_id);
  ASSERT_NE(page0, nullptr);
  snprintf(page0->GetData(), BUSTUB_PAGE_SIZE, "single flush");
  ASSERT_TRUE(bpm.UnpinPage(page0_id, true));
  ASSERT_TRUE(page0->IsDirty());

  EXPECT_FALSE(bpm.FlushPage(INVALID_PAGE_ID));
  EXPECT_FALSE(bpm.FlushPage(page0_id + 100));
  ASSERT_TRUE(bpm.FlushPage(page0_id));
  EXPECT_FALSE(page0->IsDirty());

  std::array<char, BUSTUB_PAGE_SIZE> disk_data{};
  disk_manager.ReadPage(page0_id, disk_data.data());
  EXPECT_STREQ(disk_data.data(), "single flush");

  // 让两个驻留页面都变脏，再统一刷新。
  page0 = bpm.FetchPage(page0_id);
  ASSERT_NE(page0, nullptr);
  snprintf(page0->GetData(), BUSTUB_PAGE_SIZE, "flush all page 0");
  ASSERT_TRUE(bpm.UnpinPage(page0_id, true));

  page_id_t page1_id;
  Page *page1 = bpm.NewPage(&page1_id);
  ASSERT_NE(page1, nullptr);
  snprintf(page1->GetData(), BUSTUB_PAGE_SIZE, "flush all page 1");
  ASSERT_TRUE(bpm.UnpinPage(page1_id, true));

  bpm.FlushAllPages();
  EXPECT_FALSE(page0->IsDirty());
  EXPECT_FALSE(page1->IsDirty());

  std::array<char, BUSTUB_PAGE_SIZE> page0_disk_data{};
  std::array<char, BUSTUB_PAGE_SIZE> page1_disk_data{};
  disk_manager.ReadPage(page0_id, page0_disk_data.data());
  disk_manager.ReadPage(page1_id, page1_disk_data.data());
  EXPECT_STREQ(page0_disk_data.data(), "flush all page 0");
  EXPECT_STREQ(page1_disk_data.data(), "flush all page 1");
}

// pinned 页面不能删除；删除成功后应重置 Page，并把 frame 放回 free list。
TEST(BufferPoolManagerTest, DeletePageAndReuseFrameTest) {
  DiskManagerUnlimitedMemory disk_manager;
  BufferPoolManager bpm(1, &disk_manager, 2);

  page_id_t page0_id;
  Page *page0 = bpm.NewPage(&page0_id);
  ASSERT_NE(page0, nullptr);
  page0->GetData()[0] = 'x';

  EXPECT_FALSE(bpm.DeletePage(page0_id));
  EXPECT_EQ(page0->GetPageId(), page0_id);

  ASSERT_TRUE(bpm.UnpinPage(page0_id, true));
  ASSERT_TRUE(bpm.DeletePage(page0_id));
  EXPECT_EQ(page0->GetPageId(), INVALID_PAGE_ID);
  EXPECT_EQ(page0->GetPinCount(), 0);
  EXPECT_FALSE(page0->IsDirty());
  EXPECT_EQ(page0->GetData()[0], '\0');

  EXPECT_FALSE(bpm.UnpinPage(page0_id, false));
  EXPECT_TRUE(bpm.DeletePage(page0_id));

  page_id_t page1_id;
  Page *page1 = bpm.NewPage(&page1_id);
  ASSERT_NE(page1, nullptr);
  EXPECT_EQ(page1, page0);
  EXPECT_EQ(page1_id, 1);
  EXPECT_EQ(page1->GetData()[0], '\0');
}
}  // namespace bustub
