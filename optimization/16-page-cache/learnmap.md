## 1. 🧭 核心定位与价值
- **一句话本质**：Page Cache 是内核用内存为磁盘文件做的"透明代理缓存"，将随机磁盘 IO 转化为内存访问，把 10ms 级延迟压到 100ns 级。
- **软硬件协同点**：DMA 控制器直接填充物理页面（零 CPU 拷贝），MMU 缺页异常驱动按需加载，IO 调度器按 LBA 排序批量回写，三者共同构成"内存做缓存、磁盘做持久化"的分层存储体系。

---

## 2. 🌳 前置知识树 (Prerequisites)

- **虚拟内存与页表机制**：理解 VA→PA 翻译、Page Fault 异常处理流程、缺页异常的种类（minor/major）——这是 Page Cache 介入应用访问的根本入口
- **块设备与 IO 栈**：理解 BIO 结构、IO 调度器（noop/deadline/cfq）、VFS 与文件系统的层次关系——Page Cache 夹在 VFS 和块设备层之间
- **内存回收与 LRU**：理解物理页面的生命周期（buddy allocator → page struct → 回收）、Zone 与 Watermark 机制——Page Cache 页面的回收路径与普通匿名页有本质区别

---

## 3. 🗺️ 进阶学习路径 (Learning Path)

### 阶段一：机制理解 (What & How)

**读路径 & 写路径**
- 读：`sys_read()` → `vfs_read()` → `generic_file_read_iter()` → `find_get_page()` 查 XArray → 命中则 `copy_page_to_iter()`；未命中则 `page_cache_sync_readahead()` / `page_cache_async_readahead()` 触发磁盘 IO
- 写：`sys_write()` → `vfs_write()` → `generic_perform_write()` → `grab_cache_page()` 分配/查找缓存页 → `copy_page_from_iter()` 写入 → `set_page_dirty()` 标记脏页

**核心数据结构**
- `struct page`：每个物理页的描述符，`flags` 字段的 `PG_dirty`、`PG_locked`、`PG_uptodate` 位是 Page Cache 状态机的核心
- `struct address_space`：每个 inode 关联一个 address_space，内嵌 `struct xarray i_pages`（旧版为 `struct radix_tree_root page_tree`），以文件 offset 为索引管理所有缓存页
- `XArray`：Linux 5.x 取代 Radix Tree 的新结构，支持 lockless 查找（`xa_load()`），配合 `page->_refcount` 保证并发安全

**预读机制 (Read Ahead)**
- 内核预读不是"读一页"，而是一次读一串（`readahead_size`，通常为初始 128KB，可动态增长到 2MB）
- 状态机三态：`readahead`（正在预读）、`readahead + async`（异步预读中）、无标记（缓存命中）
- 关键函数：`ondemand_readahead()` 是入口，`get_init_ra_size()` / `get_next_ra_size()` 计算预读窗口大小

### 阶段二：性能剖析 (Why Fast)

**命中路径的极致优化**
- 快路径（fast path）只有几十条指令：XArray 查找 → page 引用计数 +1 → `copy_to_user()`，全程无锁（XArray 的 RCU 查找）
- 慢路径（slow path）触发 Page Fault：进入 `do_page_fault()` → `handle_mm_fault()` → `filemap_fault()` → 查找/分配/IO，开销 1000 倍以上
- **命中即正义**：Page Cache 的核心性能公式是 `平均延迟 = 命中率 × 内存延迟 + (1-命中率) × 磁盘延迟`，命中率从 95% 升到 99%，平均延迟下降 5 倍

**写回的批处理效应**
- 脏页不是立即写盘，而是积攒到阈值（`dirty_ratio` 默认 20% 系统内存 / `dirty_background_ratio` 默认 10%）后批量回写
- `pdflush`（旧）/ `flush_work`（新）按 `inode` 组织脏页，通过 `write_cache_pages()` 提交 BIO，IO 调度器再按 LBA 排序合并
- 顺序写场景下，Page Cache 将"每次 write 触发一次磁盘 IO"变成"攒满一批再写"，吞吐提升 10-100 倍

**零拷贝场景的放大器**
- `sendfile()`：数据从 Page Cache 直接送到网卡，不经过用户态，省一次 `copy_to_user()` + `copy_from_user()`
- `splice()`：在两个文件描述符之间搬数据，全程在内核 Page Cache 内完成指针搬运
- `mmap()`：将 Page Cache 直接映射到用户态虚拟地址空间，读写都走 CPU 正常访存指令，彻底消除 read/write 系统调用开销

### 阶段三：局限与妥协 (Trade-offs)

**内存压力下的回收抖动**
- 全局回收（`kswapd` / `direct reclaim`）不区分"有价值的热页"和"一次性冷页"，大文件顺序读可能把工作集全部挤出
- Two-Queue LRU（`active_list` / `inactive_list`）试图缓解：第一次访问进 inactive，第二次访问才升 active；但对"扫描一次的大文件"仍然无效
- **cgroup v2 的 memory.reclaim** 可做精细控制，但默认配置下 Page Cache 是"用完即扔的内存海绵"，稳定性无保证

**脏页回写的延迟尖刺**
- `dirty_ratio` 阈值触发时，所有写进程会被阻塞在 `balance_dirty_pages()` 里同步回写，造成毫秒级甚至百毫秒级延迟抖动
- 机械盘上 IO 调度器的回写可能和应用读请求抢带宽，读延迟突然飙升
- 解决手段：调低 `dirty_background_ratio` 让后台回写提前启动、用 `io_uring` 异步写、或直接上 `O_DIRECT` 自己管

**O_DIRECT 与 Page Cache 的二选一**
- 绕过 Page Cache 意味着失去预读、回写合并、缓存复用三大福利，但获得了：可预测的延迟、不污染缓存、零拷贝（配合 `O_DIRECT` + IO 调度）
- 数据库（MySQL/PostgreSQL）几乎都默认用 `O_DIRECT` 读 + 自己的 Buffer Pool，因为数据库比内核更懂数据访问模式
- **折中方案**：`madvise(MADV_DONTNEED)` 用 Page Cache 但用完即丢，避免缓存污染；`posix_fadvise(POSIX_FADV_WILLNEED)` 提前预取

**内存耗尽的 OOM 风险**
- Page Cache 理论上"可回收"，但如果脏页回写速度赶不上分配速度，`direct reclaim` 会阻塞所有分配路径，系统看似有内存但全部卡住
- `min_free_kbytes` 是底线，`watermark[WMARK_MIN/LOW/HIGH]` 控制回收触发时机，调小了容易 OOM，调大了浪费内存

---

## 4. 🛠️ 实验与调试指南 (Hands-on & Profiling)

### 观测工具

- **`vmtouch`**：直接查看某个文件在 Page Cache 中占了多少页、命中率多少，也可手动把文件打入/踢出缓存
  ```bash
  vmtouch /path/to/file          # 查看缓存状态
  vmtouch -t /path/to/file       # 把文件加载进 Page Cache
  vmtouch -e /path/to/file       # 把文件从 Page Cache 驱逐
  ```
- **`pcstat`**：Go 语言实现的 Page Cache 统计工具，按文件粒度查看缓存占比，适合排查"某数据库文件到底缓存了多少"
- **`perf trace -e filemap:*`**：跟踪内核 `filemap` 系列 tracepoint，精确观测 `mm_filemap_add_to_page_cache`、`mm_pagecache_alloc`、`mm_filemap_delete_from_page_cache` 等事件的频率与延迟

### 关键指标

| 指标 | 含义 | 观测命令 |
|---|---|---|
| **Cached** | 系统 Page Cache 总大小 | `free -h` / `/proc/meminfo` |
| **Dirty** | 待回写脏页字节数 | `grep Dirty /proc/meminfo` |
| **Writeback** | 正在回写的脏页字节数 | `grep Writeback /proc/meminfo` |
| **pgpgin / pgpgout** | 磁盘换入换出的页面数（累计） | `vmstat 1` / `/proc/vmstat` |
| **pgfault / pgmajfault** | 缺页异常总次数 / 主缺页（需磁盘 IO）次数 | `ps -o min_flt,maj_flt -p <pid>` |
| **reclaim 事件** | 直接回收触发频率与耗时 | `perf record -e vmscan:mm_vmscan_direct_reclaim_begin -g` |

**调优参数**（`/proc/sys/vm/`）
- `dirty_background_ratio`：脏页占内存比例达到此值时，`pdflush` 开始后台回写（默认 10）
- `dirty_ratio`：脏页占比达到此值时，写进程同步阻塞回写（默认 20）
- `vfs_cache_pressure`：控制 inode/dentry 缓存回收倾向（默认 100，值越大越积极回收）
- `min_free_kbytes`：系统保留的最低空闲内存水位

---

## 5. 📚 推荐阅读与扩展 (Resources)

### 源码级指引

- **核心文件**：`mm/filemap.c` —— Page Cache 读写主逻辑（`filemap_read()`、`filemap_write()`、`filemap_fault()`）
- **数据结构**：`include/linux/pagemap.h`（address_space 定义）、`lib/xarray.c`（XArray 实现）
- **回写机制**：`mm/page-writeback.c`（脏页跟踪与回写决策）、`fs/fs-writeback.c`（回写执行逻辑）
- **预读算法**：`mm/readahead.c` —— `ondemand_readahead()` 是理解预读状态机的入口
- **回收机制**：`mm/vmscan.c` —— `shrink_page_list()` 是 LRU 回收的核心，关注 Page Cache 页与匿名页的回收差异

### 关联技术

- **Buffer Cache**：历史上 Buffer Cache 缓存块设备元数据（如 inode、superblock），现代 Linux 已与 Page Cache 融合，但 `struct buffer_head` 仍在 `ext2/ext3` 等老文件系统中使用
- **Transparent Huge Pages (THP)**：Page Cache 本身以 4KB 页为单位，但 `fs/xfs` 等文件系统已支持大页缓存（`page_size=2MB`），配合 hugetlbfs 可大幅降低 TLB Miss
- **`io_uring`**：新一代异步 IO 框架，可与 Page Cache 协同（默认 buffered IO），也可 `IORING_SETUP_IOPOLL` + `O_DIRECT` 走轮询模式，延迟更低
