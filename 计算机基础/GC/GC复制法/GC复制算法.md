### 1. 核心原理：半区复制 (Semispace)

复制算法的核心思想非常“简单粗暴”：**它将可用内存按容量划分为大小相等的两块，每次只使用其中一块。**

*   **From-Space（当前使用区）**：对象在这里分配。
*   **To-Space（空闲区）**：保留备用。

**工作流程**：
1.  **分配**：新对象在 From-Space 中分配。
2.  **GC 触发**：当 From-Space 满了，触发 GC。
3.  **复制（Evacuation）**：
    *   从 GC Roots 出发，找到所有**存活**的对象。
    *   将这些存活对象**复制**到 To-Space 中（紧凑排列，像俄罗斯方块一样，没有空隙）。
4.  **修正引用**：将指向旧对象的指针，修正为指向 To-Space 中新对象的地址。
5.  **交换（Swap）**：
    *   清空 From-Space（实际上不需要物理清空，只需要指针重置）。
    *   From-Space 和 To-Space 身份互换。

---

### 2. 算法细节：Cheney 算法与 Forwarding Pointer

面试官，这里有个深层次的技术细节：**如何高效地遍历和复制？** 最经典的是 1970 年提出的 **Cheney 算法**，它使用**广度优先搜索（BFS）**。

#### 关键机制：
1.  **指针碰撞（Bump Pointer）**：
    在 To-Space 中分配内存非常快，不需要维护空闲链表，只需要移动一个指针（`free` 指针）。
2.  **转发指针（Forwarding Pointer）**：
    *   当一个对象被复制到 To-Space 后，我们必须在**原对象（From-Space）的旧地址处**留下一个标记（通常放在对象头），记录它被移动到了哪里。
    *   **作用**：如果有多个对象引用同一个对象 A，当 A 第一次被复制后，后续的引用可以通过这个“转发指针”找到 A 的新地址，避免重复复制。

---

### 3. 优缺点深度分析

#### 优点
1.  **彻底解决内存碎片**：
    *   这是复制算法最大的优势。复制后的对象在 To-Space 中是连续存放的。
    *   **收益**：分配新对象时，只需要移动栈顶指针，效率等同于 C++ 的栈分配，极快。
2.  **效率与存活对象成正比**：
    *   标记-清除算法的清除阶段取决于**堆的大小**（因为要扫描所有垃圾）。
    *   复制算法的开销只取决于**存活对象的数量**。如果大部分对象都是垃圾（"朝生夕死"），那么复制算法效率极高。

#### 缺点
1.  **内存利用率低**：
    *   这是致命伤。必须浪费 50% 的内存作为 To-Space 备用。这在内存昂贵的年代是不可接受的。
2.  **存活率高时效率低**：
    *   如果对象都活着（比如老年代对象），那么需要复制大量数据，且需要频繁修改指针，开销巨大。

---

### 4. C++ 代码模拟实现 (Semispace Allocator)

为了展示底层理解，我写一段代码模拟这个“半区复制”的内存布局和分配逻辑。

```cpp
#include <iostream>
#include <cstring>
#include <vector>

// 模拟一个简单的对象
struct Object {
    int id;
    int size;
    bool forwarded; // 是否已转发
    Object* forwardingPtr; // 转发指针
    // ... 实际对象数据
};

class CopyingGC {
    char* heap;
    size_t halfSize;
    
    char* fromSpace;
    char* toSpace;
    
    char* freePtr; // 当前分配指针 (Bump Pointer)
    char* endPtr;  // 当前空间的结束边界

public:
    CopyingGC(size_t size) {
        // 分配总内存，一分为二
        heap = new char[size];
        halfSize = size / 2;
        
        fromSpace = heap;
        toSpace = heap + halfSize;
        
        // 初始化指针
        freePtr = fromSpace;
        endPtr = fromSpace + halfSize;
        
        std::cout << "Heap initialized. Half size: " << halfSize << std::endl;
    }

    ~CopyingGC() {
        delete[] heap;
    }

    // 模拟内存分配 (Bump Pointer Allocation)
    void* allocate(size_t size) {
        if (freePtr + size > endPtr) {
            std::cout << "Allocation failed! Trigger GC (Copying)..." << std::endl;
            collect();
            
            // GC 后再次尝试分配
            if (freePtr + size > endPtr) {
                throw std::bad_alloc(); // 还是不够，OOM
            }
        }
        
        void* addr = freePtr;
        freePtr += size; // 指针碰撞，极快
        return addr;
    }

    // 模拟 GC 过程
    void collect() {
        std::cout << "--- GC Started: Copying from FromSpace to ToSpace ---" << std::endl;
        
        // 1. 重置 ToSpace 的分配指针
        char* scanPtr = toSpace; // 这里的 scanPtr 用于 BFS 扫描
        char* newFreePtr = toSpace;

        // 2. 模拟从 Roots 复制存活对象 (这里简化逻辑，假设有一个 rootObj 活着)
        // 现实中需要遍历栈和寄存器
        // copy(rootObj, &newFreePtr); 

        // 3. 交换空间 (Swap)
        std::swap(fromSpace, toSpace);
        
        // 更新分配指针
        freePtr = newFreePtr; 
        endPtr = fromSpace + halfSize; // 注意：fromSpace 已经是新的活动区域了
        
        // 清空旧空间（可选，Release模式下通常不物理清零，直接覆盖）
        // memset(toSpace, 0, halfSize); 
        
        std::cout << "--- GC Finished: Spaces Swapped ---" << std::endl;
    }
    
    // 实际的复制函数逻辑 (伪代码)
    /*
    Object* copy(Object* obj, char** allocPtr) {
        if (obj->forwarded) {
            return obj->forwardingPtr; // 已经复制过，直接返回新地址
        }
        
        // 在 ToSpace 分配空间
        Object* newObj = (Object*)(*allocPtr);
        *allocPtr += obj->size;
        
        // 内存拷贝
        memcpy(newObj, obj, obj->size);
        
        // 设置转发指针
        obj->forwarded = true;
        obj->forwardingPtr = newObj;
        
        return newObj;
    }
    */
};

int main() {
    CopyingGC gc(1024); // 1KB 堆，每个半区 512B

    // 模拟分配
    gc.allocate(100);
    gc.allocate(200);
    gc.allocate(300); // 这里应该触发 GC，因为 100+200+300 > 512

    return 0;
}
```

---

### 5. 进阶：现代语言中的演变 (JVM 的 Appel 式回收)

面试官，纯粹的“复制算法”浪费一半内存太奢侈了。现代商业虚拟机（如 HotSpot JVM）结合了**弱分代假说（Weak Generational Hypothesis）**——即“绝大多数对象都是朝生夕死的”，对算法进行了改良。

**JVM 的新生代（Young Gen）设计**：
*   **Eden 区**：一个较大的区域（比如占 80%）。
*   **Survivor 区（S0, S1）**：两个较小的区域（各占 10%）。

**改良版流程**：
1.  对象默认在 Eden 区分配。
2.  GC 时，扫描 Eden 和 S0。
3.  将存活对象复制到 S1（To-Space）。
4.  清空 Eden 和 S0。
5.  **关键点**：这里只浪费了 10% 的内存（S1），而不是 50%。因为根据统计，98% 的对象在 Minor GC 时都是垃圾，S1 通常足够装下存活的对象。

### 总结

*   **定义**：复制算法通过将内存一分为二，将存活对象搬运到另一边来回收内存。
*   **核心优势**：**无内存碎片**，分配速度极快（指针碰撞）。
*   **核心劣势**：内存利用率低（浪费一半）。
*   **最佳实践**：它不适合老年代（存活率高），但它是**新生代 GC 的绝对统治者**。
