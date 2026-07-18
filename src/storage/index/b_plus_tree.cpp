#include <sstream>
#include <string>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "storage/index/b_plus_tree.h"

namespace bustub {

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
                          const KeyComparator &comparator, int leaf_max_size, int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id) {
  WritePageGuard guard = bpm_->FetchPageWrite(header_page_id_);
  auto root_page = guard.AsMut<BPlusTreeHeaderPage>();
  root_page->root_page_id_ = INVALID_PAGE_ID;
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool {
  auto header_guard = bpm_->FetchPageRead(header_page_id_);
  const auto *header_page = header_guard.As<BPlusTreeHeaderPage>();
  return header_page->root_page_id_ == INVALID_PAGE_ID;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result, Transaction *txn) -> bool {
  // Declaration of context instance.
  (void)txn;
  BUSTUB_ASSERT(result != nullptr, "Result vector cannot be null");

  page_id_t root_page_id = INVALID_PAGE_ID;
  // 1. 从 HeaderPage 中读取根页面编号。
  auto head_guard = bpm_->FetchPageRead(header_page_id_);
  const auto *header_page = head_guard.As<BPlusTreeHeaderPage>();
  root_page_id = header_page->root_page_id_;

  // 2. 没有根节点，说明是空树。
  if (root_page_id == INVALID_PAGE_ID) {
    return false;
  }

  // 3.从根节点开始向下查找
  auto current_guard = bpm_->FetchPageRead(root_page_id);
  head_guard.Drop();
  while (!current_guard.As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal_page = current_guard.As<InternalPage>();

    // InternalPage::LookUp返回应该进入的child page_id
    const page_id_t child_page_id = internal_page->Lookup(key, comparator_);

    // 先读取并锁住child，再通过移动赋值释放原来的parent
    current_guard = bpm_->FetchPageRead(child_page_id);
  }
  const auto *leaf_page = current_guard.As<LeafPage>();
  ValueType value{};
  if (!leaf_page->Lookup(key, &value, comparator_)) {
    return false;
  }

  // 当前项目的 B+ 树只支持唯一键，因此最多找到一个 value。
  result->push_back(value);
  return true;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsSafeForInsert(const BPlusTreePage *page) const -> bool {
  BUSTUB_ASSERT(page != nullptr, "Page cannot be null");

  // 叶子页在插入后 size 达到 max_size 时立即分裂，所以必须保证 size + 1 < max_size。
  // 例如 max_size=4、当前 size=2 时，插入后为 3，不会分裂；当前 size=3 则不安全。
  if (page->IsLeafPage()) {
    return page->GetSize() + 1 < page->GetMaxSize();
  }

  // InternalPage 只有在“已经为 max_size，又收到一个新 child”时才分裂。
  // 因此当前 size < max_size 就能吸收一次下层分裂，不会再把分裂传播给自己的父节点。
  return page->GetSize() < page->GetMaxSize();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsSafeForDelete(const BPlusTreePage *page, bool is_root) const -> bool {
  BUSTUB_ASSERT(page != nullptr, "Page cannot be null");

  if (is_root) {
    // 根叶子删完最后一条记录会让整棵树变空，因此至少要有 2 条记录才安全。
    // 根 InternalPage 删除一个 child 后若只剩 1 个 child，就要让该 child 成为新根；
    // 所以根 InternalPage 当前至少要有 3 个 child 才安全。
    return page->GetSize() > (page->IsLeafPage() ? 1 : 2);
  }

  // 非根叶子删除一项后仍需满足 min_size，因此删除前必须严格大于 min_size。
  if (page->IsLeafPage()) {
    return page->GetSize() > page->GetMinSize();
  }

  // 本实现要求非根 InternalPage 至少保留两个 child；当 max_size / 2 更大时采用更大值。
  // 只有删除一个 child 后仍不低于这个下限，才可以确定祖先不会被修改。
  const int min_size = std::max(2, page->GetMinSize());
  return page->GetSize() > min_size;
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ReleaseAncestors(Context *ctx) {
  BUSTUB_ASSERT(ctx != nullptr, "Context cannot be null");

  // 调用者必须先取得 child 的写锁，再调用本函数。child guard 暂存在调用者的局部变量中，
  // 因此 ReleaseAll 只会释放 Header/root/.../parent，不会在交接过程中留下无锁窗口。
  // ReleaseAll 还会严格按从上到下的顺序释放，避免与其他同样向下取锁的线程形成环路等待。
  ctx->ReleaseAll();
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::TryDeletePage(page_id_t page_id) {
  // 调用到这里以前，父节点中的 child 指针已经删除，当前线程持有的目标页 guard 也已经释放。
  // 顺序执行时 DeletePage 会立即成功；并发执行时，另一个线程可能已经 pin 住该页并正在等待页面锁，
  // 此时 DeletePage 按接口约定返回 false。逻辑摘链已经完成，不能因为“暂时无法物理回收”终止进程，
  // 也不能原地自旋等待（对方可能还在等待本线程持有的其他页面）。它结束访问后，该 frame 会重新可淘汰。
  (void)bpm_->DeletePage(page_id);
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::InsertIntoParent(Context *ctx, page_id_t old_page_id, const KeyType &separator_key,
                                      page_id_t new_page_id) -> bool {
  BUSTUB_ASSERT(ctx != nullptr, "Context cannot be null");

  // 下面三个变量描述一个尚未插入父节点的分裂结果。
  page_id_t pending_old_page_id = old_page_id;
  KeyType pending_key = separator_key;
  page_id_t pending_new_page_id = new_page_id;

  while (true) {
    // 旧节点原本就是根：它没有父节点，需要创建一个新的 Internal Root。
    if (ctx->IsRootPage(pending_old_page_id)) {
      // 只有原根本身会分裂时才需要修改 HeaderPage。锁耦合的安全判断保证：
      // 若 Header guard 已提前释放，分裂传播一定会在某个安全祖先处停止，不可能走到这里。
      BUSTUB_ASSERT(ctx->header_page_.has_value(), "Splitting the root requires the header page write guard");

      page_id_t new_root_page_id = INVALID_PAGE_ID;
      auto new_root_guard = bpm_->NewPageGuarded(&new_root_page_id);
      if (new_root_page_id == INVALID_PAGE_ID) {
        return false;
      }

      auto *new_root = new_root_guard.AsMut<InternalPage>();
      new_root->Init(internal_max_size_);
      new_root->PopulateNewRoot(pending_old_page_id, pending_key, pending_new_page_id);

      auto *header_page = ctx->header_page_->AsMut<BPlusTreeHeaderPage>();
      header_page->root_page_id_ = new_root_page_id;
      ctx->root_page_id_ = new_root_page_id;
      return true;
    }

    // 非根节点必然还有父节点留在 write_set_ 的末尾。
    BUSTUB_ASSERT(!ctx->write_set_.empty(), "A non-root split must have a parent guard");
    auto &parent_guard = ctx->write_set_.back();
    const page_id_t parent_page_id = parent_guard.PageId();
    auto *parent_page = parent_guard.AsMut<InternalPage>();

    // 父节点还有位置时，直接插入分隔 key 和新右节点，传播结束。
    if (parent_page->GetSize() < parent_page->GetMaxSize()) {
      parent_page->InsertNodeAfter(pending_old_page_id, pending_key, pending_new_page_id);
      return true;
    }

    // 父节点已满：创建新的右 InternalPage，把 pending child 与原有 child 一起均分到左右页。
    page_id_t new_internal_page_id = INVALID_PAGE_ID;
    auto new_internal_guard = bpm_->NewPageGuarded(&new_internal_page_id);
    if (new_internal_page_id == INVALID_PAGE_ID) {
      return false;
    }

    auto *new_internal_page = new_internal_guard.AsMut<InternalPage>();
    new_internal_page->Init(internal_max_size_);
    const KeyType middle_key =
        parent_page->InsertAndSplit(pending_old_page_id, pending_key, pending_new_page_id, new_internal_page);

    // 当前 parent 自己已经分裂，将它的分裂结果作为下一轮待插入项继续向祖先传播。
    pending_old_page_id = parent_page_id;
    pending_key = middle_key;
    pending_new_page_id = new_internal_page_id;
    ctx->write_set_.pop_back();
  }
}

/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value, Transaction *txn) -> bool {
  (void)txn;
  Context ctx;

  // 1. HeaderPage 是整棵树的入口。先取得它的写锁，保证“读取 root_page_id -> 锁住根页面”
  // 这段过程不会与换根并发发生。后面一旦确认某个页面安全，就会尽早释放这把锁。
  ctx.header_page_ = bpm_->FetchPageWrite(header_page_id_);
  auto *header_page = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();
  ctx.root_page_id_ = header_page->root_page_id_;

  // 2. 空树没有根页面。此时创建的页面尚未写入 HeaderPage，其他线程看不到它，
  // 所以 NewPageGuarded 提供的 BasicPageGuard 已足以保护初始化过程。
  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    page_id_t root_page_id = INVALID_PAGE_ID;
    auto root_guard = bpm_->NewPageGuarded(&root_page_id);
    if (root_page_id == INVALID_PAGE_ID) {
      return false;
    }

    // 3. 第一页既是根节点也是叶子节点，直接保存第一组 key/value。
    auto *root_leaf = root_guard.AsMut<LeafPage>();
    root_leaf->Init(leaf_max_size_);
    const bool inserted = root_leaf->Insert(key, value, comparator_);
    BUSTUB_ASSERT(inserted, "The first key should be inserted successfully");

    // 4. 页面初始化完成后再发布 root_page_id。其他线程拿到 Header 锁后只会看到完整的新根。
    header_page->root_page_id_ = root_page_id;
    ctx.root_page_id_ = root_page_id;
    return true;
  }

  // 5. 在仍持有 Header 写锁时先锁住根页面。这是第一次 latch coupling：
  // 必须先取得下一层的锁，才能释放上一层，否则 root_page_id 可能在中间被替换。
  auto root_guard = bpm_->FetchPageWrite(ctx.root_page_id_);
  const auto *root_page = root_guard.As<BPlusTreePage>();
  if (IsSafeForInsert(root_page)) {
    // 根能吸收本次修改而不分裂，root_page_id 不会变化，因此 HeaderPage 已经可以释放。
    ReleaseAncestors(&ctx);
  }
  ctx.write_set_.push_back(std::move(root_guard));

  // 6. 沿 InternalPage 向下查找。write_set_ 只保存“本次操作仍可能修改到”的路径后缀，
  // 而不是无条件保存从根到叶子的所有页面，这样不同子树中的线程才能真正并行。
  while (!ctx.write_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal_page = ctx.write_set_.back().As<InternalPage>();
    const page_id_t child_page_id = internal_page->Lookup(key, comparator_);

    // 7. 先锁 child，再判断并释放 parent/祖先。顺序不能反过来：如果先放开 parent，
    // 另一个线程可能分裂或合并它，刚才得到的 child_page_id 就不再代表稳定的下降路径。
    auto child_guard = bpm_->FetchPageWrite(child_page_id);
    const auto *child_page = child_guard.As<BPlusTreePage>();

    if (IsSafeForInsert(child_page)) {
      // child 安全意味着：即使目标叶子最终分裂，传播也一定在 child 内停止。
      // child_guard 仍在局部变量中持有 child 写锁，所以此时释放所有祖先没有无锁窗口。
      ReleaseAncestors(&ctx);
    }
    ctx.write_set_.push_back(std::move(child_guard));
  }

  // 8. 循环结束后，队尾就是目标叶子；队尾之前只保留可能被分裂传播修改的祖先。
  auto *leaf_page = ctx.write_set_.back().AsMut<LeafPage>();

  // 9. 本项目实现唯一键索引。重复 key 不修改页面，所有 guard 会由 RAII 自动释放。
  ValueType existing_value{};
  if (leaf_page->Lookup(key, &existing_value, comparator_)) {
    return false;
  }
  BUSTUB_ASSERT(leaf_page->GetSize() < leaf_page->GetMaxSize(),
                "A leaf page should have been split before becoming full");

  // 10. 插入后仍小于 max_size 时不会分裂；前面的安全判断通常已释放其全部祖先。
  if (leaf_page->GetSize() + 1 < leaf_page->GetMaxSize()) {
    const bool inserted = leaf_page->Insert(key, value, comparator_);
    BUSTUB_ASSERT(inserted, "A non-duplicate key should be inserted successfully");
    return true;
  }

  // 11. 插入后会达到 max_size，必须分裂。因为该叶子“不安全”，它到传播终点之间的
  // 父节点 guard 都还在 write_set_ 中；若它就是根，Header guard 也一定仍然存在。
  const page_id_t old_leaf_page_id = ctx.write_set_.back().PageId();
  page_id_t new_leaf_page_id = INVALID_PAGE_ID;
  auto new_leaf_guard = bpm_->NewPageGuarded(&new_leaf_page_id);
  if (new_leaf_page_id == INVALID_PAGE_ID) {
    return false;
  }

  // 12. 新页还没有被父节点或叶子链发布，其他线程无法找到它，可以安全地完成初始化。
  auto *new_leaf = new_leaf_guard.AsMut<LeafPage>();
  new_leaf->Init(leaf_max_size_);
  const bool inserted = leaf_page->Insert(key, value, comparator_);
  BUSTUB_ASSERT(inserted, "A non-duplicate key should be inserted successfully");

  // 13. 新右叶子接管旧叶子的后半部分，并插入旧叶子与原 next 之间。
  new_leaf->SetNextPageId(leaf_page->GetNextPageId());
  leaf_page->SetNextPageId(new_leaf_page_id);
  leaf_page->MoveHalfTo(new_leaf);

  // 14. 新右叶子的最小 key 是父节点的新分隔键。先释放旧叶子，让 write_set_ 队尾变成父节点，
  // 再由 InsertIntoParent 逐层插入；若父节点也满，则继续向上分裂。
  const KeyType separator_key = new_leaf->KeyAt(0);
  ctx.write_set_.pop_back();
  return InsertIntoParent(&ctx, old_leaf_page_id, separator_key, new_leaf_page_id);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RebalanceInternalAfterDeletion(Context *ctx) {
  BUSTUB_ASSERT(ctx != nullptr, "Context cannot be null");

  while (!ctx->write_set_.empty()) {
    // 调用约定：write_set_ 末尾是刚刚失去一个 child、可能欠载的 InternalPage。
    auto &current_guard = ctx->write_set_.back();
    const page_id_t current_page_id = current_guard.PageId();
    auto *current_page = current_guard.AsMut<InternalPage>();

    // 根 InternalPage 不受普通 min_size 约束；只剩一个 child 时直接让该 child 成为新根。
    if (ctx->IsRootPage(current_page_id)) {
      if (current_page->GetSize() > 1) {
        // 根仍有至少两个 child，树高不变，不需要访问 HeaderPage。
        // 因此即使先前因根页面安全而释放了 Header guard，这条路径也是合法的。
        return;
      }

      // 只有真正收缩根时才必须持有 Header 写锁。锁耦合保证：可能收缩的根会被判为不安全，
      // 因而 Header guard 不会被提前释放。
      BUSTUB_ASSERT(ctx->header_page_.has_value(), "Shrinking the root requires the header page write guard");
      BUSTUB_ASSERT(current_page->GetSize() == 1, "An internal root should retain exactly one child before shrinking");
      const page_id_t new_root_page_id = current_page->ValueAt(0);
      auto *header_page = ctx->header_page_->AsMut<BPlusTreeHeaderPage>();
      header_page->root_page_id_ = new_root_page_id;
      ctx->root_page_id_ = new_root_page_id;

      // 旧根 guard 必须先释放，BufferPoolManager 才能真正回收该页面。
      ctx->write_set_.pop_back();
      TryDeletePage(current_page_id);
      return;
    }

    // InternalPage 至少需要两个 child 才能在下一层欠载时找到兄弟；max/2 更大时仍按其限制。
    const int current_min_size = std::max(2, current_page->GetMinSize());
    if (current_page->GetSize() >= current_min_size) {
      return;
    }

    BUSTUB_ASSERT(ctx->write_set_.size() >= 2, "A non-root internal page must have a parent");
    auto &parent_guard = ctx->write_set_[ctx->write_set_.size() - 2];
    auto *parent_page = parent_guard.AsMut<InternalPage>();
    const int child_index = parent_page->ValueIndex(current_page_id);
    BUSTUB_ASSERT(child_index != -1, "The underfull internal page must be referenced by its parent");

    // 先尝试向左兄弟借最后一个 child。父分隔 key 下沉，左兄弟最后一个 key 上升替换它。
    if (child_index > 0) {
      const page_id_t left_page_id = parent_page->ValueAt(child_index - 1);
      auto left_guard = bpm_->FetchPageWrite(left_page_id);
      auto *left_page = left_guard.AsMut<InternalPage>();

      const int left_min_size = std::max(2, left_page->GetMinSize());
      if (left_page->GetSize() > left_min_size) {
        const KeyType middle_key = parent_page->KeyAt(child_index);
        const KeyType new_parent_key = left_page->MoveLastToFrontOf(current_page, middle_key);
        parent_page->SetKeyAt(child_index, new_parent_key);
        return;
      }
    }

    // 左兄弟不能借时尝试右兄弟：父分隔 key 下沉，右兄弟 key[1] 上升为新分隔 key。
    if (child_index + 1 < parent_page->GetSize()) {
      const page_id_t right_page_id = parent_page->ValueAt(child_index + 1);
      auto right_guard = bpm_->FetchPageWrite(right_page_id);
      auto *right_page = right_guard.AsMut<InternalPage>();

      const int right_min_size = std::max(2, right_page->GetMinSize());
      if (right_page->GetSize() > right_min_size) {
        const KeyType middle_key = parent_page->KeyAt(child_index + 1);
        const KeyType new_parent_key = right_page->MoveFirstToEndOf(current_page, middle_key);
        parent_page->SetKeyAt(child_index + 1, new_parent_key);
        return;
      }
    }

    // 两侧都不能借，只能合并。仍然统一采用“右页并入左页”，随后让 parent 成为下一轮处理对象。
    if (child_index > 0) {
      const page_id_t left_page_id = parent_page->ValueAt(child_index - 1);
      auto left_guard = bpm_->FetchPageWrite(left_page_id);
      auto *left_page = left_guard.AsMut<InternalPage>();
      const KeyType middle_key = parent_page->KeyAt(child_index);

      current_page->MoveAllTo(left_page, middle_key);
      parent_page->Remove(child_index);

      ctx->write_set_.pop_back();
      TryDeletePage(current_page_id);
    } else {
      BUSTUB_ASSERT(parent_page->GetSize() > 1, "An underfull internal page must have a sibling");
      const page_id_t right_page_id = parent_page->ValueAt(1);
      auto right_guard = bpm_->FetchPageWrite(right_page_id);
      auto *right_page = right_guard.AsMut<InternalPage>();
      const KeyType middle_key = parent_page->KeyAt(1);

      right_page->MoveAllTo(current_page, middle_key);
      parent_page->Remove(1);

      right_guard.Drop();
      TryDeletePage(right_page_id);

      // current_page 被保留，但下一轮需要处理 parent，所以释放当前页并让 parent 位于队尾。
      ctx->write_set_.pop_back();
    }
  }
}

/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *txn) {
  (void)txn;
  Context ctx;

  // 1. 与插入相同，先锁 HeaderPage，再读取并锁住根页面。删除可能让根只剩一个 child，
  // 此时必须修改 root_page_id_，所以在确认后续页面安全以前不能直接放开 Header。
  ctx.header_page_ = bpm_->FetchPageWrite(header_page_id_);
  ctx.root_page_id_ = ctx.header_page_->As<BPlusTreeHeaderPage>()->root_page_id_;

  // 2. 空树没有任何节点需要访问。
  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    return;
  }

  // 3. 仍持有 Header 写锁时取得根写锁，保证换根操作不可能夹在二者之间。
  auto root_guard = bpm_->FetchPageWrite(ctx.root_page_id_);
  const auto *root_page = root_guard.As<BPlusTreePage>();
  if (IsSafeForDelete(root_page, true)) {
    // 根安全：删除一项后不会清空根叶子，也不会把根 InternalPage 收缩为它唯一的 child。
    ReleaseAncestors(&ctx);
  }
  ctx.write_set_.push_back(std::move(root_guard));

  // 4. 向目标叶子下降。每次都先锁 child，再判断 child 删除一项后是否仍满足容量下限。
  // 若安全，就释放 Header 和所有旧祖先；若不安全，则保留父路径供后面的借位/合并使用。
  while (!ctx.write_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal_page = ctx.write_set_.back().As<InternalPage>();
    const page_id_t child_page_id = internal_page->Lookup(key, comparator_);

    auto child_guard = bpm_->FetchPageWrite(child_page_id);
    const auto *child_page = child_guard.As<BPlusTreePage>();
    if (IsSafeForDelete(child_page, ctx.IsRootPage(child_page_id))) {
      // child_guard 仍持有 child 写锁，释放旧路径后再把它移入 write_set_，完成无缝锁交接。
      ReleaseAncestors(&ctx);
    }
    ctx.write_set_.push_back(std::move(child_guard));
  }

  // 5. write_set_ 队尾是目标叶子；它前面的元素只包含删除可能向上影响到的路径后缀。
  auto &leaf_guard = ctx.write_set_.back();
  const page_id_t leaf_page_id = leaf_guard.PageId();
  auto *leaf_page = leaf_guard.AsMut<LeafPage>();

  // 6. key 不存在时不修改树。RAII 会自动释放当前保留的全部 guard。
  ValueType existing_value{};
  if (!leaf_page->Lookup(key, &existing_value, comparator_)) {
    return;
  }

  const bool is_root_leaf = ctx.IsRootPage(leaf_page_id);
  const bool removed = leaf_page->Remove(key, comparator_);
  BUSTUB_ASSERT(removed, "An existing key should be removed successfully");

  // 7. 根节点同时也是叶子时不受普通 min_size 限制；还有记录就不需要改变树结构。
  if (is_root_leaf && leaf_page->GetSize() > 0) {
    return;
  }

  if (is_root_leaf) {
    // 删除根叶子的最后一项会让树变空。该根在下降时一定被判为不安全，Header guard 必然仍在。
    BUSTUB_ASSERT(ctx.header_page_.has_value(), "Deleting the last root item requires the header page write guard");
    auto *header_page = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();
    header_page->root_page_id_ = INVALID_PAGE_ID;
    ctx.root_page_id_ = INVALID_PAGE_ID;

    // DeletePage 只能删除 pin_count 为 0 的页面，所以必须先销毁根页面的 guard。
    ctx.write_set_.pop_back();
    TryDeletePage(leaf_page_id);
    return;
  }

  // 8. 非根叶子删除后仍满足最小容量，不会影响父节点，操作到此结束。
  if (leaf_page->GetSize() >= leaf_page->GetMinSize()) {
    return;
  }

  // 9. 叶子欠载时，它在下降阶段必定被判为不安全，因此父页面仍位于 write_set_ 的前一项。
  BUSTUB_ASSERT(ctx.write_set_.size() >= 2, "A non-root leaf must have a parent page");
  auto &parent_guard = ctx.write_set_[ctx.write_set_.size() - 2];
  const page_id_t parent_page_id = parent_guard.PageId();
  auto *parent_page = parent_guard.AsMut<InternalPage>();
  const int child_index = parent_page->ValueIndex(leaf_page_id);
  BUSTUB_ASSERT(child_index != -1, "The target leaf must be referenced by its parent");

  // 10. 优先向左兄弟借最后一项。我们仍持有 parent 写锁，同一父节点下的其他删除线程
  // 无法同时取得另一个 child 后反向等待本页，因此这里按“parent -> sibling”的顺序加锁不会死锁。
  if (child_index > 0) {
    const page_id_t left_page_id = parent_page->ValueAt(child_index - 1);
    auto left_guard = bpm_->FetchPageWrite(left_page_id);
    auto *left_page = left_guard.AsMut<LeafPage>();

    if (left_page->GetSize() > left_page->GetMinSize()) {
      left_page->MoveLastToFrontOf(leaf_page);
      parent_page->SetKeyAt(child_index, leaf_page->KeyAt(0));
      return;
    }
  }

  // 11. 左兄弟不能借时尝试右兄弟。借走首项后，父节点中右兄弟对应的分隔 key 必须更新。
  if (child_index + 1 < parent_page->GetSize()) {
    const page_id_t right_page_id = parent_page->ValueAt(child_index + 1);
    auto right_guard = bpm_->FetchPageWrite(right_page_id);
    auto *right_page = right_guard.AsMut<LeafPage>();

    if (right_page->GetSize() > right_page->GetMinSize()) {
      right_page->MoveFirstToEndOf(leaf_page);
      parent_page->SetKeyAt(child_index + 1, right_page->KeyAt(0));
      return;
    }
  }

  // 12. 两侧都不能借时执行合并。统一让右页并入左页，再从父节点删除右页对应的项。
  if (child_index > 0) {
    const page_id_t left_page_id = parent_page->ValueAt(child_index - 1);
    auto left_guard = bpm_->FetchPageWrite(left_page_id);
    auto *left_page = left_guard.AsMut<LeafPage>();

    // 当前叶子是右页：把它合并进左兄弟，并删除 parent[child_index]。
    leaf_page->MoveAllTo(left_page);
    parent_page->Remove(child_index);

    // 当前叶子 guard 位于 write_set_ 末尾，必须先释放再回收页面。
    ctx.write_set_.pop_back();
    TryDeletePage(leaf_page_id);
  } else {
    // 当前叶子是最左 child，只能让右兄弟合并进当前页。
    BUSTUB_ASSERT(parent_page->GetSize() > 1, "An underfull leaf must have a sibling before internal rebalancing");
    const page_id_t right_page_id = parent_page->ValueAt(1);
    auto right_guard = bpm_->FetchPageWrite(right_page_id);
    auto *right_page = right_guard.AsMut<LeafPage>();

    right_page->MoveAllTo(leaf_page);
    parent_page->Remove(1);

    // right_guard 仍 pin 住待删除的右页；先显式释放，再调用 DeletePage。
    right_guard.Drop();
    TryDeletePage(right_page_id);

    // 当前叶子不需要删除，但本函数后面只会处理父节点，先释放它的 guard。
    ctx.write_set_.pop_back();
  }

  // 13. 叶子合并使父 InternalPage 少了一个 child；继续向上处理欠载。
  // 如果下降途中遇到过安全父节点，传播会在它那里停止；否则可能一直到根并更新 HeaderPage。
  BUSTUB_ASSERT(ctx.write_set_.back().PageId() == parent_page_id, "The leaf parent should be last in the write path");
  RebalanceInternalAfterDeletion(&ctx);
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE {
  // 思路：InternalPage 只负责导航。找到最左叶子的第一项后，后续扫描完全依靠叶子 next_page_id 链。
  // 因此 Begin 的定位成本是树高 O(log N)，之后 Iterator 扫描 K 条记录的成本是 O(K)。

  // 1. HeaderPage 是树的稳定入口，其中 root_page_id_ 才是真正根节点。空树没有起始记录，返回 End。
  auto header_guard = bpm_->FetchPageRead(header_page_id_);
  const auto *header_page = header_guard.As<BPlusTreeHeaderPage>();
  const page_id_t root_page_id = header_page->root_page_id_;
  if (root_page_id == INVALID_PAGE_ID) {
    return End();
  }

  // 2. 先锁住根再释放 Header Guard，避免读取根编号后、取得根页面前根发生变化。
  auto current_guard = bpm_->FetchPageRead(root_page_id);
  header_guard.Drop();

  // 3. InternalPage::array_[0].second 是最左 child。始终走 ValueAt(0)，最终得到全树最小 key 所在叶子。
  // 赋值右侧会先 Fetch 并锁住 child，随后 ReadPageGuard 的移动赋值才释放 parent，这就是读锁的逐层交接。
  while (!current_guard.As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal_page = current_guard.As<InternalPage>();
    current_guard = bpm_->FetchPageRead(internal_page->ValueAt(0));
  }

  // 4. 正常 B+ 树不会保留空的非根叶子；这里仍沿链表跳过空页，增强边界健壮性。
  const auto *leaf_page = current_guard.As<LeafPage>();
  while (leaf_page->GetSize() == 0) {
    const page_id_t next_page_id = leaf_page->GetNextPageId();
    if (next_page_id == INVALID_PAGE_ID) {
      return End();
    }
    current_guard = bpm_->FetchPageRead(next_page_id);
    leaf_page = current_guard.As<LeafPage>();
  }

  // 5. 把最左叶子的 Guard 移交给 Iterator。Begin 返回后，叶子仍保持 pinned 和读锁状态。
  return INDEXITERATOR_TYPE(bpm_, std::move(current_guard), 0);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> INDEXITERATOR_TYPE {
  // 该接口实现 lower_bound 语义：不要求 key 存在，而是返回全树第一个 >= key 的记录。

  // 1. 与 Begin() 相同，先从 HeaderPage 取得根；空树不存在 lower_bound。
  auto header_guard = bpm_->FetchPageRead(header_page_id_);
  const auto *header_page = header_guard.As<BPlusTreeHeaderPage>();
  const page_id_t root_page_id = header_page->root_page_id_;
  if (root_page_id == INVALID_PAGE_ID) {
    return End();
  }

  // 2. 使用与 GetValue 相同的 InternalPage::Lookup 导航路径，找到 key 按范围应落入的叶子。
  auto current_guard = bpm_->FetchPageRead(root_page_id);
  header_guard.Drop();
  while (!current_guard.As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal_page = current_guard.As<InternalPage>();
    current_guard = bpm_->FetchPageRead(internal_page->Lookup(key, comparator_));
  }

  // 3. 叶子内部已经有序，KeyIndex 用二分查找返回本页第一个 >= key 的位置。
  const auto *leaf_page = current_guard.As<LeafPage>();
  int index = leaf_page->KeyIndex(key, comparator_);

  // 4. index == size 表示本页所有 key 都小于目标。下一叶子的第一项才可能是全局 lower_bound。
  // 如果已经没有下一页，则目标 key 大于树中全部 key，结果就是 End。
  while (index == leaf_page->GetSize()) {
    const page_id_t next_page_id = leaf_page->GetNextPageId();
    if (next_page_id == INVALID_PAGE_ID) {
      return End();
    }
    current_guard = bpm_->FetchPageRead(next_page_id);
    leaf_page = current_guard.As<LeafPage>();
    index = 0;
  }

  // 5. 将目标叶子 Guard 和页内 lower-bound 下标一起交给 Iterator。
  return INDEXITERATOR_TYPE(bpm_, std::move(current_guard), index);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE {
  // End 不对应真实 Page，不需要 BPM、PageGuard 或特殊哨兵页；默认 Iterator 的 guard_ 为空即可。
  return INDEXITERATOR_TYPE();
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t {
  auto header_guard = bpm_->FetchPageRead(header_page_id_);
  const auto *header_page = header_guard.As<BPlusTreeHeaderPage>();
  return header_page->root_page_id_;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string &file_name, Transaction *txn) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string &file_name, Transaction *txn) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager *bpm) {
  auto root_page_id = GetRootPageId();
  auto guard = bpm->FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage *page) {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<const LeafPage *>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf->GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i);
      if ((i + 1) < leaf->GetSize()) {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;

  } else {
    auto *internal = reinterpret_cast<const InternalPage *>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i);
      if ((i + 1) < internal->GetSize()) {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      auto guard = bpm_->FetchPageBasic(internal->ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager *bpm, const std::string &outf) {
  if (IsEmpty()) {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm->FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage *page, std::ofstream &out) {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<const LeafPage *>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << page_id << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">" << "max_size=" << leaf->GetMaxSize()
        << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize() << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      out << "<TD>" << leaf->KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << page_id << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }
  } else {
    auto *inner = reinterpret_cast<const InternalPage *>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << page_id << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">" << "max_size=" << inner->GetMaxSize()
        << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize() << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        out << inner->KeyAt(i);
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_guard = bpm_->FetchPageBasic(inner->ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0) {
        auto sibling_guard = bpm_->FetchPageBasic(inner->ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId() << " " << internal_prefix
              << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId() << " -> ";
      if (child_page->IsLeafPage()) {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      } else {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree() -> std::string {
  if (IsEmpty()) {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id) -> PrintableBPlusTree {
  auto root_page_guard = bpm_->FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page->IsLeafPage()) {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page->ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page->ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page->GetSize(); i++) {
    page_id_t child_id = internal_page->ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
