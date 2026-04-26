### 1. 为什么需要一致性哈希？（解决了什么问题）

在理解一致性哈希之前，我们需要先看传统的**取模哈希（Modulo Hashing）**有什么问题。

假设我们有 *N* 台缓存服务器，对于一个 Key，我们通常用以下公式计算它应该存放在哪台机器上：
Index = hash(Key)% *N*

**这个方案在节点数量固定时非常完美，但当分布式系统需要扩容或缩容时，它就是灾难：**

*   **场景**：假设 *N=3*，现在增加一台机器变成 *N=4*。
*   **后果**：公式变成了 Index = hash(Key)% *N*。数学上可以证明，大约只有 *1/(N+1)* 的 Key 还能命中原来的机器，剩下的 ***N/(N+1)*（即 75% 以上）的 Key 会全部失效**。
*   **业务影响**：
    1.  **缓存雪崩（Cache Stampede）**：海量请求瞬间击穿缓存层，直接打到后端数据库，可能导致数据库宕机。
    2.  **数据迁移成本极高**：在分布式存储中，意味着几乎所有数据都要在节点间重新搬运。

**一致性哈希解决的核心问题就是：** **当节点数量发生变化（扩容/缩容/宕机）时，尽可能少地迁移数据，保证系统的稳定性（单调性）。**

---

### 2. 一致性哈希的核心原理

一致性哈希不再对服务器数量 *N* 取模，而是对 **2^32** 取模（即把哈希值空间组织成一个虚拟的圆环）。

#### 步骤一：哈希环（Hash Ring）
想象一个闭合的环，范围是 **[0, 2^32-1]**。

#### 步骤二：服务器映射
我们把服务器的 IP 或主机名进行哈希，映射到这个环上的具体位置。
*   Node A -> Hash(IP_A) -> 环上的点 A
*   Node B -> Hash(IP_B) -> 环上的点 B
*   Node C -> Hash(IP_C) -> 环上的点 C

#### 步骤三：数据映射与定位
对于任何一个数据 Key：
1.  计算 `Hash(Key)`，确定其在环上的位置。
2.  **顺时针查找**：从该位置出发，顺时针方向遇到的**第一台**服务器，就是该 Key 的存储节点。

#### 容错与扩展表现
*   **节点宕机**：假设 Node B 挂了。原本路由到 Node B 的数据，顺时针查找会遇到 Node C。**只有 Node B 和 Node A 之间的数据受影响**，其他数据（路由到 A 和 C 的）完全不变。
*   **节点扩容**：假设在 A 和 B 之间增加 Node D。原本路由到 B 的一部分数据（A 到 D 之间的）会拦截给 Node D。**只有新节点附近的一小部分数据需要迁移**。

---

### 3. 进阶优化：虚拟节点（Virtual Nodes）

基础的一致性哈希有一个致命缺陷：**数据倾斜（Data Skew）**。

如果节点太少（比如只有 2 台），或者哈希算法导致节点在环上分布不均匀（比如 A 和 B 挨得很近），那么大量的 Key 可能会集中涌向某一台服务器，导致负载不均衡。

**解决方案：引入虚拟节点。**

*   **原理**：不再将物理节点直接映射到环上，而是为每个物理节点创建多个“分身”（副本）。
*   **做法**：
    *   Node A 变成：`Node A#1`, `Node A#2`, ..., `Node A#100`
    *   Node B 变成：`Node B#1`, `Node B#2`, ..., `Node B#100`
*   **效果**：这 200 个虚拟节点随机散落在环上。Key 依然顺时针找虚拟节点，找到 `Node A#5`，实际上就是路由到物理节点 Node A。
*   **作用**：**让数据在物理节点间分布得更加均匀**。通常一个物理节点会对应 100-200 个虚拟节点。

#### 节点很少时怎么办

如果物理节点很少，即使使用一致性哈希，也容易出现数据倾斜和热点。常见优化：

1. **增加虚拟节点数量**：每个物理节点映射更多虚拟节点，让它们在环上更均匀地散开。
2. **加权虚拟节点**：机器配置不同或负载不同，可以给高性能节点更多虚拟节点，给低性能节点更少虚拟节点。
3. **引入 bounded load**：当目标节点负载超过平均值一定比例时，把部分 key 路由到后继节点，避免单点过载。
4. **换用 Rendezvous Hashing 或 Maglev Hashing**：在负载均衡场景中，它们能减少环维护成本，并提供较好的均匀性。
5. **业务侧打散热点 key**：对超级热点 key 做本地缓存、多副本读、key 拆分或请求合并。
6. **优先扩容真实节点**：虚拟节点只能改善分布均匀性，不能凭空增加总体 CPU、内存和带宽。

真实系统里通常会同时监控节点 QPS、内存、p99 延迟和 key 分布，发现倾斜后调整虚拟节点权重或扩容。

---

### 4. 代码实现 (Golang 版本)

```go
package main

import (
	"fmt"
	"hash/crc32"
	"sort"
	"strconv"
	"sync"
)

// HashFunc 定义哈希函数类型
type HashFunc func(data []byte) uint32

// Map 一致性哈希主结构
type Map struct {
	hashFunc    HashFunc       // 哈希算法
	replicas    int            // 虚拟节点倍数
	keys        []int          // 哈希环（已排序的哈希值切片）
	hashMap     map[int]string // 虚拟节点哈希值 -> 物理节点名称的映射
	sync.RWMutex               // 读写锁，保证并发安全
}

// New 创建一致性哈希实例
func New(replicas int, fn HashFunc) *Map {
	m := &Map{
		replicas: replicas,
		hashFunc: fn,
		hashMap:  make(map[int]string),
	}
	// 默认使用 CRC32，也可以换成 MurmurHash
	if m.hashFunc == nil {
		m.hashFunc = crc32.ChecksumIEEE
	}
	return m
}

// Add 添加物理节点
func (m *Map) Add(keys ...string) {
	m.Lock()
	defer m.Unlock()

	for _, key := range keys {
		// 为每个物理节点创建 replicas 个虚拟节点
		for i := 0; i < m.replicas; i++ {
			// 创建虚拟节点名称，例如 "NodeA#1"
			hash := int(m.hashFunc([]byte(strconv.Itoa(i) + key)))
			
			// 将虚拟节点哈希值加入切片
			m.keys = append(m.keys, hash)
			// 记录映射关系
			m.hashMap[hash] = key
		}
	}
	// 核心：对环上的点进行排序，方便后续二分查找
	sort.Ints(m.keys)
}

// Get 根据数据 Key 获取对应的物理节点
func (m *Map) Get(key string) string {
	m.RLock()
	defer m.RUnlock()

	if len(m.keys) == 0 {
		return ""
	}

	// 计算数据 Key 的哈希值
	hash := int(m.hashFunc([]byte(key)))

	// 二分查找：找到第一个 >= hash 的虚拟节点索引
	// sort.Search 实际上就是顺时针查找的过程
	idx := sort.Search(len(m.keys), func(i int) bool {
		return m.keys[i] >= hash
	})

	// 如果 idx == len(m.keys)，说明环走到了尽头，需要绕回到 0 (环状结构)
	if idx == len(m.keys) {
		idx = 0
	}

	// 返回对应的物理节点
	return m.hashMap[m.keys[idx]]
}

func main() {
	// 创建一致性哈希，每个物理节点对应 3 个虚拟节点
	cHash := New(3, nil)

	// 添加物理节点
	cHash.Add("Server_A", "Server_B", "Server_C")

	// 测试 Key 的分布
	testKeys := []string{"user_1", "user_2", "order_123", "pic_555", "video_999"}

	for _, k := range testKeys {
		server := cHash.Get(k)
		fmt.Printf("Key [%s] 路由到 -> %s\n", k, server)
	}
}
```

### 5. 总结与扩展

**总结：**
1.  **定义**：一致性哈希是一种特殊的哈希算法，将哈希空间构建成环。
2.  **作用**：解决了分布式系统中节点动态变化带来的数据剧烈震荡问题。
3.  **关键特性**：
    *   **平衡性（Balance）**：通过虚拟节点解决。
    *   **单调性（Monotonicity）**：新增节点时，旧数据不会映射到无关的旧节点上，只会迁移到新节点。
    *   **分散性（Spread）**：避免相同内容映射到不同缓冲中。

