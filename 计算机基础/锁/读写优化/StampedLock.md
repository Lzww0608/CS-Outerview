### 1. 什么是 StampedLock？(Definition)

`StampedLock` 是 Java 8 (`java.util.concurrent.locks`) 引入的一种用于替代 `ReentrantReadWriteLock` 的高性能锁。

它的核心设计思想是引入了一个 **Stamp（票据/版本号）** 的概念。
也就是，每次获取锁（无论是读锁还是写锁），都会返回一个 `long` 类型的 stamp。释放锁或转换锁模式时，必须携带这个 stamp 作为凭证。

它提供了三种访问模式，比传统的 RWLock 多了一种：

1.  **写锁（Writing）：** 独占锁，类似 RWLock 的写锁。
2.  **悲观读锁（Pessimistic Reading）：** 共享锁，类似 RWLock 的读锁。如果此时有写锁，读锁会被阻塞。
3.  **乐观读（Optimistic Reading）：** **这是核心创新点。** 它**不是**真正的锁。它仅返回一个 stamp，允许你在不阻塞写线程的情况下读取数据。读完后，你需要验证这个 stamp 是否依然有效（即在读取过程中是否有写线程修改了数据）。

**关键特征：**
*   **不可重入（Non-Reentrant）：** 与 `ReentrantReadWriteLock` 不同，StampedLock 是不可重入的。这意味着同一个线程不能两次获取同一个锁，否则会死锁。
*   **基于 CLH 队列**：底层依然使用了类似 AQS 的排队机制，但经过了专门优化。

---

### 2. 为什么需要 StampedLock？(Motivation)

既然 Java 已经有了 `ReentrantReadWriteLock` (RRWL)，为什么还要造轮子？

#### 1. 解决 RRWL 的性能瓶颈
RRWL 在高并发读的场景下，性能其实并不完美。
*   **CAS 竞争**：即使是读操作，RRWL 也需要通过 CAS (Compare-And-Swap) 修改锁的状态（比如增加读者计数）。在多核 CPU 上，这会导致大量的缓存一致性流量（Cache Coherence Traffic），限制了扩展性。
*   **写饥饿**：虽然 RRWL 支持公平模式，但公平模式吞吐量极低。非公平模式下，写线程容易饥饿。

#### 2. 引入“乐观读” (Optimistic Read)
这是 StampedLock 的杀手锏。它借鉴了我们刚才讨论的 **Seqlock（顺序锁）** 的思想。
*   在乐观读模式下，读取操作**完全不修改**锁的状态（不写内存，没有 CAS）。
*   这意味着：**读操作几乎是零开销的**，且**读操作不会阻塞写操作**。
*   这完美解决了“读多写少”场景下的性能天花板问题。

---

### 3. 代码实战与标准范式 (Code & Idioms)

StampedLock 的 API 使用起来比 `synchronized` 或 `ReentrantLock` 要复杂，它有一套严格的**标准范式**。

以下是一个经典的 `Point` 类实现，展示了如何使用乐观读：

```java
import java.util.concurrent.locks.StampedLock;

public class Point {
    private double x, y;
    private final StampedLock sl = new StampedLock();

    // 【写锁模式】：和普通互斥锁类似
    void move(double deltaX, double deltaY) {
        long stamp = sl.writeLock(); // 获取写锁
        try {
            x += deltaX;
            y += deltaY;
        } finally {
            sl.unlockWrite(stamp); // 释放写锁，必须传入 stamp
        }
    }

    // 【乐观读模式】：性能最高的路径
    double distanceFromOrigin() {
        // 1. 尝试获取乐观读 stamp (非阻塞，不加锁)
        long stamp = sl.tryOptimisticRead();
        
        // 2. 拷贝共享变量到方法栈 (局部变量)
        double currentX = x;
        double currentY = y;

        // 3. 验证 stamp 是否有效
        // 如果在拷贝过程中有写线程介入，validate 会返回 false
        if (!sl.validate(stamp)) {
            // 4. 验证失败，说明数据脏了。
            // 策略：升级为悲观读锁 (类似普通的 ReadLock)
            stamp = sl.readLock(); 
            try {
                currentX = x;
                currentY = y;
            } finally {
                // 释放悲观读锁
                sl.unlockRead(stamp);
            }
        }

        // 5. 使用局部变量进行计算
        return Math.sqrt(currentX * currentX + currentY * currentY);
    }
}
```

**代码解析：**
*   `tryOptimisticRead()`：非常快，仅读取一个 volatile 变量。
*   `validate(stamp)`：利用内存屏障（LoadBarrier），确保如果在读取 `x, y` 的过程中发生了写操作（写操作会更新 stamp），这里能检测出来。
*   **回退机制**：如果乐观读失败，代码自动切换到悲观读锁，保证了正确性。

---

### 4. StampedLock 的底层原理与内存语义

作为精通底层原理的开发者，我们需要关注其内存语义：

1.  **Stamp 的位操作**：`long` 类型的 stamp 实际上是一个位图。
    *   部分位用于标识锁的状态（读锁、写锁）。
    *   部分位用于版本计数（类似 Seqlock 的 sequence）。
2.  **内存屏障 (Memory Barriers)**：
    *   `tryOptimisticRead()` 并不像 `readLock()` 那样执行完整的 CAS。
    *   但在 `validate()` 中，它必须确保**Happens-Before** 关系。Java 利用 `Unsafe.loadFence()` 确保在 `validate` 之前读取的数据不会被重排序到 `validate` 之后。
3.  **WNode 队列**：底层维护了一个链表队列（基于 CLH 锁变种），用于管理等待的线程。

---

### 5. 应用场景与致命陷阱 (Pitfalls)

#### 应用场景
*   **读多写少，且读操作非常频繁**：如金融系统的行情快照、游戏服务器的玩家坐标读取。
*   **性能敏感**：当 `ReentrantReadWriteLock` 成为 Profiling 中的瓶颈时。

#### 致命陷阱（Top-tier 经验）

1.  **不可重入（Non-Reentrant）**：
    *   如果你在持有锁的代码块中再次调用获取锁的方法，线程会直接死锁。这是从 RRWL 迁移过来最容易踩的坑。

2.  **中断（Interrupt）是个大坑**：
    *   `StampedLock` 的 `writeLock()` 和 `readLock()` 方法对中断**不敏感**。
    *   如果一个线程在 `writeLock()` 上阻塞并被 `interrupt()`，它可能不会抛出异常，而是会导致 CPU 飙升（内部自旋逻辑问题，JDK 某些版本存在此行为）。
    *   **最佳实践**：如果需要处理中断，必须使用 `writeLockInterruptibly()` 或 `readLockInterruptibly()`。

3.  **条件变量（Condition）不支持**：
    *   StampedLock 不支持 `Condition`（如 `newCondition()`）。如果需要 `wait/notify` 机制，只能退回到 `ReentrantLock`。

### 总结

StampedLock 是 **RWLock 的进化版**，它吸取了 **Seqlock（乐观读）** 的精华。

*   **RWLock**: 悲观策略，读写互斥，读读共享（但有 CAS 开销）。
*   **Seqlock**: 激进策略，写者无敌，读者重试。
*   **StampedLock**: 混合策略。先尝试乐观读（像 Seqlock），失败了再退化成悲观读（像 RWLock）。

在高性能的 Java 中间件开发中，StampedLock 是优化读吞吐量的首选工具。