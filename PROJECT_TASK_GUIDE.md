# CMU 15-445 Spring 2023 Project 1–4 实现指南

> 本文提供修改思路与算法级伪代码，不是可直接提交的实现。接口与约束以当前仓库和官方 Spring 2023 说明为准；请保持仓库私有并独立完成代码。

范围以 [Spring 2023 Schedule](https://15445.courses.cs.cmu.edu/spring2023/schedule.html) 列出的四个正式项目为准。官方说明：[Project 1](https://15445.courses.cs.cmu.edu/spring2023/project1/) · [Project 2](https://15445.courses.cs.cmu.edu/spring2023/project2/) · [Project 3](https://15445.courses.cs.cmu.edu/spring2023/project3/) · [Project 4](https://15445.courses.cs.cmu.edu/spring2023/project4/)

## 通用开发顺序

先读头文件注释、测试和调用方，再设计不变量；每完成一个小接口就运行对应测试。Debug 构建启用 ASAN：

```sh
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . -j$(nproc)
make format check-lint
```

## Project 1：Buffer Pool

### Task 1：LRU-K Replacer

修改 `lru_k_replacer.cpp/.h`。为每个 frame 保存最近 K 次时间戳和 `evictable` 状态；`Size` 只统计可淘汰 frame。少于 K 次访问的 backward K-distance 为无穷大，并按最早一次访问打破平局。

```text
RECORD(fid): 校验 fid；history 追加 timestamp++；超过 K 则弹出最旧项
EVICT: 扫描 evictable 节点；先选 history<K 的最老节点，否则选第 K 次历史最老节点；删除并减 Size
SET_EVICTABLE: 仅状态变化时调整 Size
REMOVE: 仅允许删除 evictable 节点
```

所有公开方法都要保护共享状态；异常路径不能让计数与容器失配。

#### LRU-K 的状态设计

- `current_timestamp_`：逻辑时钟，每次 `RecordAccess` 使用当前值后递增。
- `history`：每个 frame 最近 K 次访问，队首最旧、队尾最新。
- `is_evictable`：是否允许 BPM 回收；它与是否存在访问历史是两个概念。
- `curr_size_`：可淘汰节点数，不是 `node_store_` 的大小。
- `replacer_size_`：合法 frame id 范围为 `[0, replacer_size_)`。

`Evict` 的比较可以统一成二元优先级：访问不足 K 次的节点优先；同为不足 K 次时比较第一次访问时间，同为至少 K 次时比较保留历史的队首。时间戳越小，backward K-distance 越大。

```text
best = NONE
for each (fid, node):
  if not node.evictable: continue
  category = (node.history.size < K ? INFINITE : FINITE)
  age_key = node.history.front
  if best is NONE or category 更优 or (category 相同 and age_key < best.age_key):
    best = fid
if best is NONE: return false
erase best; curr_size--; output best; return true
```

边界测试应覆盖非法负数/越界 frame id、重复 `SetEvictable`、删除不存在节点、删除 non-evictable 节点、淘汰后重新访问同一 frame，以及多线程同时访问。


#### `LRUKReplacer` 全部公开函数

```text
CONSTRUCTOR(num_frames, k):
  replacer_size = num_frames
  k_ = k
  current_timestamp = 0
  curr_size = 0
  node_store = empty

RECORD_ACCESS(fid, access_type):
  lock replacer.latch
  if fid < 0 or fid >= replacer_size: throw/abort
  if fid not in node_store:
    node_store[fid] = Node(history=empty, evictable=false)
  node = node_store[fid]
  node.history.push_back(current_timestamp)
  current_timestamp++
  while node.history.size > k: node.history.pop_front()

SET_EVICTABLE(fid, desired):
  lock replacer.latch
  validate fid
  if fid not recorded: return
  node = node_store[fid]
  if node.evictable == desired: return
  node.evictable = desired
  curr_size += (desired ? +1 : -1)

EVICT(out_fid):
  validate out pointer
  lock replacer.latch
  candidate = NONE
  for every node:
    if not evictable: continue
    infinite = history.size < k
    kth_timestamp = history.front  // history 最多保留 K 项
    if candidate NONE
       or (infinite and candidate finite)
       or (same category and kth_timestamp < candidate.timestamp):
         candidate = this node
  if candidate NONE: return false
  *out_fid = candidate.fid
  erase candidate history/node
  curr_size--
  return true

REMOVE(fid):
  lock; validate fid
  if fid absent: return
  if not node.evictable: throw/abort
  erase node; curr_size--

SIZE():
  lock
  return curr_size
```

`RecordAccess` 不调整 `curr_size`；`Evict` 失败不修改输出参数和任何容器；所有计数变化必须和状态变化发生在同一临界区。


### Task 2：Buffer Pool Manager

修改 `buffer_pool_manager.cpp`。维护 `page_table_`、`free_list_`、LRU-K 和 `Page` 元数据的一致性。获取 frame 时先用 free list，再淘汰；替换 dirty 页面前必须写盘。

```text
ACQUIRE_FRAME:
  若 free_list 非空则 pop；否则 replacer.Evict；若失败返回 NONE
  若旧页有效且 dirty → WritePage；删除旧 page_table 映射

FETCH(pid):
  命中 → pin++，记录访问，设为不可淘汰
  未命中 → 获取 frame；ReadPage；设置 pid/pin=1/dirty=false；登记映射与访问

NEW_PAGE: 获取 frame → AllocatePage → ResetMemory → 初始化元数据 → 登记映射
UNPIN: 校验存在且 pin>0；dirty |= 参数；pin--；降到 0 时设为可淘汰
FLUSH: 无论 dirty 与否都 WritePage，然后 dirty=false
DELETE: 不存在则成功；pin>0 则失败；否则移除映射/replacer，重置 frame 并放回 free list
```

避免同一线程持有 `latch_` 时再次调用会获取该锁的公开方法。

#### BPM 的 frame 获取辅助流程

建议把“获取一个可用 frame”抽象成私有 helper，避免 `NewPage` 与 `FetchPage` 复制复杂逻辑。helper 返回 frame id，并负责处理旧页面，但不要在 helper 内分配新 page id 或读入目标页。

```text
TAKE_FRAME():
  if free_list not empty: fid = pop_front
  else if not replacer.Evict(&fid): return NONE
  old = pages[fid]
  if old.page_id != INVALID:
    if old.dirty: disk.WritePage(old.page_id, old.data)
    page_table.erase(old.page_id)
  return fid
```

构造函数必须先移除 starter code 的 `NotImplementedException`，再分配连续 `Page[]`、创建 `LRUKReplacer`，并按 frame id 顺序填满 free list。析构是否 flush 以接口约定为准；本仓库析构函数只负责释放 `pages_`。

`NewPage(page_id*)`：

1. 检查输出指针并获取 BPM latch。
2. 调用 `TAKE_FRAME`；失败时不要分配 page id，也不要修改输出值。
3. `AllocatePage`，清空整页数据。
4. 设置 `page_id_`、`pin_count_=1`、`is_dirty_=false`。
5. 写入 `page_table_`，记录 LRU-K 访问并设置 non-evictable。

`FetchPage(page_id, access_type)`：

1. 命中 `page_table_`：找到 frame，递增 pin，调用 `RecordAccess(fid, access_type)`，设置 non-evictable。
2. 未命中：取 frame；失败返回 `nullptr`。
3. 重置 frame 后调用 `ReadPage(page_id, data)`。
4. 初始化元数据、建立映射、记录访问并 pin。

`UnpinPage` 中最容易写错的是 dirty 位和计数：只有 pin 原值大于 0 才能递减；dirty 应做逻辑或；只有从 1 降到 0 的那次调用才把 frame 设为 evictable。

```text
if pid not in page_table: return false
page = pages[page_table[pid]]
if page.pin_count == 0: return false
page.dirty = page.dirty OR is_dirty
page.pin_count--
if page.pin_count == 0: replacer.SetEvictable(fid, true)
return true
```

`FlushPage` 即使页面不 dirty 也必须写；`FlushAllPages` 只处理有效 page。`DeletePage` 在目标不驻留内存时仍返回成功；驻留且 pinned 时失败；成功删除后执行 `replacer.Remove`、清元数据、清内存并把 frame 放回 free list。不要把同一个 frame 重复放入 free list。


#### `BufferPoolManager` 内部 frame helper

```text
PREPARE_FRAME_FOR_REUSE():
  assert BPM latch held
  if free_list not empty:
    fid = free_list.front; pop_front
  else:
    if not replacer.Evict(&fid): return NONE

  page = pages[fid]
  if page.page_id != INVALID_PAGE_ID:
    if page.is_dirty:
      disk.WritePage(page.page_id, page.data)
    erased = page_table.erase(page.page_id)
    assert erased == 1

  page.ResetMemory()
  page.page_id = INVALID_PAGE_ID
  page.pin_count = 0
  page.is_dirty = false
  return fid
```

#### 构造、创建与获取页面

```text
BPM_CONSTRUCTOR(pool_size, disk, k, log):
  assign dependencies
  pages = new Page[pool_size]
  replacer = new LRUKReplacer(pool_size, k)
  for fid in [0, pool_size): free_list.push_back(fid)

NEW_PAGE(out_pid):
  if out_pid is null: return null/abort according to contract
  lock BPM.latch
  fid = PREPARE_FRAME_FOR_REUSE()
  if NONE: return nullptr
  new_pid = AllocatePage()
  page = pages[fid]
  page.ResetMemory()
  page.page_id = new_pid
  page.pin_count = 1
  page.is_dirty = false
  page_table[new_pid] = fid
  replacer.RecordAccess(fid)
  replacer.SetEvictable(fid, false)
  *out_pid = new_pid
  return &page

FETCH_PAGE(pid, access_type):
  lock BPM.latch
  if page_table contains pid:
    fid = page_table[pid]
    page = pages[fid]
    page.pin_count++
    replacer.RecordAccess(fid, access_type)
    replacer.SetEvictable(fid, false)
    return &page

  fid = PREPARE_FRAME_FOR_REUSE()
  if NONE: return nullptr
  page = pages[fid]
  disk.ReadPage(pid, page.data)
  page.page_id = pid
  page.pin_count = 1
  page.is_dirty = false
  page_table[pid] = fid
  replacer.RecordAccess(fid, access_type)
  replacer.SetEvictable(fid, false)
  return &page
```

磁盘 I/O 放在全局 BPM latch 内实现最简单且正确，但并发度较低；本项目先保证正确性。若优化为 I/O 时释放 latch，需要额外的 in-flight page 状态，不能只把 unlock 移出去。

#### Unpin、Flush 与 Delete

```text
UNPIN_PAGE(pid, dirty_argument):
  lock BPM.latch
  it = page_table.find(pid)
  if absent: return false
  page = pages[it.fid]
  if page.pin_count <= 0: return false
  page.is_dirty = page.is_dirty OR dirty_argument
  page.pin_count--
  if page.pin_count == 0:
    replacer.SetEvictable(it.fid, true)
  return true

FLUSH_PAGE(pid):
  if pid == INVALID_PAGE_ID: return false/assert
  lock BPM.latch
  if pid absent: return false
  page = pages[page_table[pid]]
  disk.WritePage(pid, page.data)  // 不看 dirty
  page.is_dirty = false
  return true

FLUSH_ALL_PAGES():
  lock BPM.latch
  for each (pid, fid) in page_table:
    if pid == INVALID: continue
    disk.WritePage(pid, pages[fid].data)
    pages[fid].is_dirty = false

DELETE_PAGE(pid):
  lock BPM.latch
  if pid absent:
    DeallocatePage(pid)
    return true
  fid = page_table[pid]
  page = pages[fid]
  if page.pin_count > 0: return false
  replacer.Remove(fid)       // 此时必须是 evictable
  page_table.erase(pid)
  page.ResetMemory()
  page.page_id = INVALID
  page.pin_count = 0
  page.is_dirty = false
  free_list.push_back(fid)
  DeallocatePage(pid)
  return true
```


### Task 3：Read/Write Page Guards

修改 `page_guard.cpp/.h` 及 BPM 的 guarded wrappers。Guard 是 move-only RAII：持有时页面保持 pinned；析构或 `Drop` 时恰好 unpin 一次。读写 Guard 还分别管理 RLatch/WLatch，必须先解 page latch 再 unpin。

```text
MOVE: 转移 bpm/page/dirty；令源 guard 失效
DROP: 若有效 → 解 latch（派生 Guard）→ bpm.UnpinPage(pid, dirty) → 清空成员
UPGRADE_READ/WRITE: Basic 交出所有权 → 获取对应 page latch → 构造目标 Guard
```

#### Guard 所有权状态机

一个有效 Basic Guard 拥有“一次 Unpin 的责任”。默认构造或 move 后的源对象无此责任。用 `bpm_ != nullptr && page_ != nullptr` 表示有效最清楚。

```text
BASIC_MOVE_CTOR(src): steal src.bpm/page/dirty; src = invalid
BASIC_MOVE_ASSIGN(src):
  if this != &src:
    this.Drop()
    steal src state
    src = invalid
BASIC_DROP:
  if invalid: return
  copy pid/dirty/bpm
  invalidate self
  bpm.UnpinPage(pid, dirty)
```

Read/Write Guard 构造时对应 wrapper 已经取得页面 latch；`Drop` 顺序必须是 `RUnlatch/WUnlatch → BasicGuard.Drop`。反过来可能让 frame 在仍持 page latch 时被淘汰并复用。`AsMut/GetDataMut` 自动把 Basic Guard 标 dirty；只读 API 不应修改 dirty。

Guarded wrapper 的失败结果返回默认无效 Guard：

```text
FETCH_BASIC(pid): page = FetchPage(pid); return page ? BasicGuard(this,page) : BasicGuard()
FETCH_READ(pid): page = FetchPage(pid); if null return {}; page.RLatch(); return ReadGuard(this,page)
FETCH_WRITE(pid): page = FetchPage(pid); if null return {}; page.WLatch(); return WriteGuard(this,page)
```


#### BPM Guard wrapper 与 Page Guard

```text
NEW_PAGE_GUARDED(out_pid):
  page = NewPage(out_pid)
  if page == null: return empty BasicGuard
  return BasicGuard(this, page)

FETCH_BASIC(pid):
  page = FetchPage(pid)
  return page ? BasicGuard(this,page) : empty

FETCH_READ(pid):
  page = FetchPage(pid)
  if null: return empty
  page.RLatch()
  return ReadGuard(this,page)

FETCH_WRITE(pid):
  page = FetchPage(pid)
  if null: return empty
  page.WLatch()
  return WriteGuard(this,page)

BASIC_DROP():
  if page == null: return
  local_bpm=bpm; local_pid=page.PageId; local_dirty=is_dirty
  bpm=null; page=null; is_dirty=false
  local_bpm.UnpinPage(local_pid, local_dirty)

BASIC_MOVE_ASSIGN(src):
  if address(this)==address(src): return self
  Drop()
  bpm=src.bpm; page=src.page; is_dirty=src.is_dirty
  src.bpm=null; src.page=null; src.is_dirty=false
  return self

READ_DROP():
  if inner.page valid: inner.page.RUnlatch()
  inner.Drop()

WRITE_DROP():
  if inner.page valid: inner.page.WUnlatch()
  inner.Drop()
```

Read/Write move constructor直接 move 内部 Basic Guard；move assignment 先 `Drop()` 自己，再 move 源 Guard。析构函数只调用 `Drop()`，因此重复 Drop 必须安全。



#### Project 1 验收顺序

1. `lru_k_replacer_test`：先单线程语义，再自建并发压力测试。
2. `buffer_pool_manager_test`：重点观察 pin、dirty、free list 和磁盘写次数。
3. `page_guard_test`：覆盖 move construction、move assignment、自移动、重复 Drop 和离开作用域。
4. Debug/ASAN 与 Release 各跑一次；Release 会暴露错误依赖 `BUSTUB_ASSERT` 副作用的问题。


## Project 2：B+Tree

### Task 1：B+Tree Pages

实现公共页头、internal page 与 leaf page。数组操作必须保持有序，准确区分 `size` 与 `max_size`。Internal page 的第 0 个 key 无效，value 数量比有效分隔键多一个；leaf page 保存 key/RID 并维护 `next_page_id`。

```text
LEAF_LOOKUP(key): lower_bound；相等则返回 value
LEAF_INSERT: lower_bound 定位；重复则失败；右移并插入
INTERNAL_LOOKUP(key): 找到最后一个 <= key 的有效分隔键，返回对应 child
REMOVE_AT(i): 左移后续元素；size--
```

#### 页布局与容量不变量

B+Tree 页对象直接解释 `Page::data_`，因此不能用普通构造函数，也不能在页类中放动态容器或虚函数。`Init` 必须写全页头。建议先单独完成页级数组操作并测试，再写树结构。

- 公共页：`page_type_`、`size_`、`max_size_`。
- Leaf：`array_[0..size)` 都是有效 `(key, RID)`，另有 `next_page_id_`。
- Internal：`array_[0].second` 是最左 child，`array_[0].first` 无效；有效分隔键从下标 1 开始。
- 非根节点最小容量使用 `GetMinSize()` 的仓库定义，不要在树代码里散落手写 `(max+1)/2`。
- 临时溢出通常允许 `size == max_size`，随后立即 split；以当前 starter code 的测试约定为准。

页级 helper 推荐包括二分定位、插入、删除、批量移动。即使接口未预先声明，也可添加 private helper，但不要改变公开签名。

```text
LEAF_LOWER_BOUND(key):
  lo=0, hi=size
  while lo<hi:
    mid=(lo+hi)/2
    if array[mid].key < key: lo=mid+1 else hi=mid
  return lo

INTERNAL_CHILD(key):
  // 找最后一个 separator <= key；无则 child[0]
  lo=1, hi=size
  upper_bound over valid keys
  return ValueAt(result_index)
```


#### Page 基础函数与建议 helper

```text
BPLUS_TREE_PAGE.Init(type,max): page_type=type; size=0; max_size=max
IsLeafPage(): return page_type == LEAF
GetMinSize(): return IsLeaf ? max_size/2 : (max_size+1)/2  // 最终以 starter contract 为准

LEAF.Init(max): base.Init(LEAF,max); next_page_id=INVALID
LEAF.KeyAt(i): assert 0<=i<size; return array[i].first
LEAF.ValueAt(i): return array[i].second
LEAF.LowerBound(key): binary search first array[i].key >= key
LEAF.Insert(key,value):
  pos=LowerBound
  if pos<size and key equal: return false
  shift [pos,size) one slot right
  array[pos]=(key,value); size++; return true
LEAF.Remove(key):
  pos=LowerBound; if absent return false
  shift [pos+1,size) left; size--; return true

INTERNAL.Init(max): base.Init(INTERNAL,max)
INTERNAL.ValueIndex(child): linear/binary applicable search array[i].second
INTERNAL.Lookup(key):
  ans=0
  binary search valid keys [1,size) for last key <= target
  return array[ans].second
INTERNAL.InsertAfter(old_child,key,new_child):
  pos=ValueIndex(old_child)+1
  shift right; array[pos]=(key,new_child); size++
INTERNAL.RemoveAt(pos): shift left; size--
```


### Task 2a：Insertion 与 Point Search

下降到叶节点时保存祖先路径。插入后未溢出即可结束；溢出则创建兄弟页、均分元素，并把兄弟最小键插入父节点。根分裂时创建新根并更新 header page。

```text
GET(key): 从 root 逐层 InternalLookup；在 leaf 查找
INSERT(key, value):
  空树 → 创建 leaf root
  找到 leaf；重复返回 false；插入
  while 当前页溢出:
    split 当前页得到 sibling 与 separator
    若当前页是 root → 创建新 root
    否则向 parent 插入 (separator, sibling_id)，继续检查 parent
```

所有新建/获取的页面必须通过 Guard 管理，根 ID 变化必须写入 header page。

#### 查找与空树处理

header page 中的 root id 是树是否为空的唯一权威来源。每次操作先 guard header，复制 root id；只读操作随后可尽早释放 header guard。

```text
GET_VALUE(key, result):
  root = read header.root
  if root == INVALID: return false
  guard = FetchPageRead(root)
  while guard.As<Page>().IsLeaf == false:
    child = guard.As<Internal>().Lookup(key)
    next = FetchPageRead(child)
    guard = move(next)       // move assignment 自动释放父页
  pos = leaf.lower_bound(key)
  if pos valid and equal: result.push_back(value); return true
  return false
```

`IsEmpty` 和 `GetRootPageId` 也应读取 header，而不是维护容易失配的第二份 root 状态。

#### 插入：从局部变化向根传播

使用 `Context`：`header_page_` 保存 header 写 guard，`write_set_` 保存根到当前节点的写 guard。第一次插入创建 leaf，初始化后写 root id。普通插入先检查重复 key；失败路径不应留下新页或 dirty 的结构变化。

Leaf split 的基本步骤：

1. `NewPageGuarded` 创建并 `Init` 新 leaf。
2. 将有序元素分成左右两半。
3. `new_leaf.next = old_leaf.next`，再令 `old_leaf.next = new_leaf_id`。
4. 向父节点传播 `new_leaf.KeyAt(0)` 和 `new_leaf_id`。

Internal split 时，中间分隔关系必须符合当前页布局。传播给父亲的是新右页能够区分的首个有效键；移动后仍需保证两个 internal 页的第 0 key 无效。

```text
INSERT_IN_PARENT(left_id, separator, right_id, ctx):
  if left_id is root:
    new_root = NewInternal()
    new_root[0].value = left_id
    new_root insert(separator, right_id)
    header.root = new_root.id
    return
  parent = ctx.write_set previous guard
  insert pair immediately after left_id
  if parent overfull:
    split parent into parent/right_parent
    promoted = right_parent first separator according to layout
    INSERT_IN_PARENT(parent.id, promoted, right_parent.id, ctx)
```

不要在持有指向页内数据的裸指针后 move/drop 对应 Guard；frame 一旦 unpin，指针就可能失效。


#### Header、查找路径与只读接口

```text
READ_ROOT():
  header_guard = bpm.FetchPageRead(header_page_id)
  return header_guard.As<Header>().root_page_id

IS_EMPTY(): return READ_ROOT() == INVALID
GET_ROOT_PAGE_ID(): return READ_ROOT()

FIND_LEAF_READ(key, leftmost=false):
  pid = READ_ROOT(); if INVALID return empty
  guard = FetchPageRead(pid)
  while not guard.As<BPlusTreePage>().IsLeafPage:
    internal = guard.As<Internal>()
    child = leftmost ? internal.ValueAt(0) : internal.Lookup(key)
    child_guard = FetchPageRead(child)
    guard = move(child_guard)
  return guard

GET_VALUE(key, result):
  leaf_guard = FIND_LEAF_READ(key)
  if invalid: return false
  leaf = leaf_guard.As<Leaf>()
  pos = leaf.LowerBound(key)
  if pos==leaf.size or comparator(leaf.KeyAt(pos),key)!=0: return false
  result.push_back(leaf.ValueAt(pos))
  return true
```

#### 写路径获取与祖先释放

```text
FIND_LEAF_WRITE(key, operation, ctx):
  ctx.header_page = FetchPageWrite(header_page_id)
  ctx.root_page_id = header.root
  if root INVALID: return empty
  current = FetchPageWrite(root)
  while current is internal:
    child_id = current.Internal.Lookup(key)
    child = FetchPageWrite(child_id)  // parent 后 child
    ctx.write_set.push_back(move(current))
    if IS_SAFE(child, operation):
      ctx.header_page.reset()         // root 不会变化
      ctx.write_set.clear()           // RAII 释放祖先
    current = move(child)
  return current

IS_SAFE(page, INSERT): return page.size < page.max_size - 1 (按溢出约定调整)
IS_SAFE(page, DELETE): return page.size > page.min_size
```

安全阈值必须结合“插入后何时判 overflow”的约定推导，并通过小 max-size 测试确认，不能机械照抄上式。

#### Insert 完整传播

```text
INSERT(key,value):
  ctx.header = FetchPageWrite(header_id)
  if header.root == INVALID:
    new_guard = bpm.NewPageGuarded(&pid)
    leaf = new_guard.AsMut<Leaf>(); leaf.Init(leaf_max_size)
    leaf.Insert(key,value)
    header.root = pid
    return true

  leaf_guard = FIND_LEAF_WRITE(key, INSERT, ctx)
  leaf = leaf_guard.AsMut<Leaf>()
  if leaf contains key: return false
  leaf.Insert(key,value)
  if leaf.size < leaf.max_size: return true

  right_guard = NewPageGuarded(&right_id)
  right = right_guard.AsMut<Leaf>(); right.Init(leaf_max_size)
  split_index = leaf.size / 2
  move leaf[split_index..end) to right[0..]
  shrink leaf; set right size
  right.next = leaf.next; leaf.next = right_id
  separator = right.KeyAt(0)
  PROPAGATE_SPLIT(leaf_guard.PageId, separator, right_id, ctx)
  return true

PROPAGATE_SPLIT(left_id,key,right_id,ctx):
  while true:
    if left_id == ctx.root_page_id:
      root_guard=NewPageGuarded(&new_root_id)
      root.Init(internal_max_size)
      root.array[0].value=left_id
      root.array[1]=(key,right_id); root.size=2
      ctx.header.AsMut<Header>().root_page_id=new_root_id
      return

    parent_guard = ctx.write_set.back; pop_back
    parent.InsertAfter(left_id,key,right_id)
    if parent.size < parent.max_size: return

    new_guard=NewPageGuarded(&new_id); new_internal.Init(internal_max_size)
    choose split boundary according to internal layout
    promoted_key = boundary separator
    move right half child mappings into new_internal
    make new_internal.array[0].key invalid
    left_id=parent_guard.PageId; key=promoted_key; right_id=new_id
```


### Task 2b：Deletion

删除后若节点仍满足最小占用率则结束；否则优先向同父兄弟借元素，不能借时合并并删除父节点中的分隔项。内部节点借/并时要正确移动父分隔键。根只剩一个 child 时降低树高，空 leaf root 则置空树。

```text
REMOVE(key): 找 leaf 并删除；不存在则结束
while 非 root 且 underflow:
  若 sibling 可借 → redistribute sibling/current 并更新 parent separator；结束
  否则 merge 两节点；删除 parent 对应项；删除被合并页；current = parent
调整 root：internal size==1 → 唯一 child 成新根；leaf size==0 → root=INVALID
```

#### 删除：借位、合并与根收缩

删除可分成三个稳定步骤：先删 leaf key，再修复占用率，最后修根。每次处理 underflow 节点都从 Context 取 parent，通过 `ValueIndex(child_id)` 找当前节点位置，并据此选择左/右 sibling。

```text
REPAIR_UNDERFLOW(node, parent):
  index = parent.ValueIndex(node.id)
  left  = index>0 ? parent.ValueAt(index-1) : NONE
  right = index+1<parent.size ? parent.ValueAt(index+1) : NONE

  if left exists and left.size > left.min:
    move left last entry to node front
    update parent separator at index
    return DONE
  if right exists and right.size > right.min:
    move right first entry to node end
    update parent separator at index+1
    return DONE

  if left exists:
    merge node into left; parent.remove(index); DeletePage(node.id)
  else:
    merge right into node; parent.remove(index+1); DeletePage(right.id)
  return PARENT_MAY_UNDERFLOW
```

Leaf 借位后父分隔键应等于右侧 leaf 的新最小键。Internal 借位不同：父分隔键下沉到接收节点，兄弟的边界键上升到父节点。Internal merge 也必须把父分隔键带入合并结果，不能只拼接两个数组。

根处理：

```text
if root is leaf and root.size == 0:
  header.root = INVALID; DeletePage(old_root)
else if root is internal and root.size == 1:
  header.root = root.ValueAt(0); DeletePage(old_root)
```

只有实际从 BPM 删除页后才可让其 frame 回收；确保相关 Guard 已 Drop，否则 `DeletePage` 会因 pin count 非零失败。


#### Remove、借位和合并

```text
REMOVE(key):
  if tree empty: return
  leaf_guard = FIND_LEAF_WRITE(key, DELETE, ctx)
  if key absent: return
  leaf.Remove(key)
  if leaf is root:
    ADJUST_ROOT(leaf_guard,ctx); return
  if leaf.size >= leaf.min_size: return
  COALESCE_OR_REDISTRIBUTE(move(leaf_guard),ctx)

COALESCE_OR_REDISTRIBUTE(node_guard,ctx):
  while node is not root and node.size < node.min_size:
    parent_guard = ctx.write_set.back; pop_back
    index = parent.ValueIndex(node.PageId)
    choose sibling_id = (index>0 ? parent.ValueAt(index-1) : parent.ValueAt(index+1))
    sibling_guard = FetchPageWrite(sibling_id)

    if sibling.size > sibling.min_size:
      if node/sibling are leaves:
        move one boundary mapping to deficient leaf
        update parent key to right leaf's first key
      else:
        rotate parent separator down and sibling boundary key up
      return

    if sibling is left:
      merge node into sibling (internal merge includes parent.KeyAt(index))
      victim_id=node.PageId; parent.RemoveAt(index)
      drop node guard; bpm.DeletePage(victim_id)
    else:
      merge sibling into node (internal merge includes parent.KeyAt(index+1))
      victim_id=sibling.PageId; parent.RemoveAt(index+1)
      drop sibling guard; bpm.DeletePage(victim_id)

    node_guard = move(parent_guard)

  if node is root: ADJUST_ROOT(node_guard,ctx)

ADJUST_ROOT(root_guard,ctx):
  if root is leaf and root.size==0:
    old=root.id; ctx.header.root=INVALID
    drop root; bpm.DeletePage(old)
  else if root is internal and root.size==1:
    old=root.id; ctx.header.root=root.ValueAt(0)
    drop root; bpm.DeletePage(old)
```


### Task 3：Index Iterator

实现 `index_iterator.cpp/.h` 与 `Begin/End`。迭代器保存当前 leaf guard 和数组下标；跨越页尾时沿 `next_page_id` 获取下一 leaf。`End` 使用稳定的哨兵状态。

```text
DEREFERENCE: 返回 leaf[index]
INCREMENT: index++；若到页尾则 fetch next leaf，index=0；无 next 则设为 End
BEGIN: 从根一路走最左 child；BEGIN(key) 定位 key 的 lower_bound
```

#### Iterator 生命周期

Iterator 至少保存 BPM 指针、当前 `ReadPageGuard` 和 index。它拥有 pin/latch，因此应 move，不应复制一个 Guard。`operator*` 返回当前 mapping 的 const 引用，其有效期只保证到迭代器移动或销毁当前 guard 之前。

```text
OPERATOR_PLUS_PLUS:
  if end: return self
  index++
  if index < leaf.size: return self
  next = leaf.next_page_id
  if next == INVALID: guard.Drop(); index=0; mark end
  else: guard = bpm.FetchPageRead(next); index=0
  return self

BEGIN(): 找最左 leaf；空树返回 End
BEGIN(key): 像查找一样下降；index=lower_bound(key)；若 index==size 则前进到下一 leaf
END(): 返回无 guard 的统一哨兵
```

相等比较应比较 end 状态，或比较同一 leaf page id 与 index；不要比较 Guard 对象地址。


#### Iterator 与 Begin/End

```text
BEGIN():
  guard=FIND_LEAF_READ(dummy,leftmost=true)
  if invalid or leaf.size==0: return END()
  return Iterator(bpm, move(guard), index=0)

BEGIN(key):
  guard=FIND_LEAF_READ(key)
  if invalid: return END
  index=leaf.LowerBound(key)
  if index==leaf.size:
    next=leaf.next
    if next INVALID: return END
    guard=FetchPageRead(next); index=0
  return Iterator(...)

ITERATOR_INCREMENT():
  if IsEnd: return self
  index++
  if index < leaf.size: return self
  next=leaf.next
  if next INVALID: guard.Drop(); index=0; return self
  guard=FetchPageRead(next); index=0; return self

IS_END(): return guard invalid
EQUAL(a,b):
  if both End: true
  if exactly one End: false
  return page_id equal and index equal
DEREFERENCE(): assert not End; return leaf.array[index]
```


### Task 4：Concurrency Control

使用 page guard 实现 latch crabbing，禁止用全局树锁替代。查找一路持读锁；写操作持写锁下降，当子节点对当前操作“安全”时释放更早祖先。插入安全通常表示不会分裂，删除安全表示删除后不会下溢。

```text
DESCEND(op): 锁 parent → 锁 child → 若 child 对 op 安全则释放所有祖先 → 继续
SEARCH: 逐层获取下一页读锁后释放上一页
WRITE: 保留可能发生结构修改的祖先；split/merge 时按固定顺序锁相关页
```

不要在同一线程重复获取同一页读锁；所有返回路径都依靠 Guard 释放 latch/pin。

#### 并发 latch crabbing 细化

`Context::read_set_`/`write_set_` 用 RAII 表达锁链。只读下降采用 lock coupling：先获取 child read guard，再释放 parent。插入/删除下降时，拿到 child write guard 后判断 child 是否安全；安全则释放 header 与所有祖先，只保留 child。

- 插入安全：再插入一个元素也不会 split。
- 删除安全：删除一个元素后仍不会 underflow；root 的判定单独处理。
- 所有结构修改都从叶向上使用仍保留的 write guards。
- 多页操作采用一致顺序，例如先 parent，再按 page id/左右顺序获取 sibling，避免交叉死锁。

并发测试不能只验证结果，还应验证线程能结束。超时通常意味着重复锁同页、Guard 未释放、锁顺序反转，或在持有 header 写锁时等待不必要的页面。



#### Project 2 验收矩阵

- 插入：空树、重复 key、连续递增/递减、随机顺序、多层连续 split、根 split。
- 查找：最小/最大/不存在 key，split 边界 key。
- 删除：无 underflow、向左/向右借、leaf/internal merge、级联合并、根收缩、删空树。
- Iterator：空树、`Begin(key)` 命中/间隙/大于最大值、跨多个 leaf、与 End 比较。
- 并发：只读、并发插入不相交 key、并发删除、读写混合；运行 contention 与 sequential-scale 测试。


## Project 3：Query Execution

所有执行器遵循 Volcano 模型：`Init()` 初始化状态，`Next()` 每次产生至多一个 tuple/RID。输出必须严格符合 plan 的 output schema。

### Task 1：Access Method Executors

实现 SeqScan、Insert、Update、Delete、IndexScan。扫描跳过逻辑删除记录；写操作消费完整 child，并维护表和所有索引，最终只输出一行受影响数量。

```text
SEQ_NEXT: iterator 前进直到找到未删除 tuple；按 output expression 投影并返回 tuple/RID
INSERT_NEXT: 若已执行返回 false；循环 child tuple → table.InsertTuple → 每个 index.InsertEntry；返回 count
DELETE_NEXT: 循环 child tuple/RID → 更新 TupleMeta.is_deleted → 每个 index.DeleteEntry；返回 count
INDEX_NEXT: iterator 给出 key/RID → TableHeap.GetTuple → 跳过 deleted → 返回 tuple
```

#### 先理解执行框架

开始前依次阅读 `AbstractExecutor`、一个已实现的流式执行器（如 Filter/Projection）、plan node、`ExecutorFactory`、Catalog/TableHeap/IndexInfo。Executor 构造函数只保存依赖；`Init` 允许重复调用并必须重置全部游标；`Next` 返回 false 后再次调用也应稳定返回 false。

每个 `Next(tuple, rid)` 都要回答三个问题：输入 schema 是谁的、表达式按哪个 schema Evaluate、输出 tuple 用哪个 schema 构造。不要默认 child schema 等于 table schema。

#### Task 1：五种 Access Method Executor

SeqScan 初始化 catalog 中的 `TableInfo` 和 table iterator。每次 Next 先保存当前 tuple/RID，再推进 iterator，避免前后置 `++` 混淆；读取 `TupleMeta` 并跳过 deleted。Spring 2023 的 SeqScan 输出原 tuple 的副本与原 RID。

```text
INIT: table_info = catalog.GetTable(plan.table_oid); iter = table.MakeIterator()
NEXT:
  while not iter.IsEnd:
    (meta, current_tuple) = iter.GetTuple
    rid = current_tuple.GetRid
    ++iter
    if meta.is_deleted: continue
    output tuple=current_tuple, rid=rid; return true
  return false
```

Insert/Update/Delete 是一次性执行器，需要 `executed_` 标志。它们完全消费 child，最后只产生一个单列 INTEGER tuple；第二次 Next 返回 false。

```text
INSERT_NEXT:
  if executed: return false
  count=0
  while child.Next(tuple, ignored_rid):
    new_rid = table.InsertTuple(meta_not_deleted, tuple)
    for index in catalog.GetTableIndexes(table.name):
      key = tuple.KeyFromTuple(table.schema, index.key_schema, index.key_attrs)
      index.InsertEntry(key, new_rid, txn)
    count++
  output Tuple([count], plan.OutputSchema); executed=true; return true
```

Update 不能原地覆盖：对 child 给出的旧 tuple/RID 计算 target expressions 得到新 tuple，逻辑删除旧 RID，再插入新 tuple；每个索引先删除旧 key/RID，再插入新 key/new RID。表达式基于 child tuple 与 child output schema 求值。

Delete 根据 child 返回的 RID 更新 `TupleMeta.is_deleted_`，并从全部索引删除对应 entry。先取得旧 meta，再只改变删除字段；不要破坏 starter code 中其余事务字段约定。

IndexScan 初始化 `IndexInfo`、对应 `TableInfo`、B+Tree 具体类型及 iterator。Next 从索引得到 RID，再从 TableHeap 取 tuple；逻辑删除 tuple 必须跳过。索引只提供定位，最终输出数据来自 table。

Task 1 的一致性检查：任何 table 写操作和 index 写操作必须成对；构造 index key 时使用 table schema、index key schema 和 key attrs，不能假设索引永远是第 0 列。


#### Access Method Executors

```text
SEQ_SCAN_CONSTRUCTOR(ctx,plan):
  save ctx, plan
  table_info = catalog.GetTable(plan.GetTableOid)

SEQ_SCAN_INIT():
  iterator = table_info.table.MakeIterator()

SEQ_SCAN_NEXT(out_tuple,out_rid):
  while iterator not End:
    (meta, source) = iterator.GetTuple()
    current_rid = source.GetRid()
    iterator++
    if meta.is_deleted: continue
    *out_tuple = copy source
    *out_rid = current_rid
    return true
  return false

INSERT_INIT():
  child.Init()
  table_info = catalog.GetTable(plan.TableOid)
  indexes = catalog.GetTableIndexes(table_info.name)
  executed = false

INSERT_NEXT(out_tuple,out_rid):
  if executed: return false
  count=0
  while child.Next(&input,&unused_rid):
    meta={insert_txn=INVALID, delete_txn=INVALID, is_deleted=false}
    new_rid = table.InsertTuple(meta,input)
    if insertion failed: handle according to API; continue/throw
    for idx_info in indexes:
      key = input.KeyFromTuple(table_schema, idx.key_schema, idx.key_attrs)
      idx.index.InsertEntry(key,new_rid,txn)
    count++
  *out_tuple = Tuple([Integer(count)], plan.OutputSchema)
  *out_rid = INVALID_RID
  executed=true
  return true
```

```text
UPDATE_INIT(): child.Init; load table/index metadata; executed=false

UPDATE_NEXT(out_tuple,out_rid):
  if executed: return false
  count=0
  while child.Next(&old_tuple,&old_rid):
    values=[]
    for target_expr in plan.TargetExpressions:
      values.push(expr.Evaluate(old_tuple, child.OutputSchema))
    new_tuple=Tuple(values, table_schema)

    old_meta=table.GetTupleMeta(old_rid)
    old_meta.is_deleted=true
    table.UpdateTupleMeta(old_meta,old_rid)

    new_meta={...,is_deleted=false}
    new_rid=table.InsertTuple(new_meta,new_tuple)
    for idx in indexes:
      old_key=old_tuple.KeyFromTuple(...)
      new_key=new_tuple.KeyFromTuple(...)
      idx.DeleteEntry(old_key,old_rid,txn)
      idx.InsertEntry(new_key,new_rid,txn)
    count++
  output count tuple; executed=true; return true

DELETE_INIT(): child.Init; load table/index metadata; executed=false
DELETE_NEXT(out_tuple,out_rid):
  if executed: return false
  count=0
  while child.Next(&old_tuple,&rid):
    meta=table.GetTupleMeta(rid)
    if meta.is_deleted: continue/ensure child already filtered
    meta.is_deleted=true
    table.UpdateTupleMeta(meta,rid)
    for idx in indexes:
      key=old_tuple.KeyFromTuple(...)
      idx.DeleteEntry(key,rid,txn)
    count++
  output count tuple; executed=true; return true
```

```text
INDEX_SCAN_CONSTRUCTOR:
  index_info=catalog.GetIndex(plan.IndexOid)
  table_info=catalog.GetTable(index_info.table_name)
  tree=dynamic_cast<BPlusTreeIndexForTwoIntegerColumn*>(index_info.index)

INDEX_SCAN_INIT(): iterator=tree.GetBeginIterator(); end=tree.GetEndIterator()
INDEX_SCAN_NEXT(out_tuple,out_rid):
  while iterator != end:
    mapping=*iterator; ++iterator
    rid=mapping.second
    (meta,source)=table.GetTuple(rid)
    if meta.is_deleted: continue
    *out_tuple=source; *out_rid=rid; return true
  return false
```


### Task 2：Aggregation 与 Join

Aggregation 是 pipeline breaker：先消费 child 构建以 group-by 值为 key 的内存哈希表，再迭代输出结果；HAVING 由上层 Filter 执行。空输入、NULL 和无 group-by 聚合需要单独处理。

```text
AGG_INIT: child.Init；对每个 tuple 生成 group key 与 aggregate inputs；Combine
AGG_NEXT: 遍历 hash table；构造 group columns + aggregate values 并输出（HAVING 由上层 Filter 处理）
NLJ_NEXT: 为每个 left tuple 重新 Init/扫描 right；predicate 为 TRUE 才匹配；LEFT JOIN 无匹配时补 NULL
HASH_JOIN_INIT: 消费一侧构建 key→tuples 哈希表
HASH_JOIN_NEXT: 扫描另一侧并逐个输出匹配；LEFT JOIN 未匹配时补 NULL
```

优化规则 `nlj_as_hash_join.cpp` 只在谓词是跨左右子树的等值连接时改写，并保持 schema、join type 与子节点不变。

#### Task 2：Aggregation 状态与 NULL

`SimpleAggregationHashTable` 的 key 是 group-by values，value 是 aggregate states。`COUNT(*)` 每行加一；`COUNT(expr)` 只统计非 NULL；`SUM/MIN/MAX` 遇 NULL 不更新，首个非 NULL 值替换初始 NULL。

```text
COMBINE(state, input, type):
  COUNT_STAR: state = state + 1
  COUNT: if input not NULL: state = state + 1
  SUM: if input not NULL: state = (state NULL ? input : state + input)
  MIN: if input not NULL and (state NULL or input < state): state=input
  MAX: if input not NULL and (state NULL or input > state): state=input
```

Aggregation 的 build phase 放在 `Init` 最直观：初始化 child 和空 hash table，消费所有输入，生成 group key 与 aggregate input 后 Combine，最后保存 hash iterator。无 group-by 且输入为空时仍需人工插入一个初始 aggregate row，使 COUNT(*) 输出 0、其他聚合输出 NULL；有 group-by 且空输入则输出 0 行。HAVING 在本学期由上层 Filter 处理，不应塞进 AggregationExecutor。

NestedLoopJoin 需要持久化 outer tuple、inner 扫描位置、outer 是否匹配等跨 `Next` 状态。

```text
NLJ_NEXT:
  loop:
    if no current_left:
      if not left.Next(current_left): return false
      right.Init(); matched=false
    while right.Next(r):
      pred = plan.Predicate.EvaluateJoin(left,left_schema,r,right_schema)
      if pred is non-NULL and pred.GetAs<bool>() == true:
        matched=true; output concat(left,r); return true
    if join_type==LEFT and not matched:
      output concat(left, NULL values matching right schema)
      clear current_left; return true
    clear current_left
```

HashJoin 选择一侧在 `Init` 全量 build，key 必须支持多列、哈希碰撞和同 key 多 tuple。SQL NULL join key 不应与另一个 NULL 匹配。Probe 侧的一个 tuple 可能对应 vector 中多个匹配，因此保存 probe tuple 与 match index 跨越多次 Next。LEFT JOIN 若左侧作为 probe，查无匹配时补右侧 typed NULL。

优化规则必须先递归优化 children，再检查当前节点：

1. 当前节点必须为 NLJ，且 join type 被 HashJoin 支持。
2. predicate 必须是 `=`，或由 AND 连接的一到两个 `=`。
3. 每个等式两边必须分别引用 tuple index 0 和 1；若反向出现则交换。
4. 提取左右 key expression，创建 HashJoinPlanNode，保留 output schema 和 optimized children。
5. 任一条件不满足，返回仅替换了 children 的原类型 plan。


#### AggregationExecutor 与哈希表

```text
MAKE_AGG_KEY(tuple):
  return [expr.Evaluate(tuple,child_schema) for expr in plan.GroupBys]
MAKE_AGG_VALUE(tuple):
  return [expr.Evaluate(tuple,child_schema) for expr in plan.Aggregates]

AGG_INIT():
  child.Init()
  hash_table.Clear()
  saw_input=false
  while child.Next(&tuple,&rid):
    saw_input=true
    hash_table.InsertCombine(MAKE_AGG_KEY(tuple), MAKE_AGG_VALUE(tuple))
  if not saw_input and plan.GroupBys empty:
    hash_table.InsertInitial(empty_key)
  iterator=hash_table.Begin()

AGG_NEXT(out_tuple,out_rid):
  if iterator == hash_table.End: return false
  key=iterator.Key; aggregate=iterator.Value
  values=key.group_bys followed by aggregate.values
  *out_tuple=Tuple(values,plan.OutputSchema)
  *out_rid=INVALID_RID
  ++iterator
  return true
```

`CombineAggregateValues` 对每个 aggregate index 独立 switch；不要把所有聚合共用一个 NULL 分支。COUNT 的初始值为 0，其他初始值为类型正确的 NULL。

#### NestedLoopJoinExecutor

需要成员：`left_tuple_`、`have_left_`、`left_matched_`。由于一次 `Next` 只能返回一行，不能把这些变量写成局部变量。

```text
NLJ_INIT():
  left.Init(); right.Init()
  have_left=false; left_matched=false

NLJ_NEXT(out_tuple,out_rid):
  forever:
    if not have_left:
      if not left.Next(&left_tuple,&left_rid): return false
      have_left=true; left_matched=false
      right.Init()

    while right.Next(&right_tuple,&right_rid):
      predicate=plan.Predicate.EvaluateJoin(left_tuple,left_schema,right_tuple,right_schema)
      if not predicate.IsNull and predicate.GetAsBool:
        left_matched=true
        values = all values(left_tuple) + all values(right_tuple)
        *out_tuple=Tuple(values,plan.OutputSchema)
        *out_rid=INVALID_RID
        return true

    if plan.JoinType==LEFT and not left_matched:
      values=left values + one typed NULL per right output column
      have_left=false
      output values; return true

    have_left=false
```

#### HashJoinExecutor

若 build right/probe left，LEFT JOIN 最自然。成员需要 `hash_table<Key, vector<Tuple>>`、当前 left/probe tuple、当前匹配 vector 指针或副本、match cursor、probe_has_matches。

```text
HASH_JOIN_INIT():
  left.Init(); right.Init(); hash.clear()
  while right.Next(&r,&rid):
    key=EvaluateRightJoinKey(r)
    if any key component NULL: continue
    hash[key].push_back(r)
  have_probe=false; match_index=0

HASH_JOIN_NEXT(out_tuple,out_rid):
  forever:
    if have_probe and match_index < matches.size:
      r=matches[match_index++]
      output concat(probe,r); return true

    if have_probe:
      have_probe=false
      // 之前有 matches 时已经全部输出；无 matches 的 LEFT 行在取 probe 时直接输出

    if not left.Next(&probe,&rid): return false
    key=EvaluateLeftJoinKey(probe)
    matches = (key contains NULL ? empty : hash.find(key))
    match_index=0; have_probe=true
    if matches not empty: continue
    if join_type==LEFT:
      have_probe=false
      output probe + typed right NULLs; return true
    have_probe=false
```

Key equality必须逐列调用 `CompareEquals`，hash 必须组合每列 hash；hash 相同不代表 key 相等，`unordered_map` equality仍需正确实现。

#### 两个 optimizer rule

```text
OPTIMIZE_NLJ_AS_HASH_JOIN(plan):
  children=[Optimize(child) for child]
  rebuilt=plan.CloneWithChildren(children)
  if rebuilt not NestedLoopJoin: return rebuilt
  conditions=FLATTEN_AND(rebuilt.predicate)
  if conditions.size not in supported range: return rebuilt
  left_keys=[]; right_keys=[]
  for condition in conditions:
    if condition not Comparison(EQUAL,lhs,rhs): return rebuilt
    if lhs tuple_idx==0 and rhs tuple_idx==1: append lhs,rhs
    else if lhs tuple_idx==1 and rhs tuple_idx==0: append rhs,lhs
    else: return rebuilt
  return HashJoinPlan(output_schema, children, left_keys,right_keys,join_type)

OPTIMIZE_SORT_LIMIT_AS_TOPN(plan):
  children=[Optimize(child) for child]
  rebuilt=plan.CloneWithChildren(children)
  if rebuilt is Limit and rebuilt.child is Sort:
    sort=rebuilt.child
    return TopNPlan(rebuilt.output_schema, sort.child, sort.order_bys, rebuilt.limit)
  return rebuilt
```


### Task 3：Sort、Limit 与 Top-N

Sort 在 `Init` 消费 child，用每个 order-by expression 和 ASC/DESC 比较器排序；本学期 Limit 不支持 offset，只输出最多 limit 行。Top-N 用大小为 N 的堆避免保存全部输入。优化规则把相邻 `Limit(Sort(child))` 改写为 `TopN(child)`。

```text
SORT_INIT: 收集全部 tuple → 按多列 comparator 排序；NEXT 顺序返回
LIMIT_NEXT: 已返回 limit 行则 false；否则调用 child.Next，成功时计数加一
TOPN_INIT: 对每个 tuple push heap；若 size>N 则 pop 当前最差项；最后整理为输出顺序
REWRITE: 若 node=Limit 且 child=Sort → TopN(sort.order_bys, limit, sort.child)
```

#### Task 3：Comparator、Limit 与 Top-N

Sort comparator 对 order-bys 逐项求值。第一处不相等决定次序；DEFAULT 按 ASC。比较 `Value` 时使用 BusTub 比较 API并处理三值结果，不要直接依赖 C++ 运算符。所有 key 都相等时返回 false，满足 strict weak ordering。

```text
LESS(a,b):
  for (direction, expr) in order_bys:
    va=expr.Evaluate(a, child_schema); vb=expr.Evaluate(b, child_schema)
    if va == vb: continue
    if direction DESC: return va > vb
    else: return va < vb
  return false
```

本学期 `LimitPlanNode` 不要求 offset：保存 `emitted_`，当 `emitted_ == limit` 返回 false，否则向 child 要下一行并原样转发 tuple/RID。

TopN 的堆顶应是“当前保留结果中最差的一项”，这样每读一行只需与堆顶比较。N=0 必须直接得到空结果。Build 后把堆弹入数组会得到逆序，需反转或从尾到头输出。

```text
TOPN_INIT:
  child.Init; heap=empty
  while child.Next(t):
    heap.push(t)
    if heap.size > N: heap.pop_worst()
  while heap not empty: result.push_back(heap.top); heap.pop
  reverse(result); cursor=0
```

`SortLimitAsTopN` 仅匹配 `Limit(Sort(child))`，用 Sort 的 `order_bys` 与 Limit 的 limit 构造 TopN，child 应是已经递归优化的 Sort child。不要保留原 Sort 节点，否则没有节省排序。

#### Sort、Limit、TopN

```text
SORT_INIT():
  child.Init; tuples=[]
  while child.Next(&t,&rid): tuples.push_back(t)
  sort(tuples, MULTI_COLUMN_LESS)
  cursor=0
SORT_NEXT(out,rid):
  if cursor==tuples.size: return false
  *out=tuples[cursor++]; *rid=INVALID; return true

MULTI_COLUMN_LESS(a,b):
  for (direction,expr) in order_bys:
    av=expr.Evaluate(a,child_schema); bv=expr.Evaluate(b,child_schema)
    if values equal: continue
    return direction==DESC ? av greater bv : av less bv
  return false

LIMIT_INIT(): child.Init; emitted=0
LIMIT_NEXT(out,rid):
  if emitted>=limit: return false
  if not child.Next(out,rid): return false
  emitted++; return true

TOPN_INIT():
  child.Init; max_heap ordered by desired result order; clear result
  if N==0: return
  while child.Next(&t,&rid):
    heap.push(t)
    if heap.size>N: heap.pop()   // pop 当前最差
  result.resize(heap.size)
  for i from result.size-1 down to 0: result[i]=heap.top; heap.pop
  cursor=0
TOPN_NEXT(out,rid):
  if cursor==result.size: return false
  *out=result[cursor++]; *rid=INVALID; return true
```


#### Project 3 调试与测试路线

- 使用 `EXPLAIN (o,s)` 确认实际 plan 和 schema；执行器没被选中就不能证明其正确。
- 按官方 `p3.00` 到 `p3.19` 顺序跑 SQLLogicTest，失败时加 `--verbose`。
- 写入类测试后立刻用 SeqScan 验证 table，再创建 index 用 IndexScan 验证索引一致性。
- Join 覆盖一对多、多对多、空一侧、LEFT 无匹配、多 key 和 NULL。
- Aggregate 覆盖空输入、全 NULL、无 group-by、多 group、DISTINCT（group-by 实现）。
- Sort/TopN 覆盖多列方向、N=0、N 大于行数和所有 key 相等。
- `p3.19-integration-2.slt` 还应在 Release 下运行；ASAN 用于排查越界和对象生命周期。



## Project 4：Concurrency Control

### Task 1：Lock Manager

实现表/行锁请求队列、五种锁模式兼容矩阵、升级和隔离级别约束。每个资源队列由 mutex + condition variable 保护；等待必须使用谓词循环。先持正确的表级意向锁，才能申请行锁。

```text
LOCK(resource, mode):
  校验事务状态、隔离级别、锁层次与升级合法性；非法则 Abort
  入队请求；wait 直到请求位于可授予位置且与已授予锁兼容
  标记 granted；更新 transaction lock set

UNLOCK: 校验前置条件；移除请求与事务 lock set；必要时进入 SHRINKING；notify_all
```

一次只能有一个升级者；兼容判断要考虑队列公平性，避免后来请求越过等待中的冲突请求。

#### Task 1：先编码规则表，再写队列

先把 `lock_manager.h` 的 `[LOCK_NOTE]`、`[UNLOCK_NOTE]` 转成纯 helper，避免在四个公开接口中复制规则：

- `AreLocksCompatible(held, requested)`：五模式兼容矩阵。
- `CanTxnTakeLock(txn, mode)`：隔离级别与 GROWING/SHRINKING 状态。
- `CanLockUpgrade(old, new)`：只允许指定升级边。
- `CheckAppropriateLockOnTable(txn, oid, row_mode)`：行 S 需要表 IS/S/SIX/X；行 X 需要 IX/SIX/X。
- `GrantNewLocksIfPossible(queue)`：按 FIFO 批量授予队首连续兼容请求。

表锁兼容关系应先写成测试表：

| 已授予 \\ 请求 | IS | IX | S | SIX | X |
|---|---:|---:|---:|---:|---:|
| IS | ✓ | ✓ | ✓ | ✓ | ✗ |
| IX | ✓ | ✓ | ✗ | ✗ | ✗ |
| S | ✓ | ✗ | ✓ | ✗ | ✗ |
| SIX | ✓ | ✗ | ✗ | ✗ | ✗ |
| X | ✗ | ✗ | ✗ | ✗ | ✗ |

Row 只允许 S/X。非法操作不是普通 false：应先把 txn 设为 ABORTED，再抛带正确 AbortReason 的 `TransactionAbortException`。事务已因死锁被异步 abort、在等待中醒来则移除请求并返回 false。

`LockTable`/`LockRow` 的正常请求流程：

```text
LOCK(txn, mode, resource):
  validate mode + isolation/state + multilevel prerequisite
  lock global map latch; get_or_create resource queue; unlock global latch
  lock queue.latch
  if txn already holds same mode: return true
  if txn holds different mode: return UPGRADE(...)
  request = new LockRequest(granted=false); queue.push_back(request)
  GrantNewLocksIfPossible(queue)
  while not request.granted and txn.state != ABORTED:
    queue.cv.wait(queue.latch)
    GrantNewLocksIfPossible(queue)
  if aborted:
    erase/delete request; clear upgrading if needed; grant/notify successors; return false
  add resource to txn matching lock set
  return true
```

请求对象是裸指针时必须明确唯一所有者和 delete 时机。访问 table/row lock map 时用 map latch；访问某资源请求队列时用 queue latch；始终保持固定加锁顺序，且等待 `cv` 前不能持 map latch。

升级流程：旧请求暂时保留或移到优先位置均可，但对外必须表现为：同资源最多一个 `upgrading_`；验证升级边；升级请求优先于普通 waiter；等待期间不能让事务同时拥有两种 bookkeeping；成功/失败都重置 `upgrading_` 并通知。

允许的升级：`IS→S/X/IX/SIX`、`S→X/SIX`、`IX→X/SIX`、`SIX→X`。相同模式直接成功，降级与其他边按 incompatible upgrade 处理。

`UnlockRow(force=false)`/`UnlockTable`：

```text
UNLOCK:
  locate granted request owned by txn; missing → abort+throw
  if table unlock and txn still holds any row lock on table → abort+throw
  erase request; remove from txn lock set
  unless force: apply 2PL state transition based on isolation + released mode
  GrantNewLocksIfPossible(queue); cv.notify_all()
```

RR 释放 S/X 进入 SHRINKING；RC 只有释放 X 进入 SHRINKING；RU 只有 X 合法且释放后进入 SHRINKING。`force=true` 用于扫描跳过不可见 tuple，不触发两阶段锁状态变化。


#### 验证、兼容与 bookkeeping helper

```text
ABORT_AND_THROW(txn,reason): txn.state=ABORTED; throw TransactionAbortException(txn.id,reason)

CAN_TXN_TAKE_LOCK(txn,mode):
  if txn.state==ABORTED: return false
  switch isolation:
    RR: if state==SHRINKING reject every mode
    RC: if state==SHRINKING allow only S/IS
    RU: reject S/IS/SIX always; if state==SHRINKING reject X/IX
  return true

ADD_LOCK_SET(txn,mode,table_or_row): insert resource into matching txn set
REMOVE_LOCK_SET(...): erase from exactly the matching set
```

#### Table/Row Lock 公共主流程

```text
LOCK_TABLE(txn,mode,oid):
  validate CanTxnTakeLock or abort+throw
  under table_lock_map_latch: queue=get_or_create(oid)
  lock queue.latch
  existing=find request with txn.id
  if existing granted:
    if existing.mode==mode: return true
    return UPGRADE_TABLE(queue,existing,mode)
  request=new LockRequest(txn.id,mode,oid)
  queue.push_back(request)
  GRANT_NEW_LOCKS(queue)
  wait with predicate: request.granted OR txn.state==ABORTED
  if txn aborted:
    erase request; delete request; GRANT_NEW_LOCKS; notify_all; return false
  ADD_LOCK_SET(txn,mode,oid)
  return true

LOCK_ROW(txn,mode,oid,rid):
  if mode is IS/IX/SIX: abort ATTEMPTED_INTENTION_LOCK_ON_ROW
  validate transaction state/isolation
  validate txn holds appropriate table lock
  obtain row queue from row_lock_map
  then same enqueue/wait/grant flow
  on success add rid to shared/exclusive row lock set for oid
```

`GRANT_NEW_LOCKS` 必须在 queue latch 下执行：扫描队列顺序，只要遇到无法越过的等待请求就停止；对可同时授予的一组请求，检查它们与所有 granted 请求及本批已选请求兼容，然后置 `granted=true`。状态变化后 `notify_all`。

#### Upgrade

```text
UPGRADE(queue,existing,new_mode):
  if not CanLockUpgrade(existing.mode,new_mode): abort INCOMPATIBLE_UPGRADE
  if queue.upgrading != INVALID and != txn.id: abort UPGRADE_CONFLICT
  queue.upgrading=txn.id
  old_mode=existing.mode
  remove old mode from txn bookkeeping at正确时机
  mark/replace request as waiting new_mode and place before normal waiters
  GRANT_NEW_LOCKS(queue)
  wait until granted or txn aborted
  queue.upgrading=INVALID
  if aborted:
    erase/delete request; notify; return false
  replace bookkeeping old_mode → new_mode
  notify_all; return true
```

升级过程中旧锁是否继续作为 granted blocker必须与队列设计一致；关键是不出现“其他事务趁升级空窗获得冲突锁”，也不让升级请求等待自己。建议单独写升级 grant 判断，而不是盲用普通请求逻辑。

#### Unlock

```text
UNLOCK_TABLE(txn,oid):
  if txn any shared/exclusive row lock set contains rows of oid:
    abort TABLE_UNLOCKED_BEFORE_UNLOCKING_ROWS
  queue=find table queue; lock queue
  req=find granted request of txn; absent → abort ATTEMPTED_UNLOCK_BUT_NO_LOCK_HELD
  old_mode=req.mode
  erase/delete req; REMOVE_LOCK_SET
  UPDATE_TXN_STATE_ON_UNLOCK(txn,old_mode,force=false)
  GRANT_NEW_LOCKS(queue); notify_all; return true

UNLOCK_ROW(txn,oid,rid,force):
  queue=find row queue; lock queue
  req=find granted txn request; absent → abort corresponding reason
  old_mode=req.mode
  erase/delete; REMOVE_LOCK_SET
  if not force: UPDATE_TXN_STATE_ON_UNLOCK(txn,old_mode)
  GRANT_NEW_LOCKS; notify_all; return true
```


### Task 2：Deadlock Detection

周期性从锁等待队列构建 waits-for graph：等待事务指向阻塞它的已授予事务（以及因队列顺序阻塞它的请求）。检测环后终止环中事务 ID 最大者，清图并重复，直到无环。

```text
BUILD_GRAPH: 对每个等待请求，添加 waiter → blocker 边
HAS_CYCLE: DFS(color) 或 Kahn；发现环时确定 victim=max(txn_id in cycle)
DETECT_LOOP: sleep interval → build → while cycle: victim.Abort；唤醒相关队列；重建图
```

图接口自身也要线程安全；遍历邻接表时保持确定顺序便于稳定测试。

#### Task 2：确定性的 waits-for graph

图可以用 `unordered_map<txn_id_t, set/vector>` 存，但 `HasCycle` 必须按 txn id 升序选择起点和邻居。`AddEdge` 幂等，`RemoveEdge` 对不存在边无操作，`GetEdgeList` 最好排序后返回以便测试稳定。

DFS 需要区分“本次递归栈”和“已经完整搜索”：

```text
DFS(u):
  state[u]=GRAY; path.push(u)
  for v in sorted(adj[u]):
    if state[v]==WHITE and DFS(v): return true
    if state[v]==GRAY:
      cycle = path suffix starting at v
      victim = max(cycle)
      return true
  path.pop; state[u]=BLACK; return false

HAS_CYCLE:
  for u in sorted(all_nodes):
    if WHITE and DFS(u): output victim; return true
  return false
```

每轮检测必须从当前 lock queues 全量重建图，不在多轮间增量维护。对于每个未授予请求，向所有实际阻塞它的未中止事务加边；既考虑不兼容的 granted holder，也考虑因 FIFO/升级优先级挡在前面的 waiter。一个 waiter 可能产生多条边。

```text
RUN_CYCLE_DETECTION:
  while enabled:
    sleep interval
    clear graph
    snapshot/lock each request queue and build edges, skipping aborted txns
    while HasCycle(victim):
      txn_manager.GetTransaction(victim).state = ABORTED
      remove/ignore victim node and rebuild or remove its edges
    notify_all request queues so aborted waiters can退出
```

注意“youngest”在该项目中即 cycle 内最大 txn id，而不是全图最大 id。一次唤醒必须打破所有环。


#### Graph API 与后台检测

```text
ADD_EDGE(a,b): lock waits_for_latch; adjacency[a].insert(b)
REMOVE_EDGE(a,b): lock; if a exists erase b; erase empty adjacency entry if desired
GET_EDGE_LIST(): lock; flatten all pairs; sort lexicographically; return

HAS_CYCLE(out_victim):
  lock/snapshot graph
  collect and sort all source+destination nodes
  state all WHITE; path=[]
  DFS nodes ascending, neighbors ascending
  on back edge to GRAY node:
    cycle_nodes=path suffix from repeated node
    *out_victim=max(cycle_nodes); return true
  return false

BUILD_GRAPH_FROM_QUEUE(queue):
  for waiter in queue where not granted and txn not aborted:
    for holder in queue where granted and txn not aborted:
      if modes incompatible: AddEdge(waiter.txn,holder.txn)
    add any additional FIFO-blocker edges required by queue grant policy

RUN_CYCLE_DETECTION():
  loop while enabled:
    sleep interval
    clear graph
    lock/snapshot every table and row queue; BUILD_GRAPH_FROM_QUEUE
    while HasCycle(&victim):
      txn=txn_manager.GetTransaction(victim)
      if exists: txn.state=ABORTED
      remove victim from working graph or rebuild excluding aborted
    for every queue: queue.cv.notify_all()
```


### Task 3：Concurrent Query Execution

在 SeqScan、Insert、Delete 中按隔离级别获取表锁和 tuple 锁；失败时抛 `ExecutionException`。写操作记录 write set，供 `TransactionManager::Abort` 逆序撤销；Commit/Abort 最后释放所有锁。

```text
SEQ_SCAN: 获取 IS；访问 tuple 前按隔离级别获取 S；读取后在 READ_COMMITTED 下释放 S
INSERT: 获取 IX；插入 tuple；获取/记录 X 语义；追加 TableWriteRecord
DELETE: 获取 IX；目标 tuple 获取 X；标记删除；追加删除 WriteRecord
COMMIT: 应用提交语义 → 释放全部锁 → COMMITTED
ABORT: 逆序遍历 write set，撤销 insert/delete → 释放全部锁 → ABORTED
```

重点测试重复访问同一 tuple、锁升级、不同隔离级别、事务包含多条语句，以及异常路径是否完整释放资源。

#### Task 3：把锁接入 Executor

锁策略矩阵：

| 操作 | RR | RC | RU |
|---|---|---|---|
| 普通扫描表锁 | IS，持有到结束 | IS | 无需 S/IS |
| 普通扫描行锁 | S，持有到 commit/abort | S，读完释放 | 不加 S |
| 删除扫描 | IX + 行 X | IX + 行 X | IX + 行 X |
| 插入 | IX，插入时获得行 X | 同左 | 同左 |
| 写锁释放 | commit/abort | commit/abort | commit/abort |

SeqScan `Init` 使用 `MakeEagerIterator`，并依据 `ExecutorContext::IsDelete()` 决定 IS 还是 IX；RU 普通读可以不取表锁。Next 必须先记录 iterator 当前 RID，再按隔离级别锁 row，然后读取 meta/tuple。若 tuple deleted 或 predicate 不通过，对刚取得的锁按要求 force unlock；RC 对输出 tuple 的 S 锁也应及时正常 unlock；RR 保留 S 锁。

```text
SEQ_NEXT:
  while iterator not end:
    rid = iterator current RID; ++iterator
    desired = ctx.IsDelete ? X : READ_LOCK_FOR_ISOLATION
    if desired exists and txn does not already hold sufficient row lock:
      if not LockRow(...): throw ExecutionException
    (meta, tuple) = table.GetTuple(rid)
    if deleted or predicate false:
      if this call newly acquired S: UnlockRow(force=true)
      continue
    if RC and newly acquired S: UnlockRow(force=false)
    return tuple/rid
  return false
```

“是否由本次调用新取得”很重要：一个事务可能跨多条语句重复访问 tuple，不能把之前已有且按隔离级别需保留的锁误释放。

Insert `Init` 获取 IX。Next 调用支持 lock manager/txn/table id 的 `TableHeap::InsertTuple` 变体，使插入和行锁建立原子关联；成功后追加 INSERT 类型的 `TableWriteRecord`。Delete 不重复取锁，因为 delete-mode SeqScan 已获取 X，但必须逻辑删除并追加 DELETE write record。

TransactionManager：

```text
COMMIT(txn):
  // 本学期写入已在执行时生效
  ReleaseLocks(txn)
  txn.state = COMMITTED

ABORT(txn):
  for record in reverse(txn.table_write_set):
    if record is INSERT: table.ApplyDelete(inserted_rid)
    if record is DELETE: table.RollbackDelete(deleted_rid)
  clear write set
  ReleaseLocks(txn)
  txn.state = ABORTED
```

具体回滚 API 以 `TableHeap` 和 `TableWriteRecord` 当前定义为准。逆序撤销不可省略；同一事务对相关数据做多次写时，正序可能无法恢复原状态。释放锁时通常先 row 后 table，避免触发“表锁先于行锁释放”的异常。







#### P4 Executor 与 TransactionManager

```text
SEQ_SCAN_INIT():
  table_info=Catalog.GetTable
  deleting=exec_ctx.IsDelete()
  desired_table_lock = deleting ? IX : (isolation==RU ? NONE : IS)
  if desired and txn does not already hold sufficient lock:
    if not LockTable: throw ExecutionException
  iterator=table.MakeEagerIterator()

SEQ_SCAN_NEXT():
  while iterator not end:
    rid=iterator.GetRID; ++iterator
    desired_row = deleting ? X : (isolation==RU ? NONE : S)
    acquired_here=false
    if desired and txn lacks sufficient row lock:
      if not LockRow: throw ExecutionException
      acquired_here=true
    (meta,tuple)=table.GetTuple(rid)
    visible = not meta.is_deleted and optional pushed_predicate true
    if not visible:
      if acquired_here: UnlockRow(force=true)
      continue
    if isolation==RC and desired_row==S and acquired_here:
      UnlockRow(force=false)
    output tuple/rid; return true
  return false
```

```text
INSERT_INIT():
  child.Init
  ensure txn holds IX or stronger table lock; otherwise LockTable(IX)
INSERT_NEXT():
  consume child once
  for tuple:
    rid=table.InsertTuple(meta,tuple,lock_manager,txn,oid)
    if failed: throw ExecutionException
    txn.table_write_set.push(TableWriteRecord(rid,oid,INSERT,tuple,table))
  output count once

DELETE_NEXT():
  // child SeqScan in delete mode already holds X row locks
  for (tuple,rid) from child:
    old_meta=table.GetTupleMeta(rid)
    mark deleted
    txn.table_write_set.push(TableWriteRecord(rid,oid,DELETE,tuple,table))
  output count once

RELEASE_LOCKS(txn):
  copy all row lock sets; UnlockRow each (or manager internal cleanup path)
  copy all table lock sets; UnlockTable each

COMMIT(txn):
  txn.table_write_set.clear()
  RELEASE_LOCKS(txn)
  txn.state=COMMITTED

ABORT(txn):
  while write_set not empty, from back:
    record=pop_back
    if INSERT: undo inserted tuple using TableHeap API
    if DELETE: restore deletion metadata using TableHeap API
  RELEASE_LOCKS(txn)
  txn.state=ABORTED
```

Commit/Abort 的最终状态设置顺序需结合 `Unlock` 是否检查 ABORTED 状态：如果公开 Unlock 会拒绝 aborted txn，TransactionManager 应使用内部释放路径，或在释放后再设最终状态。以本仓库测试对状态的要求为准。

#### Project 4 验收顺序

1. 单事务合法/非法锁请求，逐个核对 AbortReason。
2. 两事务兼容、阻塞、FIFO、批量授予与每条升级路径。
3. 三种隔离级别的 GROWING/SHRINKING 转换和 `force` unlock。
4. 图 API：无环、自环、多个环、共享节点、确定 victim。
5. 后台死锁：等待线程被唤醒、一次清除多个环、无 aborted 节点残留。
6. 事务集成：commit 可见、abort 回滚、多语句事务、重复读取同 tuple。
7. `terrier-bench` 先短时运行，再运行官方 30 秒；超时用线程堆栈判断是 LM 等待还是 executor 漏锁。

以下伪代码按“几乎可以逐行翻译成实现，但省略 C++ 语法和具体 helper 名称”的粒度书写。`defer guard.Drop()` 表示所有成功和失败路径都必须释放资源。

## 测试路线

```sh
# P1
cmake --build build --target lru_k_replacer_test buffer_pool_manager_test page_guard_test

# P2
cmake --build build --target b_plus_tree_insert_test b_plus_tree_delete_test b_plus_tree_concurrent_test

# P3
cmake --build build --target sqllogictest
./build/bin/bustub-sqllogictest test/sql/p3.00-primer.slt --verbose

# P4
cmake --build build --target lock_manager_test txn_integration_test deadlock_detection_test
```

每个 Project 完成后再运行 `make check-tests`、对应的 `check-clang-tidy-pN` 和 Release 构建。公开测试通过不等于所有边界条件正确，尤其要自行补充空输入、重复操作、资源耗尽和并发交错测试。
