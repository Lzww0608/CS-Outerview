# Page Cache（页缓存）

## 定义与作用
页缓存是操作系统内核将磁盘文件内容缓存到内存中的机制，将磁盘 IO 转化为内存访问，解决磁盘与内存之间 1000 倍以上的速度差距问题。

## 核心原理
- Page Cache 以页（通常 4KB）为单位管理，使用 Radix Tree 或 XArray 快速查找文件偏移对应的缓存页
- 读操作：先查 Page Cache，命中直接返回，未命中则触发缺页异常，从磁盘加载页面到缓存
- 写操作：先写入 Page Cache 标记为脏页（Dirty Page），由 pdflush 线程异步回写磁盘；也可通过 fsync/msync 强制同步
- 使用 LRU（最近最少使用）或其变种（如 Two-Queue LRU）管理缓存淘汰，保护热点数据

## 软硬件协同点
1. **DMA 直接填充**：磁盘控制器通过 DMA 直接将数据写入 Page Cache 的物理页面，无需 CPU 参与数据拷贝，CPU 只处理中断
2. **MMU 缺页异常**：访问未缓存的页面时，CPU 触发 Page Fault，内核中断处理程序负责调度 IO 加载页面，整个过程对应用透明
3. **回写与 IO 调度**：pdflush 将脏页按磁盘 LBA 地址排序后批量写入，充分利用电梯算法减少磁盘寻道，顺序写比随机写快 100 倍
4. **直接 IO 旁路**：O_DIRECT 标志可绕过 Page Cache，应用自己管理缓存；此时 IO 直接提交给块设备，适合数据库等了解数据访问模式的场景

## 典型应用场景
1. **Linux 文件系统默认行为**：所有普通文件的 read/write 都经过 Page Cache，一个被充分缓存的 MySQL 数据库查询响应时间从 10ms 降到 100us
2. **KVM 虚拟机内存**：KVM 使用 Page Cache 缓存客户机磁盘镜像，配合 KSM（Kernel Same-page Merging）合并相同页面，物理内存利用率提升 30%
