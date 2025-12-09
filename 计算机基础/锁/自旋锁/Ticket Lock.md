### 1. 核心概念：银行排队叫号

Ticket Lock 的设计灵感完全来源于生活中的银行或餐厅排队叫号系统。

它不再是一群人围着一个门（锁变量）抢，谁抢到谁进；而是引入了**顺序**。

#### 核心结构
Ticket Lock 维护两个原子计数器：
1.  **Ticket（排队号）**：相当于取号机。每个新来的线程都要去领一个号，然后 `Ticket` 加 1。
2.  **Owner（叫号/服务号）**：相当于柜台上的显示屏。显示当前正在服务哪个号码。

#### 算法逻辑
1.  **Lock（取号）**：线程 A 原子地增加 `Ticket`，得到的旧值就是自己的号码（比如 10 号）。
2.  **Spin（等待）**：线程 A 不断读取 `Owner` 的值。如果 `Owner != 10`，说明还没轮到自己，继续自旋。
3.  **Enter（入场）**：当 `Owner` 变为 10 时，线程 A 获得锁，进入临界区。
4.  **Unlock（叫下一位）**：线程 A 离开时，原子地增加 `Owner`（变为 11）。此时，拿着 11 号的线程 B 发现屏幕变了，于是获得锁。

---

### 2. 代码实战 (C++)

为了体现底层细节，我们需要注意**内存序**和**伪共享（False Sharing）**的问题。

```cpp
#include <atomic>
#include <thread>
#include <immintrin.h>

class TicketLock {
private:
    // alignas(64) 是为了防止伪共享（False Sharing）。
    // 如果 ticket 和 owner 在同一个 Cache Line，
    // 对 ticket 的写操作会导致读取 owner 的核心缓存失效，造成不必要的流量。
    alignas(64) std::atomic<int> ticket{0};
    alignas(64) std::atomic<int> owner{0};

public:
    void lock() {
        // 1. 领号 (Fetch-and-Add)
        // memory_order_relaxed 足够了，因为我们只关心拿到唯一的号码
        int my_ticket = ticket.fetch_add(1, std::memory_order_relaxed);

        // 2. 查号 (Spin)
        // 只要 owner 不等于我的号，就一直转
        while (owner.load(std::memory_order_acquire) != my_ticket) {
            // 提示 CPU 正在自旋
            _mm_pause();
        }
    }

    void unlock() {
        // 3. 叫下一位
        // 只需要增加 owner，不需要返回旧值
        // memory_order_release 保证临界区操作不会重排到这一步之后
        owner.fetch_add(1, std::memory_order_release);
    }
};
```

---

### 3. 深度分析：优缺点与底层瓶颈

#### 优点
1.  **严格的公平性（Strict Fairness）**：这是 Ticket Lock 最大的卖点。它实现了 **FIFO（先进先出）**，彻底解决了线程饥饿问题。
2.  **代码逻辑简单**：比后续要讲的 MCS/CLH 锁更容易实现。
3.  **无原子操作竞争**：在 `lock` 过程中，只有“取号”这一步是原子 RMW（Read-Modify-Write）操作。之后的等待过程全是纯读（Read），不像 TAS 那样不断尝试 CAS。

#### 缺点（面试杀手锏）

面试官通常会问：“Ticket Lock 看起来很完美，为什么在超多核（Many-core）系统下性能依然不行？”

答案在于：**全局变量自旋导致的扩展性问题（Scalability Collapse）。**

让我们看看硬件层面发生了什么：

1.  **全局自旋**：假设有 100 个线程在排队。它们手里拿着 1 到 100 号。
2.  **单一热点**：这 100 个线程都在盯着**同一个变量** `owner` 看。
3.  **释放时的风暴**：
    *   当前持有锁的线程（0号）执行 `unlock`，修改 `owner` 从 0 变为 1。
    *   **缓存失效**：这一写操作，会导致其他 100 个核心缓存里的 `owner` 变量所在的 Cache Line 全部变为 **Invalid**。
    *   **全员刷新**：这 100 个核心几乎同时发现缓存失效，同时去内存/L3 缓存拉取最新的 `owner` 值。
    *   **然而**：其中 99 个线程读到 1 后，发现 `1 != my_ticket`，只能叹口气继续自旋。只有拿着 1 号票的那个线程成功进入。

**结论**：
虽然 Ticket Lock 避免了 TAS 的“争抢写”，但它依然存在**O(N) 的读取流量**。每当锁释放一次，所有等待的 N 个线程都要刷新缓存。随着核心数 N 的增加，总线流量线性增长，导致性能急剧下降。

---

### 4. 伪共享（False Sharing）隐患

在上面的代码中，我特意加了 `alignas(64)`。如果不加这个，会发生什么？

*   `ticket` 和 `owner` 都是 `int`（4字节），它们极大概率会被 CPU 放在**同一个 Cache Line**（通常 64 字节）里。
*   当新线程来 `lock` 时，它修改 `ticket`。
*   因为 `ticket` 和 `owner` 在一起，修改 `ticket` 会导致整个 Cache Line 失效。
*   那些正在自旋读取 `owner` 的线程，虽然 `owner` 值没变，但因为 Cache Line 失效了，它们也被迫重新去内存拉取数据。
*   **后果**：新线程进场（写 ticket）会干扰正在排队的线程（读 owner），造成无谓的性能损耗。

### 5. 总结与引出下一代

**Ticket Lock** 是自旋锁进化史上的一大步，它引入了**秩序**。

*   **适用场景**：线程数适中，对公平性有严格要求的场景。
*   **致命弱点**：所有线程都在**同一个全局变量**上自旋。这违背了多核扩展性的黄金法则——**“让不同的核心在不同的内存地址上自旋”**。

为了解决这个问题，我们需要让每个线程只关注自己私有的变量，或者只关注它“前一个”排队者的状态。这就是 **MCS Lock** 和 **CLH Lock**（基于链表的队列锁）的设计哲学。
