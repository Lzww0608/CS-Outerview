### 1. 什么是 Per-CPU 变量？(Definition)

Per-CPU 变量，顾名思义，就是**“每个 CPU 核心都有一份独立副本”**的全局变量。

*   **普通全局变量**：所有 CPU 共享同一个内存地址。访问时需要加锁（Mutex/Spinlock）或使用原子操作，竞争激烈。
*   **Per-CPU 变量**：逻辑上是一个变量名，但物理上，系统为每个 CPU 核心（Core 0, Core 1, ...）都分配了独立的内存空间。
    *   当 CPU 0 访问该变量时，它读写的是属于 CPU 0 的那份副本。
    *   当 CPU 1 访问该变量时，它读写的是属于 CPU 1 的那份副本。

**核心优势**：
**完全无锁（Lock-Free）**。因为只有当前 CPU 能访问自己的副本（绝大多数情况下），所以根本不存在竞争！

---

### 2. 实现原理：从编译器到内核 (Under the Hood)

Per-CPU 变量的实现非常精妙，涉及链接器脚本、段寄存器和内核内存管理。

#### A. 静态定义 (Static Definition)
在 Linux 内核代码中，我们这样定义：
```c
DEFINE_PER_CPU(int, my_counter);
```

1.  **特殊的段（Section）**：编译器会将 `my_counter` 放到一个特殊的 ELF 段中（通常叫 `.data..percpu`）。
2.  **加载时复制**：
    *   系统启动时，内核会读取这个段的大小。
    *   内核会根据 CPU 的数量（比如 64 核），申请 64 块同样大小的内存区域（Per-CPU Area）。
    *   内核将 `.data..percpu` 段的内容初始化拷贝到这 64 块内存中。

#### B. 动态访问 (Dynamic Access)
当代码执行 `this_cpu_read(my_counter)` 时，发生了什么？

*   **基址 + 偏移 (Base + Offset)**：
    *   变量 `my_counter` 的地址，实际上只是一个**偏移量（Offset）**。
    *   每个 CPU 都有一个私有的**基地址（Per-CPU Base Address）**。
    *   **真实地址 = 当前 CPU 的基地址 + 变量的偏移量**。

*   **硬件加速 (x86 GS 寄存器)**：
    *   在 x86_64 架构下，Linux 内核利用 **GS 段寄存器** 来存储当前 CPU 的基地址。
    *   访问指令极其高效：`mov %gs:offset, %reg`。
    *   这意味着获取 Per-CPU 变量几乎没有额外开销，和访问局部变量一样快！

---

### 3. 应用场景 (Use Cases)

Per-CPU 变量适用于**“统计类”**或**“缓存类”**场景，特别是那些**写操作极高频，但聚合读取低频**的场景。

#### 1. 系统统计计数器 (Statistics)
这是最经典的应用。比如统计网络包数量、磁盘 I/O 次数、系统调用次数。
*   **传统做法**：`atomic_inc(&global_count)`。这会导致严重的缓存颠簸（Cache Bouncing）。
*   **Per-CPU 做法**：
    *   写入：每个 CPU 只更新自己的 `local_count`（无锁，极快）。
    *   读取：当用户运行 `top` 或 `ifconfig` 时，遍历所有 CPU，把它们的 `local_count` 加起来。
    *   *Trade-off*：写性能极大提升，读性能稍微下降（但读很少发生）。

#### 2. 本地缓存 (Local Caches)
内存分配器（Slab Allocator / Slub）是典型的例子。
*   每个 CPU 都有一个本地的小对象缓存池（Free List）。
*   申请内存时，先从当前 CPU 的缓存池里拿。
*   只有当本地池空了，才去全局池（加锁）申请。
*   这极大地减少了全局锁的竞争。

#### 3. 调度器运行队列 (Runqueues)
Linux 调度器为每个 CPU 维护一个独立的运行队列（Runqueue）。
*   CPU 0 只需要在自己的队列里找任务跑，不需要去抢 CPU 1 的任务。
*   只有在 CPU 0 忙死而 CPU 1 闲死的时候，才会触发**负载均衡（Load Balancing）**，跨 CPU 迁移任务（这时候才需要锁）。

---

### 4. 潜在陷阱 (Pitfalls)

虽然 Per-CPU 变量很强，但使用时必须极其小心：

1.  **禁止抢占 (Preemption Disable)**：
    *   访问 Per-CPU 变量时，必须确保当前线程**不会被迁移到其他 CPU 上**。
    *   如果线程在 CPU 0 上读了地址，还没写，就被调度到了 CPU 1 上，然后把数据写到了 CPU 1 的副本里，这就乱套了。
    *   *解决*：通常使用 `get_cpu_var()`（会自动禁止抢占）和 `put_cpu_var()`（开启抢占）。

2.  **伪共享 (False Sharing) 的变种**：
    *   虽然逻辑上隔离了，但如果两个 CPU 的 Per-CPU 内存区域在物理上靠得太近（处于同一个 Cache Line），依然会有伪共享问题。
    *   *解决*：内核会对 Per-CPU 区域进行对齐（Padding），确保每个 CPU 的数据独占 Cache Line。

### 总结

Per-CPU 变量是**空间换时间**的极致体现。

*   它通过让每个 CPU 玩自己的“私房钱”，消灭了 99% 的锁竞争。
*   它是 Linux 内核能够线性扩展到数千个 CPU 核心的基石技术之一。
*   对于高并发系统设计者来说，如果能把全局竞争的数据拆解为 Per-CPU（或 Per-Thread）的数据，往往能带来数量级的性能提升。