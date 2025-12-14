### 1. 什么是 CyclicBarrier？(Definition)

`CyclicBarrier`（循环栅栏）是 Java JUC (`java.util.concurrent`) 包提供的一个同步辅助类。
它允许一组线程互相等待，直到所有线程都到达某个公共的**屏障点 (Barrier Point)**。

**核心特性：**

1.  **全员到达 (All-or-None)**：只有当指定数量（N）的线程都调用了 `await()` 方法后，这些线程才会同时被唤醒，继续执行后续代码。
2.  **可循环使用 (Cyclic)**：与 `CountDownLatch` 是一次性的不同，`CyclicBarrier` 在释放等待线程后，会自动重置，可以重复使用。
3.  **回调任务 (Barrier Action)**：它支持一个可选的 `Runnable` 任务。当最后一个线程到达屏障时，这个任务会被执行（且由最后一个到达的线程执行），通常用于聚合结果。

---

### 2. CyclicBarrier vs CountDownLatch

这是面试中最容易混淆的点：

| 特性         | CountDownLatch (倒计时门闩)                    | CyclicBarrier (循环栅栏)                   |
| :----------- | :--------------------------------------------- | :----------------------------------------- |
| **核心逻辑** | **减法**：N 个线程扣动扳机，1 个线程等待起跑。 | **加法**：N 个线程互相等待，人齐了一起跑。 |
| **侧重点**   | 一个线程等待其他线程完成任务。                 | 多个线程互相等待，强调**步调一致**。       |
| **可重用性** | **一次性**。计数归零后无法重置。               | **可循环**。屏障打开后自动重置。           |
| **阻塞对象** | 调用 `await()` 的主线程阻塞。                  | 调用 `await()` 的所有工作线程都会阻塞。    |

---

### 3. 实现原理：ReentrantLock + Condition

`CyclicBarrier` 的底层并没有用什么黑魔法，它完全基于 Java 的 **ReentrantLock** 和 **Condition** 实现。

#### 核心状态
*   `parties`：总共需要等待的线程数（固定值）。
*   `count`：当前还剩多少个线程没到（递减）。
*   `generation`：当前的“代数”（用于支持循环重置）。

#### `await()` 的伪代码逻辑
```java
public int await() {
    lock.lock(); // 获取互斥锁
    try {
        // 1. 检查当前代是否损坏（比如有人中断了）
        if (generation.broken) throw new BrokenBarrierException();

        // 2. 计数器减一
        int index = --count;

        // 3. 如果 index == 0，说明我是最后一个到达的！
        if (index == 0) { 
            boolean ranAction = false;
            try {
                // 执行可选的回调任务
                if (barrierCommand != null) barrierCommand.run();
                ranAction = true;
                
                // 【关键一步】：唤醒所有等待的线程，并重置 barrier
                nextGeneration(); 
                return 0;
            } finally {
                if (!ranAction) breakBarrier();
            }
        }

        // 4. 如果 index > 0，说明人还没齐，我也得睡
        for (;;) {
            try {
                trip.await(); // 在 Condition 上挂起 (释放锁)
            } catch (InterruptedException ie) {
                // 处理中断逻辑...
            }
            // 被唤醒后，检查是不是人齐了(nextGeneration)，还是栅栏坏了
            if (g != generation) return index; // 换代了，说明上一波人齐了，走！
        }
    } finally {
        lock.unlock();
    }
}

void nextGeneration() {
    trip.signalAll(); // 唤醒所有在 Condition 上睡着的线程
    count = parties;  // 重置计数器
    generation = new Generation(); // 开启新的一代
}
```

**原理总结**：
就是一把锁 (`lock`) 保护一个计数器 (`count`)。线程来了就减 1，如果没减到 0 就去 `Condition` (`trip`) 上睡觉。最后一个减到 0 的人负责执行回调，然后喊醒 (`signalAll`) 所有人。

---

### 4. 应用场景 (Use Cases)

`CyclicBarrier` 适用于**多线程分治计算**或**多阶段任务同步**。

#### 1. 多线程计算 + 结果聚合 (MapReduce 简版)
*   **场景**：我们要统计全国 30 个省份的 GDP。
*   **实现**：
    *   启动 30 个线程，每个线程负责计算一个省的 GDP。
    *   设置 `CyclicBarrier(30, new AggregatorTask())`。
    *   每个线程算完后调用 `barrier.await()`。
    *   当第 30 个线程算完并 `await` 时，自动触发 `AggregatorTask` 把 30 个结果加起来。

#### 2. 多阶段并行任务 (Phased Processing)
*   **场景**：游戏加载。
    *   阶段 1：加载地图。
    *   阶段 2：加载模型。
    *   阶段 3：加载音效。
*   **实现**：
    *   所有线程并发加载地图 -> `await()` (等地图像素都加载完)。
    *   所有线程并发加载模型 -> `await()` (等模型都加载完)。
    *   所有线程并发加载音效 -> `await()` (游戏开始)。
    *   *因为 CyclicBarrier 可重用，同一个对象可以用到底。*

#### 3. 压力测试模拟
*   **场景**：模拟 1000 个用户**同时**并发请求某个 API。
*   **实现**：
    *   启动 1000 个线程，每个线程准备好请求参数后，先 `await()`。
    *   当第 1000 个线程就位，屏障打开，1000 个请求瞬间同时发出，制造最大并发压力。

### 总结

`CyclicBarrier` 是**“集结号”**。
它通过显式的**同步点**，让并发执行的线程在时间轴上对齐。

虽然在高性能无锁编程中（如 LMAX Disruptor）我们更倾向于用无锁队列来传递消息，但在传统的业务并发开发中，`CyclicBarrier` 依然是处理**分阶段并行任务**最优雅的工具。