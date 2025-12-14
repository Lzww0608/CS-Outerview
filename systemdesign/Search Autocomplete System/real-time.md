设计一个支持 **Real-time Trending（实时热搜/趋势）** 的自动补全系统，比传统的静态补全系统要复杂得多。

传统系统的权重通常基于“过去几周/几月的总搜索量”，数据每天或每周更新一次（Batch Processing）。
而实时趋势系统要求：**如果某个词在过去 1 小时内搜索量飙升，它必须立刻出现在补全列表的顶部。**

以下是针对该需求的高阶系统设计方案，涵盖架构、数据流和关键算法。

---

### 1. 总体架构设计

我们将系统拆分为两条链路：
1.  **离线/历史链路 (Offline Path):** 处理长期累积的历史数据（传统的 Trie 树）。
2.  **实时链路 (Real-time Path):** 处理最近 N 分钟/小时的热点数据（流式计算）。

**核心组件图解：**

```text
[用户 User]
    | (输入 "ipho")
    v
[负载均衡 LB]
    |
[API Gateway]
    |----------------------------------------+
    | (读请求: 获取补全)                      | (写请求: 记录搜索日志)
    v                                        v
[Autocomplete Service]                  [Analytics Service]
    |              \                         |
    | (读取)        \ (读取)                 v
[历史 Trie Cache]   [实时 Trending Cache]   [消息队列 Kafka] <--- 缓冲海量日志
(Redis/Memcached)  (Redis ZSet)              |
                                             v
                                    [流式计算引擎 Stream Processor]
                                    (Apache Flink / Spark Streaming)
                                             |
                                             v
                                    [实时数据更新 Worker]
```

---

### 2. 核心设计步骤详解

#### 第一步：数据采集与缓冲 (Data Ingestion)
*   **挑战：** 搜索请求量巨大（假设每秒数万 QPS），直接写数据库会挂。
*   **方案：**
    *   当用户执行搜索时，前端发送异步日志到后端。
    *   后端将日志推送到 **Kafka**。Kafka 作为缓冲区，负责削峰填谷。

#### 第二步：流式聚合 (Stream Processing)
这是实时系统的“心脏”。我们需要计算“最近 X 时间窗口内的 Top N 搜索词”。

*   **工具：** 使用 **Apache Flink** 或 **Spark Streaming**。
*   **算法：滑动窗口 (Sliding Window)**
    *   我们需要统计过去 1 小时的数据，但每 5 分钟更新一次结果。
    *   **Window Size:** 60 分钟。
    *   **Slide Interval:** 5 分钟。
*   **处理逻辑：**
    1.  从 Kafka 读取搜索流。
    2.  按照 `search_query` 进行 `KeyBy`（分组）。
    3.  在窗口内进行 `Count` 聚合。
    4.  每 5 分钟输出一次该窗口内的 Top K（例如 Top 1000）热词及其频率。

    > **优化 - 概率数据结构：** 如果搜索词极其分散，内存放不下所有词的计数，可以使用 **Count-Min Sketch** 算法。它能在极小的内存中估算出海量数据的频率，虽然有极小误差，但对热搜场景完全可接受。

#### 第三步：存储实时趋势 (Real-time Storage)
计算出的热词需要存放在一个支持**快速排序和查找**的地方。

*   **工具：** **Redis** 是最佳选择。
*   **数据结构：** **Sorted Set (ZSet)**。
    *   Key: `trending_queries`
    *   Member: `搜索词 (e.g., "iphone 15")`
    *   Score: `频率/热度值 (e.g., 5000)`
*   **生命周期管理：** 每次流计算写入新的 Top K 时，可以覆盖旧的 ZSet，或者设置较短的 TTL（过期时间），保证数据的新鲜度。

#### 第四步：查询服务与混合排序 (Serving & Ranking)
当用户输入 "ip" 时，Autocomplete Service 需要合并两部分数据：

1.  **历史数据：** 从构建好的静态 Trie 树中找到 "ip" 开头的词（如 `ipad`, `ip address`）。
2.  **实时数据：** 从 Redis ZSet 中获取当前的 Top Trending 列表。
    *   *注意：* ZSet 主要是存“完整词”。为了支持前缀匹配，我们可以在内存中维护一个**微型 Trie (Mini-Trie)**，专门存 Top 1000 的热词。由于数据量小（仅几千个），这个 Mini-Trie 可以直接存在应用服务器的内存中，每几分钟从 Redis 拉取更新。

3.  **最终排名算法 (Ranking Logic)：**
    系统需要给出一个最终分数 `Final_Score`。
    $$Final\_Score = (W_{history} \times P_{history}) + (W_{realtime} \times P_{realtime})$$
    *   $P$: 归一化后的频率概率。
    *   $W$: 权重。如果是突发新闻，我们希望 $W_{realtime}$ 很高。
    *   通常会给实时热词一个**加权系数 (Boosting Factor)**。如果某个词在实时列表中，它的排名会被强行拉高。

---

### 3. 关键难点与优化

#### A. 怎么判断“Trending”（趋势）不仅仅是“高频”？
单纯的高频（High Frequency）可能是常青词（如 "Facebook", "Weather"）。Trending 意味着**“当前频率远高于历史平均频率”**。
*   **做法：** 流式计算引擎在计算当前窗口频率时，可以对比该词的历史平均基线。
*   **公式：** $Trending\_Score = \frac{Current\_Count}{Historical\_Average}$。只有比值超过阈值的才进入 Trending 列表。

#### B. 前端体验优化 (Debouncing)
用户输入极快时，不要对每个字符都请求后端。
*   **防抖 (Debounce)：** 等用户停止输入 200ms 后再发请求。
*   **本地缓存：** 如果用户搜了 "iphone"，结果里有 "iphone 15"；当用户继续打出 "iphone 1" 时，前端可以直接从上次的结果里过滤，不用请求后端。

#### C. 应对“脏数据”和“攻击”
如果有人写脚本疯狂刷某个词，会导致它上热搜。
*   **去噪：** 在 Analytics Service 层，基于 IP、User-Agent 进行限流。
*   **黑名单：** 过滤掉色情、暴力或被操纵的关键词。

### 4. 总结方案

要实现 Real-time Trending Autocomplete，核心在于**Lambda 架构**的思想：

1.  **Batch Layer (Trie):** 保证基础体验，覆盖长尾词，数据准确但更新慢。
2.  **Speed Layer (Flink + Redis ZSet):** 捕捉热点，数据更新快（分钟级）。
3.  **Serving Layer:** 动态合并两者的结果，通过加权算法将热点词顶到列表最前。