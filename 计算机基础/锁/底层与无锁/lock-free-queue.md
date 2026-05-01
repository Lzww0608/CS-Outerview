# 无锁队列

无锁队列是指队列的入队、出队操作不依赖互斥锁保护整个临界区，而是使用原子操作、CAS、内存序和特定数据结构保证并发正确性。它的目标不是“没有任何等待”，而是避免线程阻塞和内核态睡眠，提高低冲突、短操作路径下的吞吐和尾延迟。

## 1. 无锁不等于无等待

并发算法常见进度保证：

| 概念 | 含义 | 典型特征 |
| --- | --- | --- |
| Blocking | 线程可能因为锁、条件变量、I/O 被挂起 | `mutex`、`condition_variable` |
| Lock-Free | 系统整体总有线程能推进 | 某个线程可能长期 CAS 失败 |
| Wait-Free | 每个线程都能在有限步内完成 | 实现复杂，成本通常更高 |
| Obstruction-Free | 没有竞争时能完成 | 有竞争时需要外部退避或协调 |

面试里说“无锁队列”通常指 lock-free queue，不一定是 wait-free queue。

## 2. 常见队列模型

设计前要先说明生产者和消费者数量，不同模型复杂度差异很大：

1. **SPSC**：单生产者、单消费者。可以用环形数组和两个原子下标，最简单、性能最好。
2. **MPSC**：多生产者、单消费者。入队侧需要原子竞争，出队侧较简单。
3. **SPMC**：单生产者、多消费者。出队侧需要原子竞争。
4. **MPMC**：多生产者、多消费者。最复杂，常用 Michael-Scott 链表队列或带序号的有界环形队列。

如果面试题没有说明模型，先反问或主动限定模型。生产环境里也要按真实读写模式选结构，不能拿 MPMC 的复杂度去解决 SPSC 问题。

## 3. SPSC 环形队列

SPSC 队列可以用固定大小数组、`head`、`tail` 实现：

```text
producer 只写 tail，只读 head
consumer 只写 head，只读 tail
```

关键点：

1. 容量通常取 2 的幂，用 `index & (capacity - 1)` 替代取模。
2. 用空一个槽区分满和空，或者用单调递增序号区分。
3. 生产者写入元素后，用 release 语义发布 tail。
4. 消费者 acquire 读取 tail 后，才能安全读取元素。
5. `head` 和 `tail` 要避免落在同一缓存行，减少伪共享。

简化示例：

```cpp
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

template <typename T, std::size_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be power of two");

public:
    bool push(const T& value) {
        auto tail = tail_.load(std::memory_order_relaxed);
        auto next = tail + 1;
        if (next - head_.load(std::memory_order_acquire) > N) {
            return false;
        }
        buffer_[tail & (N - 1)] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        auto head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        T value = buffer_[head & (N - 1)];
        head_.store(head + 1, std::memory_order_release);
        return value;
    }

private:
    std::array<T, N> buffer_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
```

这个版本适合 `T` 可平凡复制或赋值成本可控的场景。若 `T` 构造/析构复杂，要用未初始化存储、placement new 和显式析构管理对象生命周期。

## 4. MPMC 有界环形队列

MPMC 环形队列常用“每个槽位一个 sequence”的设计。核心思想是：数组槽位不只保存元素，还保存该槽位当前应该被哪个逻辑下标使用。

入队简化流程：

```text
读取 tail
找到 slot = buffer[tail % capacity]
检查 slot.sequence == tail，说明槽位可写
CAS tail -> tail + 1 抢占槽位
写入数据
slot.sequence = tail + 1 发布可读
```

出队简化流程：

```text
读取 head
找到 slot = buffer[head % capacity]
检查 slot.sequence == head + 1，说明槽位可读
CAS head -> head + 1 抢占槽位
读取数据
slot.sequence = head + capacity 发布可写
```

这种设计可以避免只靠 `head/tail` 判断满空时的 ABA 和覆盖问题。真实实现要处理内存序、对象生命周期、缓存行填充、失败退避和关闭语义。

## 5. Michael-Scott 链表队列

Michael-Scott Queue 是经典无界 MPMC lock-free queue。它使用一个哨兵节点，维护原子 `head` 和 `tail` 指针：

1. 入队时创建新节点，CAS 把旧 tail 的 `next` 从空改为新节点。
2. 如果链接成功，再尝试推进 `tail` 到新节点。
3. 出队时读取 `head`、`tail`、`head->next`。
4. 如果队列非空，CAS 推进 `head` 到 `next`，再返回旧 next 中的数据。
5. `tail` 落后时，其他线程可以帮忙推进 `tail`。

它的难点不在 CAS 本身，而在安全释放旧节点。出队成功后旧 head 不能立刻 `delete`，因为其他线程可能刚读到这个节点但还没完成校验。

常见内存回收方案：

1. Hazard Pointer：线程声明正在访问的节点，删除方延迟释放危险节点。
2. Epoch Based Reclamation：所有线程离开旧 epoch 后批量释放退休节点。
3. 引用计数：节点被访问时增加引用，复杂且原子开销较高。
4. 业务规避：固定节点池或进程退出统一释放，适合部分实时系统但不通用。

## 6. 如何平衡有锁和无锁

无锁不是默认更好。可以按下面维度取舍：

| 维度 | 优先有锁 | 可以考虑无锁 |
| --- | --- | --- |
| 临界区复杂度 | 多字段不变量、条件等待、异常路径多 | 单一入队/出队、状态简单 |
| 竞争程度 | 高竞争且会长时间自旋 | 低到中等竞争，操作很短 |
| 等待语义 | 需要阻塞等待、超时、取消、关闭 | 调用方可以重试、丢弃或退避 |
| 正确性风险 | 团队缺少无锁经验 | 有成熟库、充分压测和内存回收方案 |
| 资源使用 | CPU 紧张，阻塞更划算 | 尾延迟敏感，不能频繁线程切换 |

真实后端里常见选择：

1. 普通业务队列：`mutex + condition_variable` 或成熟并发队列，清晰可靠。
2. 日志、指标、音视频帧、网络包环形缓冲：SPSC/MPSC 无锁队列更常见。
3. 高性能交易、网关、存储引擎：可使用无锁或低锁结构，但通常配合 CPU 亲和性、内存池和严格压测。
4. 可丢弃任务：队列满时直接丢弃或降级，比无限自旋更稳定。

## 7. 易错点

1. CAS 成功只保证某个原子位置更新成功，不自动保证多个字段不变量正确。
2. `memory_order_relaxed` 不能用于发布已写入的队列元素，发布/消费通常需要 release/acquire。
3. 链表无锁队列必须解决节点安全回收，否则容易 use-after-free。
4. 高竞争下 CAS 自旋会造成缓存行反复失效，吞吐可能不如互斥锁。
5. 队列满/空、关闭、生产者退出、消费者阻塞等待这些工程语义经常比核心算法更容易出 bug。
6. 使用固定环形数组时要明确覆盖策略：阻塞、返回失败、丢弃旧数据还是丢弃新数据。

## 8. 面试回答模板

我会先确认队列模型：SPSC、MPSC 还是 MPMC。SPSC 可以用有界环形数组，生产者只推进 tail，消费者只推进 head，元素写入后用 release 发布，消费端用 acquire 读取，并把 head/tail 做缓存行对齐。MPMC 会复杂很多，可以用每槽 sequence 的有界队列，或者 Michael-Scott 链表队列。链表队列还必须解决 ABA 和内存回收，常见方案是 Hazard Pointer 或 Epoch。是否使用无锁要看临界区长度、竞争程度、是否需要阻塞等待和团队维护成本；普通业务队列用锁更稳，极致低延迟和简单数据通路才考虑无锁。
