//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.cpp
//
// Identification: src/buffer/lru_k_replacer.cpp
//
// Copyright (c) 2015-2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/lru_k_replacer.h"
#include <iterator>
#include <limits>
#include <mutex>
#include "common/exception.h"
#include "common/macros.h"

namespace bustub {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k) : replacer_size_(num_frames), k_(k) {}

auto LRUKReplacer::Evict(frame_id_t *frame_id) -> bool {
  BUSTUB_ASSERT(frame_id != nullptr, "frame_id must not be null");

  std::unique_lock<std::mutex> lock(latch_);

  if (curr_size_ == 0) {
    return false;
  }

  auto candidate = node_store_.end();
  bool candidate_is_infinite = false;
  size_t candidate_timestamp = std::numeric_limits<size_t>::max();

  for (auto it = node_store_.begin(); it != node_store_.end(); ++it) {
    auto &node = it->second;

    // 正在被使用、不可淘汰的 Frame 直接跳过
    if (!node.is_evictable_) {
      continue;
    }

    BUSTUB_ASSERT(!node.history_.empty(),
                  "An evictable frame must have access history");

    // 访问次数不足 K，backward K-distance 视为正无穷
    const bool current_is_infinite = node.history_.size() < k_;

    size_t current_timestamp;

    if (current_is_infinite) {
      // 多个正无穷候选者之间，使用传统 LRU：
      // 最早访问时间越小，越优先淘汰
      current_timestamp = node.history_.front();
    } else {
      // 找到倒数第 K 次访问时间
      auto kth_access = node.history_.end();
      std::advance(kth_access, -static_cast<std::ptrdiff_t>(k_));
      current_timestamp = *kth_access;
    }

    bool should_replace = false;

    if (candidate == node_store_.end()) {
      // 第一个可淘汰节点直接成为候选者
      should_replace = true;
    } else if (current_is_infinite != candidate_is_infinite) {
      // 访问不足 K 次的节点，优先于访问达到 K 次的节点
      should_replace = current_is_infinite;
    } else if (current_timestamp < candidate_timestamp) {
      // 同类别中，时间戳越早，淘汰优先级越高
      should_replace = true;
    }

    if (should_replace) {
      candidate = it;
      candidate_is_infinite = current_is_infinite;
      candidate_timestamp = current_timestamp;
    }
  }

  // curr_size_ 和实际状态不一致时，也安全返回 false
  if (candidate == node_store_.end()) {
    return false;
  }

  *frame_id = candidate->first;

  // 成功淘汰后删除访问历史
  node_store_.erase(candidate);
  --curr_size_;

  return true;
}

void LRUKReplacer::RecordAccess(frame_id_t frame_id, [[maybe_unused]] AccessType access_type) {
  std::unique_lock<std::mutex> lock(latch_);

  if (frame_id < 0 || static_cast<size_t>(frame_id) >= replacer_size_) {
    throw Exception("Invalid frame id");
  }

  auto &node = node_store_[frame_id];

  node.history_.push_back(current_timestamp_++);

  if (node.history_.size() > k_) {
    node.history_.pop_front();
  }
}

void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::unique_lock<std::mutex> lock(latch_);

  if (frame_id < 0 || static_cast<size_t>(frame_id) >= replacer_size_) {
    throw Exception("Invalid frame id");
  }

  auto it = node_store_.find(frame_id);

  // 没有 RecordAccess 过，直接返回
  if (it == node_store_.end()) {
    return;
  }

  auto &node = it->second;

  // 状态没有变化，不修改任何内容
  if (node.is_evictable_ == set_evictable) {
    return;
  }

  node.is_evictable_ = set_evictable;

  if (set_evictable) {
    ++curr_size_;
  } else {
    --curr_size_;
  }
}

void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::unique_lock<std::mutex> lock(latch_);
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= replacer_size_) {
    throw Exception("Invalid frame id");
  }

  auto it = node_store_.find(frame_id);

  if (it == node_store_.end()) {
    return;
  }

  if (!it->second.is_evictable_) {
    throw Exception("Cannot remove a non-evictable frame");
  }

  node_store_.erase(it);
  --curr_size_;
}

auto LRUKReplacer::Size() -> size_t {
  std::unique_lock<std::mutex> lock(latch_);
  return curr_size_;
}

}  // namespace bustub
