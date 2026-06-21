# Lock-Free Queue（无锁队列）

## 定义与作用
无锁队列使用 CPU 原子操作而非互斥锁实现多线程安全的队列操作，消除锁竞争带来的上下文切换和线程阻塞开销，解决高并发场景下锁成为性能瓶颈的问题。

## 核心原理
- 核心是 Compare-And-Swap（CAS）原子指令：原子地比较内存值与预期值，相等则写入新值并返回是否成功
- 入队/出队操作通过 CAS 循环（CAS Loop）更新头尾指针，失败则重试，无需线程挂起
- 解决 ABA 问题：使用版本号（Tagged Pointer），CAS 时同时比较指针和版本号，确保值被修改后再改回也能识别
- Disruptor 模式：环形缓冲区 + 序号栅栏，单生产者单消费者场景下甚至无需 CAS，普通读写即可

## 软硬件协同点
1. **CAS 硬件指令**：x86 的 `lock cmpxchg`、ARM 的 `ldrex/strex` 是无锁算法的硬件基础，`lock` 前缀锁住内存总线，确保操作原子性
2. **内存屏障（Memory Barrier）**：无锁算法需正确插入内存屏障（`mb`/`rmb`/`wmb`），防止 CPU 乱序执行导致的数据不一致；C++11 的 `std::memory_order` 封装了这些细节
3. **缓存一致性协议交互**：CAS 操作会导致 Cache Line 在多核间来回"弹跳"（Cache Line Bouncing），性能随核心数增加而下降；Disruptor 通过每个线程独立的序号变量缓解此问题
4. **TSX 硬件事务内存**：Intel TSX（Transactional Synchronization Extensions）可将多个 CAS 操作包装为硬件事务，进一步简化无锁算法并提升性能

## 典型应用场景
1. **LMAX Disruptor**：金融交易系统使用 Disruptor 无锁环形缓冲区实现线程间消息传递，单线程吞吐达 6 百万消息/秒，延迟稳定在微秒级
2. **Linux 内核 per-CPU 队列**：内核网络栈使用 per-CPU 无锁队列管理 sk_buff，每个 CPU 核独立入队，出队时批量窃取，网络栈吞吐量提升 2 倍
