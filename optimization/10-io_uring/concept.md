# io_uring

## 定义与作用
io_uring 是 Linux 5.1 引入的新一代异步 IO 框架，通过共享内存环形队列消除系统调用开销，支持所有类型的文件和网络 IO，解决传统 AIO 功能受限和 epoll 高并发下的系统调用风暴问题。

## 核心原理
- 两个无锁环形队列（SQ 提交队列，CQ 完成队列）在用户态与内核之间共享，无需内存拷贝
- 用户将 IO 请求放入 SQ，内核消费 SQ 执行异步 IO，完成后将结果放入 CQ
- 支持轮询模式（IOPOLL），内核主动轮询设备完成状态，完全避免中断开销；零拷贝模式（IORING_SETUP_SQPOLL）内核线程自动轮询 SQ，连系统调用都省了

## 软硬件协同点
1. **内存序与原子操作**：环形队列的生产者-消费者同步依赖 CPU 的内存屏障指令（如 x86 的 `lfence`/`sfence`）和 `atomic_t` 原子操作，确保多核下的正确性
2. **NVMe 原生异步接口**：io_uring 与 NVMe 的异步队列天然对齐，SQ 直接映射到 NVMe 提交队列，IO 路径从系统调用 → 块层 → NVMe 缩短为两层
3. **网卡 IO 合并**：网络 IO 场景下，io_uring 可将多个 send/recv 请求批量提交，网卡支持 TCP Segmentation Offload（TSO）时，内核只需提交一个大缓冲区，分片由硬件完成
4. **File-backed mmap 配合**：io_uring 支持对 mmap 区域的异步 write，DMA 直接从用户映射的页面取数据，Page Cache 和 Direct I/O 模式都适用

## 典型应用场景
1. **高性能网络服务器**：Rust 的 `tokio-uring` 运行时使用 io_uring 处理网络 IO，Redis 基准测试中 QPS 比 epoll 版本高 40%，延迟降低 30%
2. **高性能存储引擎**：Facebook 的 CacheLib 使用 io_uring 实现异步缓存回写，SSD 随机写 IOPS 从 1 万提升到 8 万，队列深度利用率达 95%
