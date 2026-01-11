简单来说，Green Tea GC 是为了解决**“内存墙（Memory Wall）”**问题而生的，它将GC的核心视角从“对象”转变为“内存页”。

以下是深层原理解析及改进对比：

### 1. 核心理念：从“对象中心”到“页中心”

这是 Green Tea GC 最本质的区别。

*   **之前的 GC (Go 1.5 - 1.25 default): Object-Centric (以对象为中心)**
    *   **工作方式**：经典的并发三色标记法（Tri-color Marking）。GC 像一个爬虫，沿着指针图（Object Graph）一个接一个地“爬行”。
    *   **问题**：这种“图漫游（Graph Flood）”是**随机内存访问**。指针指向哪里，CPU就得去哪里读取。在现代硬件上，CPU 运算速度远超内存读取速度。如果指针指向的内存不在 CPU 缓存（L1/L2/L3）中，CPU 就会发生大量的 **Cache Miss** 和 **Stall（停顿）**，空转等待数据从主存加载。
    *   **痛点**：在多核大内存机器上，GC 消耗的 CPU 周期中，有超过 **35%** 实际上是在单纯等待内存响应，而不是在做标记逻辑。

*   **Green Tea GC (Go 1.26): Page/Span-Centric (以页/跨度为中心)**
    *   **工作方式**：GC 不再急着处理单个对象，而是以 **8KB 的 Span（跨度/页）** 为单位进行调度。
    *   **机制**：当 GC 发现一个对象是活的，它不会立刻去扫描该对象引用的下游对象，而是将该对象所在的 **Span** 标记为“含待处理工作”。GC 会累积工作，然后**成批地、顺序地**扫描整个 Span 中的所有活跃对象。
    *   **优势**：这种方式强制实现了**空间局部性（Spatial Locality）**。CPU 可以连续读取一段连续的内存，极大提高了缓存命中率，大幅减少了 CPU Stall。

### 2. 具体改进点 (Improvements)

相比于之前的 GC，Green Tea GC 在 Go 1.26 带来了以下显著改进：

#### A. 吞吐量提升与 CPU 消耗降低 (Throughput & Efficiency)
*   **改进**：在 GC 密集型（尤其是小对象多、指针多）的负载下，Green Tea GC 能减少 **10% ~ 40%** 的 GC CPU 开销。
*   **原理**：减少了 CPU 等待内存的时间（Memory Latency），同样的标记任务，CPU 能更快完成。这意味着留给业务代码（Mutator）的 CPU 时间片更多了，整体 QPS 会上升。

#### B. 引入 SIMD 向量化加速 (SIMD Acceleration)
*   **改进**：Go 1.26 利用 Green Tea 的架构，引入了 SIMD（单指令多数据）指令集优化（如 AVX-512 或 NEON）。
*   **原理**：因为 Green Tea 是按 Span 批量扫描的，内存布局更加规整。这使得编译器和 Runtime 可以生成向量化代码，一次指令就能扫描或标记多个指针/位图，而旧的随机访问模式很难利用 SIMD。

#### C. 更好的多核扩展性 (Scalability)
*   **改进**：在 64 核、128 核等超多核服务器上，性能衰减更小。
*   **原理**：旧 GC 在分发“对象”任务时，工作窃取（Work Stealing）队列的锁竞争和同步开销较大。Green Tea 以“页”为单位分发任务，粒度更粗，同步开销更低，更适合 NUMA（非一致性内存访问）架构。

### 3. 代码与应用层面的影响

虽然这是 Runtime 底层的改动，但对我们编写代码和调优有深远影响。

**代码演示（概念）：**

假设我们有一个典型的链表结构，这种结构在旧 GC 下是“缓存杀手”：

```go
// 典型的链表节点，在堆内存中可能物理分布非常离散
type Node struct {
    Value int64
    Next  *Node
}

// 构建一个巨大的链表
func buildList(n int) *Node {
    head := &Node{}
    current := head
    for i := 0; i < n; i++ {
        // 每次 new 可能会分配在堆的不同位置
        current.Next = &Node{Value: int64(i)}
        current = current.Next
    }
    return head
}
```

*   **旧 GC**：扫描 `head -> next -> next` 时，每次跳转都可能跳到内存的随机位置，导致 CPU L1/L2 Cache 失效。
*   **Green Tea GC**：它不会傻傻地顺着链表跳。它会识别出这些 Node 分布在哪些 Span 上。比如 Span A 里有 50 个 Node，Span B 里有 30 个。它会先加载 Span A 到缓存，一口气处理完这 50 个 Node 的标记，再换 Span B。**用“批处理”战胜了“随机跳跃”。**

需要注意的是，虽然它是默认开启的，但在极少数对象分布极度稀疏的场景下可能会有轻微回退，Go 1.26 依然提供了 GODEBUG 或 GOEXPERIMENT=nogreenteagc 来进行逃生通道回滚。