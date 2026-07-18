//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager.cpp
//
// Identification: src/buffer/buffer_pool_manager.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/buffer_pool_manager.h"

#include "common/exception.h"
#include "common/macros.h"
#include "storage/page/page_guard.h"

namespace bustub {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager, size_t replacer_k,
                                     LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager) {
  // we allocate a consecutive memory space for the buffer pool
  pages_ = new Page[pool_size_];
  replacer_ = std::make_unique<LRUKReplacer>(pool_size, replacer_k);

  // Initially, every page is in the free list.
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_.emplace_back(static_cast<int>(i));
  }
}

BufferPoolManager::~BufferPoolManager() { delete[] pages_; }

auto BufferPoolManager::TakeFrame() -> frame_id_t {
  frame_id_t frame_id = INVALID_PAGE_ID;

  // 优先使用从未存放页面的空闲 frame，避免不必要的淘汰和磁盘写入。
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else if (!replacer_->Evict(&frame_id)) {
    // 空闲列表和 replacer 都无法提供 frame，说明所有 frame 当前都处于 pinned 状态。
    return INVALID_PAGE_ID;
  }

  BUSTUB_ASSERT(frame_id >= 0 && static_cast<size_t>(frame_id) < pool_size_, "Invalid frame selected for reuse");

  Page &page = pages_[frame_id];
  BUSTUB_ASSERT(page.pin_count_ == 0, "A pinned page cannot be reused");

  if (page.page_id_ != INVALID_PAGE_ID) {
    // 覆盖 frame 前必须先写回脏页，否则内存中的修改会丢失。
    if (page.is_dirty_) {
      disk_manager_->WritePage(page.page_id_, page.data_);
    }

    // 此 frame 即将被复用，旧页面已不再驻留于缓冲池，必须删除对应的页表映射。
    page_table_.erase(page.page_id_);
  }

  // 清空页面数据及元数据，让 NewPage() 或 FetchPage() 可以安全地安装新页面。
  page.ResetMemory();
  page.page_id_ = INVALID_PAGE_ID;
  page.pin_count_ = 0;
  page.is_dirty_ = false;

  return frame_id;
}

auto BufferPoolManager::NewPage(page_id_t *page_id) -> Page * {
  // TODO(P1): 1. 获取 latch_，保证选取 frame 和修改缓冲池元数据的过程是线程安全的。
  std::unique_lock<std::mutex> lock(latch_);
  // TODO(P1): 2. 调用 TakeFrame() 取得可复用的 frame；若没有可用 frame，则返回 nullptr。
  auto frame_id = TakeFrame();
  if (frame_id == INVALID_PAGE_ID) {
    return nullptr;
  }
  // TODO(P1): 3. 调用 AllocatePage() 分配新的 page id。
  auto new_page_id = AllocatePage();
  // TODO(P1): 4. 初始化目标 Page：清空数据，设置 page id、pin_count_ = 1、is_dirty_ = false。
  Page &page = pages_[frame_id];
  page.page_id_ = new_page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;
  // TODO(P1): 5. 在 page_table_ 中建立新 page id 到 frame id 的映射。
  page_table_[new_page_id] = frame_id;
  // TODO(P1): 6. 调用 replacer_->RecordAccess() 记录本次访问，并将该 frame 标记为不可淘汰。
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  // TODO(P1): 7. 将新 page id 写入输出参数 page_id，最后返回目标 Page 指针。
  *page_id = new_page_id;
  return &page;
}

auto BufferPoolManager::FetchPage(page_id_t page_id, [[maybe_unused]] AccessType access_type) -> Page * {
  // TODO(P1): 1. 获取 latch_，保护 page_table_、free_list_、replacer_ 和 Page 元数据。
  std::unique_lock<std::mutex> lock(latch_);

  // TODO(P1): 2. 在 page_table_ 中查找 page_id；若命中，进入缓存命中流程。
  auto page_table_iter = page_table_.find(page_id);
  if (page_table_iter != page_table_.end()) {
    const frame_id_t frame_id = page_table_iter->second;
    Page &page = pages_[frame_id];

    // TODO(P1): 3. 缓存命中时递增 pin_count_，记录访问，并将 frame 标记为不可淘汰，然后返回 Page 指针。
    page.pin_count_++;
    replacer_->RecordAccess(frame_id, access_type);
    replacer_->SetEvictable(frame_id, false);
    return &page;
  }

  // TODO(P1): 4. 缓存未命中时调用 TakeFrame()；若没有可用 frame，则返回 nullptr。
  const frame_id_t frame_id = TakeFrame();
  if (frame_id == INVALID_PAGE_ID) {
    return nullptr;
  }

  Page &page = pages_[frame_id];

  // TODO(P1): 5. 调用 disk_manager_->ReadPage()，将指定磁盘页读入选中的 frame。
  disk_manager_->ReadPage(page_id, page.data_);

  // TODO(P1): 6. 设置新页面的 page id、pin_count_ = 1、is_dirty_ = false，并更新 page_table_。
  page.page_id_ = page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;
  page_table_[page_id] = frame_id;

  // TODO(P1): 7. 将 access_type 传给 RecordAccess()，再将 frame 标记为不可淘汰，最后返回 Page 指针。
  replacer_->RecordAccess(frame_id, access_type);
  replacer_->SetEvictable(frame_id, false);
  return &page;
}

auto BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty, [[maybe_unused]] AccessType access_type) -> bool {
  // TODO(P1): 1. 获取 latch_，防止并发修改目标页面的 pin count 和 dirty 状态。
  std::unique_lock<std::mutex> lock(latch_);
  // TODO(P1): 2. 在 page_table_ 中查找 page_id；页面不在缓冲池中时返回 false。
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }
  // TODO(P1): 3. 检查 pin_count_；如果已经为 0，则不能再次 Unpin，返回 false。
  Page &page = pages_[it->second];
  // TODO(P1): 4. 使用逻辑或合并 dirty 状态，避免传入 false 时清除页面已有的脏标记。
  // TODO(P1): 5. 将 pin_count_ 减一；只有当它降到 0 时，才把对应 frame 标记为可淘汰。
  if (page.pin_count_ <= 0) {
    return false;
  }
  page.is_dirty_ |= is_dirty;
  if (--page.pin_count_ == 0) {
    replacer_->SetEvictable(it->second, true);
  }
  // TODO(P1): 6. 如需支持 leaderboard，再根据要求处理 access_type；成功完成后返回 true。
  return true;
}

auto BufferPoolManager::FlushPage(page_id_t page_id) -> bool {
  // TODO(P1): 1. 检查 page_id 是否有效，并获取 latch_。
  if (page_id == INVALID_PAGE_ID) {
    return false;
  }
  std::unique_lock<std::mutex> lock(latch_);
  // TODO(P1): 2. 在 page_table_ 中查找页面；页面不在缓冲池中时返回 false。
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  // TODO(P1): 3. 无论页面是否为脏页，都调用 disk_manager_->WritePage() 将页面数据写回磁盘。
  disk_manager_->WritePage(page_id, pages_[it->second].data_);
  // TODO(P1): 4. 写回成功后清除 is_dirty_，最后返回 true；不要修改 pin_count_ 和 replacer 状态。
  pages_[it->second].is_dirty_ = false;
  return true;
}

void BufferPoolManager::FlushAllPages() {
  // TODO(P1): 1. 获取 latch_，保证遍历期间 page_table_ 和页面元数据保持稳定。
  std::unique_lock<std::mutex> lock(latch_);
  // TODO(P1): 2. 遍历 page_table_ 中所有驻留页面，而不是遍历无效的空闲 frame。
  // TODO(P1): 3. 对每个有效页面调用 disk_manager_->WritePage()，无论其 dirty 状态如何。
  // TODO(P1): 4. 每个页面写回成功后将 is_dirty_ 设为 false；不要在持锁时调用会重复获取 latch_ 的 FlushPage()。
  for (const auto &[page_id, frame_id] : page_table_) {
    Page &page = pages_[frame_id];
    disk_manager_->WritePage(page_id, page.data_);
    page.is_dirty_ = false;
  }
}

auto BufferPoolManager::DeletePage(page_id_t page_id) -> bool {
  // TODO(P1): 1. 获取 latch_，并在 page_table_ 中查找 page_id。
  std::unique_lock<std::mutex> lock(latch_);
  auto page_table_iter = page_table_.find(page_id);

  // TODO(P1): 2. 页面不在缓冲池中时，调用 DeallocatePage()，并将该操作视为成功。
  if (page_table_iter == page_table_.end()) {
    DeallocatePage(page_id);
    return true;
  }

  const frame_id_t frame_id = page_table_iter->second;
  Page &page = pages_[frame_id];

  // TODO(P1): 3. 页面在缓冲池中但 pin_count_ > 0 时不能删除，直接返回 false，且不修改任何状态。
  if (page.pin_count_ > 0) {
    return false;
  }

  // TODO(P1): 4. 对未被 pinned 的页面调用 replacer_->Remove()，停止跟踪对应 frame。
  replacer_->Remove(frame_id);

  // TODO(P1): 5. 删除 page_table_ 中的映射，并重置 Page 的数据、page id、pin count 和 dirty 状态。
  page_table_.erase(page_table_iter);
  page.ResetMemory();
  page.page_id_ = INVALID_PAGE_ID;
  page.pin_count_ = 0;
  page.is_dirty_ = false;

  // TODO(P1): 6. 将释放出的 frame 放回 free_list_，注意同一 frame 不能重复加入。
  free_list_.push_back(frame_id);

  // TODO(P1): 7. 调用 DeallocatePage() 释放磁盘页，最后返回 true。
  DeallocatePage(page_id);
  return true;
}

auto BufferPoolManager::AllocatePage() -> page_id_t { return next_page_id_++; }

auto BufferPoolManager::FetchPageBasic(page_id_t page_id) -> BasicPageGuard {
  // TODO(P1): 1. 调用 FetchPage() 获取并 pin 住页面。
  auto page = FetchPage(page_id);
  // TODO(P1): 2. 获取失败时返回默认构造的空 BasicPageGuard。
  if (page == nullptr) {
    return {};
  }
  // TODO(P1): 3. 获取成功时使用当前 BPM 和 Page 指针构造 BasicPageGuard，由 Guard 负责后续 Unpin。
  return {this, page};
}

auto BufferPoolManager::FetchPageRead(page_id_t page_id) -> ReadPageGuard {
  // TODO(P1): 1. 调用 FetchPage() 获取并 pin 住页面；失败时返回空 ReadPageGuard。
  auto page = FetchPage(page_id);
  if (page == nullptr) {
    return {};
  }
  // TODO(P1): 2. 成功后先对 Page 调用 RLatch() 获取读锁。
  page->RLatch();
  // TODO(P1): 3. 使用当前 BPM 和 Page 指针构造 ReadPageGuard；其 Drop() 应先解读锁，再 Unpin 页面。
  return {this, page};
}

auto BufferPoolManager::FetchPageWrite(page_id_t page_id) -> WritePageGuard {
  // TODO(P1): 1. 调用 FetchPage() 获取并 pin 住页面；失败时返回空 WritePageGuard。
  auto page = FetchPage(page_id);
  if (page == nullptr) {
    return {};
  }
  // TODO(P1): 2. 成功后先对 Page 调用 WLatch() 获取写锁。
  page->WLatch();
  // TODO(P1): 3. 使用当前 BPM 和 Page 指针构造 WritePageGuard；其 Drop() 应先解写锁，再 Unpin 页面。
  return {this, page};
}

auto BufferPoolManager::NewPageGuarded(page_id_t *page_id) -> BasicPageGuard {
  // TODO(P1): 1. 调用 NewPage() 创建并 pin 住新页面，同时让它填写 page_id。
  auto page = NewPage(page_id);
  if (page == nullptr) {
    return {};
  }
  // TODO(P1): 2. 创建失败时返回默认构造的空 BasicPageGuard。
  // TODO(P1): 3. 创建成功时使用当前 BPM 和 Page 指针构造 BasicPageGuard，由 Guard 管理 Unpin。
  return {this, page};
}

}  // namespace bustub
