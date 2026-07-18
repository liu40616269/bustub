#include "storage/page/page_guard.h"
#include "buffer/buffer_pool_manager.h"

namespace bustub {

BasicPageGuard::BasicPageGuard(BasicPageGuard &&that) noexcept
    : bpm_(that.bpm_), page_(that.page_), is_dirty_(that.is_dirty_) {
  // TODO(P1): 1. 接管 that 的 bpm_、page_ 和 is_dirty_，让当前 Guard 管理同一个页面。
  that.bpm_ = nullptr;
  that.page_ = nullptr;
  that.is_dirty_ = false;
  // TODO(P1): 2. 将 that 的 bpm_、page_ 清空，并将 is_dirty_ 恢复为 false，防止两个 Guard 重复 Unpin。
}

void BasicPageGuard::Drop() {
  // TODO(P1): 1. 如果 page_ 为空，说明当前 Guard 不持有页面，直接返回以保证 Drop() 可以重复调用。
  if (page_ == nullptr) {
    return;
  }
  // TODO(P1): 2. 保存当前页面的 page id，并调用 bpm_->UnpinPage(page_id, is_dirty_) 释放这次 pin。
  page_id_t page_id = page_->GetPageId();
  bpm_->UnpinPage(page_id, is_dirty_);

  // TODO(P1): 3. 将 bpm_、page_ 清空，并将 is_dirty_ 恢复为 false，使当前 Guard 变为空 Guard。
  bpm_ = nullptr;
  page_ = nullptr;
  is_dirty_ = false;
}

auto BasicPageGuard::operator=(BasicPageGuard &&that) noexcept -> BasicPageGuard & {
  // TODO(P1): 1. 检查是否为自移动赋值；如果 this == &that，直接返回 *this。
  if (this == &that) {
    return *this;
  }
  // TODO(P1): 2. 调用 Drop() 释放当前 Guard 原先持有的页面，避免 pin 泄漏。
  Drop();
  // TODO(P1): 3. 接管 that 的 bpm_、page_ 和 is_dirty_。
  bpm_ = that.bpm_;
  page_ = that.page_;
  is_dirty_ = that.is_dirty_;
  // TODO(P1): 4. 清空 that 的全部状态，防止源 Guard 析构时再次 Unpin 同一页面。
  that.bpm_ = nullptr;
  that.page_ = nullptr;
  that.is_dirty_ = false;
  // TODO(P1): 5. 返回 *this。
  return *this;
}

BasicPageGuard::~BasicPageGuard() {  // NOLINT
  // TODO(P1): 1. 调用 Drop()，让 Guard 离开作用域时自动 Unpin 页面并清空状态。
  Drop();
}

ReadPageGuard::ReadPageGuard(ReadPageGuard &&that) noexcept
    : guard_(std::move(that.guard_)) {
  // TODO(P1): 1. 通过移动 that.guard_ 构造当前 guard_，把页面读锁和 pin 的管理权一并转移过来。
  // TODO(P1): 2. 不要重新加读锁；BasicPageGuard 的移动构造应保证 that.guard_ 变为空 Guard。
}

auto ReadPageGuard::operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard & {
  // TODO(P1): 1. 检查是否为自移动赋值；如果 this == &that，直接返回 *this。
  if (this == &that) {
    return *this;
  }
  // TODO(P1): 2. 调用当前 ReadPageGuard 的 Drop()，先释放原页面的读锁和 pin。
  Drop();
  // TODO(P1): 3. 将 that.guard_ 移动赋值给 guard_，接管源 Guard 对页面的管理权。
  this->guard_ = std::move(that.guard_);
  // TODO(P1): 4. 返回 *this；源 Guard 应已被 BasicPageGuard 的移动赋值清空。
  return *this;
}

void ReadPageGuard::Drop() {
  // TODO(P1): 1. 如果 guard_.page_ 为空，说明当前 Guard 不持有页面，直接返回。
  if (guard_.page_ == nullptr) {
    return;
  }
  // TODO(P1): 2. 调用 guard_.page_->RUnlatch() 释放读锁；必须在 Unpin 之前完成。
  guard_.page_->RUnlatch();
  // TODO(P1): 3. 调用 guard_.Drop()，由 BasicPageGuard 将页面 Unpin 并清空内部状态。
  guard_.Drop();
}

ReadPageGuard::~ReadPageGuard() {  // NOLINT
  // TODO(P1): 1. 调用 Drop()，自动释放读锁并 Unpin 页面。
  Drop();
}

WritePageGuard::WritePageGuard(WritePageGuard &&that) noexcept
    : guard_(std::move(that.guard_)) {
  // TODO(P1): 1. 通过移动 that.guard_ 构造当前 guard_，把页面写锁和 pin 的管理权一并转移过来。
  // TODO(P1): 2. 不要重新加写锁；BasicPageGuard 的移动构造应保证 that.guard_ 变为空 Guard。
}

auto WritePageGuard::operator=(WritePageGuard &&that) noexcept -> WritePageGuard & {
  // TODO(P1): 1. 检查是否为自移动赋值；如果 this == &that，直接返回 *this。
  if (this == &that) {
    return *this;
  }
  // TODO(P1): 2. 调用当前 WritePageGuard 的 Drop()，先释放原页面的写锁和 pin。
  Drop();
  // TODO(P1): 3. 将 that.guard_ 移动赋值给 guard_，接管源 Guard 对页面的管理权和 dirty 状态。
  guard_ = std::move(that.guard_);
  // TODO(P1): 4. 返回 *this；源 Guard 应已被 BasicPageGuard 的移动赋值清空。
  return *this;
}

void WritePageGuard::Drop() {
  // TODO(P1): 1. 如果 guard_.page_ 为空，说明当前 Guard 不持有页面，直接返回。
  if (guard_.page_ == nullptr) {
    return;
  }
  // TODO(P1): 2. 调用 guard_.page_->WUnlatch() 释放写锁；必须在 Unpin 之前完成。
  guard_.page_->WUnlatch();
  // TODO(P1): 3. 调用 guard_.Drop()，由 BasicPageGuard 传递 dirty 状态、Unpin 页面并清空内部状态。
  guard_.Drop();
}

WritePageGuard::~WritePageGuard() {  // NOLINT
  // TODO(P1): 1. 调用 Drop()，自动释放写锁并 Unpin 页面。
  Drop();
}

}  // namespace bustub
