# Direct I/O（直接 IO）

## 定义与作用
Direct I/O 绕过操作系统 Page Cache，让应用直接与磁盘设备交互，解决 Page Cache 引入的额外拷贝开销和不可预测的回写延迟问题。

## 核心原理
- 打开文件时指定 O_DIRECT 标志，内核将 IO 请求直接提交给块设备层，跳过 Page Cache
- 数据直接在用户缓冲区与磁盘之间通过 DMA 传输，减少一次内核态到用户态的拷贝
- 代价是失去 Page Cache 的缓存优势，需要应用自己实现缓存层和 IO 调度

## 软硬件协同点
1. **内存对齐硬约束**：Direct I/O 要求用户缓冲区地址、文件偏移、传输长度都必须按 512 字节（传统扇区）或 4KB（高级格式化扇区）对齐，否则返回 EINVAL 错误
2. **DMA 直接传输**：磁盘控制器的 DMA 引擎直接访问用户空间内存，无需 CPU 参与数据搬运，这是 Direct I/O 零拷贝的硬件基础
3. **IO 调度器交互**：绕过 Page Cache 意味着内核无法进行 IO 合并和排序；应用需自己批量提交 IO 请求，或使用 `libaio` + `io_setup` 让内核调度器合并
4. **NVMe 多队列优化**：现代 NVMe SSD 支持 64K 个 IO 队列，Direct I/O 配合 `io_uring` 可将请求直接提交到硬件队列，绕过内核通用块层的额外开销

## 典型应用场景
1. **数据库数据文件读写**：MySQL InnoDB 使用 O_DIRECT 读写数据文件（但 WAL 仍使用 Page Cache），避免双缓存（Double Buffering）问题，内存利用率提升 30%
2. **分布式存储系统**：Ceph OSD 使用 Direct I/O 读写底层块设备，自己实现的 Object Store 缓存层比通用 Page Cache 更了解数据访问模式，命中率提升 15%
