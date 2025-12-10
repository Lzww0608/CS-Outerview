实际生产环境中（如 Linux 内核、Java JVM、数据库内核），使用的是它们的**高度改良变种**。这些变种通常融合了多种锁的优点，并针对特定场景（如 NUMA、虚假唤醒、内存回收）做了极致优化。

以下是工业界主流的自旋锁实现方案及其实例：

---

### 1. Linux 内核：qspinlock (Queued Spinlock)

这是目前 Linux 内核（x86 架构）中默认的自旋锁实现，它是自旋锁技术的集大成者。

*   **背景**：早期的 Linux 使用 Ticket Lock。但在云计算和超多核（100+ cores）时代，Ticket Lock 的 O(N) 缓存颠簸问题变得不可接受。于是 MCS 锁被引入，但原始 MCS 锁的结构体太大（需要 `next` 指针和 `locked` 标志，至少 16 字节），导致 `struct page` 等核心数据结构膨胀严重。
*   **解决方案**：**qspinlock**。
*   **核心原理**：
    *   **基于 MCS 的变种**：底层依然是 MCS 队列机制，保证 O(1) 的缓存失效。
    *   **极致压缩**：它将整个锁压缩在 **4 字节（32位）** 中！
        *   利用位域（Bitfields）存储 Tail 索引、Pending 位、Locked 位。
        *   **Per-CPU 数组**：它不再动态分配 MCS 节点，而是预先为每个 CPU 分配了固定的 MCS 节点数组（通常每 CPU 4 个，用于处理进程上下文、软中断、硬中断等嵌套场景）。
    *   **混合策略**：
        *   **无竞争**：直接 CAS 修改 Locked 位（类似 TAS），极快。
        *   **轻微竞争**：使用 Pending 位进行短时间的自旋（类似 Ticket Lock 的变种）。
        *   **严重竞争**：构建 MCS 队列，进入排队模式。
*   **地位**：这是目前工业界最顶级的自旋锁实现之一，兼顾了空间（4字节）和时间（MCS 的扩展性）。

---

### 2. Java JVM (HotSpot): ObjectMonitor (synchronized) & AQS

Java 的并发库极其成熟，它采用了**CLH 的变种**。

#### 2.1 `java.util.concurrent` (AQS)
Java 的 `ReentrantLock`、`CountDownLatch` 等基于 **AQS (AbstractQueuedSynchronizer)**。
*   **原型**：**CLH 队列锁**。
*   **改良点**：
    *   **显式链表**：原始 CLH 是隐式链表（只靠 `prev` 指针），AQS 维护了显式的 `head` 和 `tail`，以及双向链表（`prev` 和 `next`）。
    *   **阻塞而非自旋**：AQS 的节点在获取不到锁时，**不会一直死循环自旋**（那是浪费 CPU）。它会自旋很短一段时间（自适应），如果还拿不到，就调用 `LockSupport.park()` **挂起线程**。
    *   **状态管理**：节点中不仅存锁状态，还存线程引用、等待状态（SIGNAL, CANCELLED 等）。

#### 2.2 `synchronized` (ObjectMonitor)
*   **偏向锁 -> 轻量级锁 -> 重量级锁** 的膨胀机制。
*   **轻量级锁**：本质上就是一种 **自旋锁（CAS）**。
*   **自适应自旋 (Adaptive Spinning)**：JVM 会根据上一次在这个锁上的自旋成功率来决定这次转多久。如果上次转两下就拿到了，这次就多转会儿；如果上次转了很久没拿到，这次可能直接挂起，避免浪费 CPU。

---

### 3. 数据库内核（以 PostgreSQL 和 MySQL 为例）

数据库对锁的性能要求极高，且临界区通常很短。

#### 3.1 PostgreSQL: `s_lock`
*   PG 使用的是一种**带有指数回退（Exponential Backoff）的 TAS 锁**。
*   它没有使用复杂的 MCS/CLH，因为 PG 的架构是多进程模型，共享内存中的锁结构必须尽可能简单且鲁棒。
*   **策略**：
    1.  尝试 TAS。
    2.  失败则自旋几次。
    3.  还失败则 `sleep`（让出 CPU）。
    4.  如果长时间拿不到，会触发“随机延迟”，防止活锁。

#### 3.2 MySQL (InnoDB): `rw_lock`
*   InnoDB 的读写锁内部实现了自旋机制。
*   **自适应**：通过 `innodb_spin_wait_delay` 和 `innodb_sync_spin_loops` 参数控制。
*   它会先自旋一段时间，如果拿不到锁，再退化为操作系统互斥量（Mutex）挂起。

---

### 4. 现代 C++ 库：`folly::MicroSpinLock` (Facebook)

Facebook 的 Folly 库是高性能 C++ 的代表。

*   **MicroSpinLock**：
    *   **极小**：只占用 **1 个字节**（8 bit）。
    *   **原理**：基于 TAS，但利用了位操作。
    *   **场景**：用于对象头部压缩，或者对内存占用极度敏感的海量小对象锁。
*   **PicoSpinLock**：
    *   利用指针的低位（因为指针通常是 8 字节对齐的，低 3 位总是 0）来存储锁状态。**0 字节开销！**

---

### 5. 总结：工业界的趋势

工业界在选择自旋锁时，遵循以下 **"3S" 原则**：

1.  **Space (空间)**：
    *   原始 MCS/CLH 节点太大，现代实现倾向于压缩（如 Linux qspinlock 的 4 字节，Folly 的 1 字节）。
2.  **Scalability (扩展性)**：
    *   在多核场景下，必须避免全局热点。**MCS 队列机制**是解决严重竞争的终极方案（如 Linux qspinlock 的慢速路径）。
3.  **Smart (智能/自适应)**：
    *   **Hybrid（混合）是主流**。
    *   没人会傻傻地一直自旋（浪费电费）。
    *   **策略**：先 CAS 抢一下（乐观） -> 抢不到自旋一会儿（自适应） -> 竞争太激烈就排队（MCS/CLH） -> 实在不行就挂起（Mutex）。

**举例总结表：**

| 场景                | 使用的锁类型        | 核心特点                                           |
| :------------------ | :------------------ | :------------------------------------------------- |
| **Linux Kernel**    | **qspinlock**       | 4字节压缩，基于 MCS 队列，Per-CPU 节点，极致性能。 |
| **Java AQS**        | **CLH 变种**        | 双向链表，结合了自旋与线程挂起（Park）。           |
| **PostgreSQL**      | **TAS + Backoff**   | 简单健壮，适合多进程共享内存。                     |
| **Facebook Folly**  | **Micro/Pico Lock** | 1字节或0字节开销，极致内存优化。                   |
| **DPDK (网络开发)** | **Ticket/MCS**      | 用户态轮询驱动，不挂起，追求绝对的低延迟。         |
