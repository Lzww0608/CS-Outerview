### 1. 需求分析与规模估算 (Requirements & Constraints)

在动手画图之前，必须明确量级。

*   **功能性需求：**
    *   视频上传（断点续传）。
    *   视频处理（转码、压缩、生成缩略图）。
    *   视频播放（自适应流媒体）。
    *   搜索与推荐（略讲，重点放在流媒体核心）。
    *   用户交互（点赞、评论）。

*   **非功能性需求：**
    *   **高可用性 (Availability)：** 系统必须始终在线（99.99%）。
    *   **低延迟 (Latency)：** 播放开始时间（Start-up time）必须极短，且无缓冲。
    *   **高吞吐 (Throughput)：** 支持海量并发上传和播放。
    *   **一致性 (Consistency)：** 最终一致性即可（上传后几秒钟才能搜到是可以接受的）。

*   **估算 (Back-of-the-envelope Estimation)：**
    *   假设日活 (DAU): 1亿。
    *   读写比: 100:1 (看的人远多于发的人)。
    *   **存储：** 假设每分钟上传500小时视频。如果要保留10年，需要 Exabyte (EB) 级别的存储。
    *   **带宽：** 流量是巨大的成本，需要极致的压缩和边缘计算。

---

### 2. 总体架构设计 (High-Level Architecture)

我们将系统分为三个核心部分：**客户端(Client)**、**CDN/边缘层**、**后端服务层**。

#### 架构图逻辑：
1.  **Client:** Web/Mobile/Smart TV。
2.  **Load Balancer (L7):** Nginx/Envoy，负责SSL终结和流量分发。
3.  **API Gateway:** Go编写，处理鉴权、限流、路由。
4.  **Microservices:**
    *   **Metadata Service (Go/gRPC):** 处理视频标题、描述、评论。
    *   **Upload Service (Go):** 处理文件上传流。
    *   **Transcoding Service (C++/Rust):** 视频转码核心。
    *   **Recommendation Service (Python/C++):** 推荐算法。
5.  **Storage:**
    *   **Blob Storage (S3/GCS):** 存储原始视频和转码后的切片。
    *   **Database (MySQL/Vitess or Cassandra):** 存储元数据。
    *   **Cache (Redis/Memcached):** 热点数据缓存。

---

### 3. 核心子系统深度解析 (Deep Dive)

这是体现技术深度的关键环节。

#### 3.1 视频上传与转码系统 (Upload & Transcoding)

**挑战：** 用户上传的是各种格式（AVI, MKV, MOV），我们需要将其转换为统一的流媒体格式（HLS/DASH），并生成不同分辨率（360p, 720p, 4K）。这是一个CPU密集型任务。

**设计方案：DAG (有向无环图) 工作流**

1.  **预处理：** 上传服务接收视频流，先写入临时存储（Temp Storage）。
2.  **分片 (Chunking)：** 为了并行处理，我们将大视频切分为GOP (Group of Pictures) 对齐的小片段。
3.  **消息队列：** 使用 Kafka 作为任务分发中心。
4.  **转码工作者 (Workers)：**
    *   这里使用 **C++** 或 **Rust** 编写，调用 FFmpeg 库或硬件加速（NVENC/Intel QSV）。
    *   **为什么用 C++/Rust？** 极致的性能和内存控制。Go 的 GC 在高负载视频处理下可能会导致延迟抖动，而 C++/Rust 可以手动管理内存，利用 SIMD 指令集加速。

**代码实现思路 (C++ Worker 伪代码):**

```cpp
// 这是一个简化的转码Worker逻辑
#include <iostream>
#include <vector>
#include <string>
// 假设引入了FFmpeg库的封装
#include "ffmpeg_wrapper.h" 

class Transcoder {
public:
    void processChunk(const std::string& inputPath, const std::string& outputPath, const VideoConfig& config) {
        // 1. 打开输入流
        AVFormatContext* inputCtx = openInput(inputPath);
        
        // 2. 配置编码器 (H.264/H.265/AV1)
        AVCodecContext* codecCtx = setupEncoder(config.codec, config.bitrate);
        
        // 3. 解码 -> 缩放/滤镜 -> 编码 Loop
        AVPacket packet;
        while (av_read_frame(inputCtx, &packet) >= 0) {
            auto frame = decode(packet);
            auto scaledFrame = scale(frame, config.resolution); // 调整分辨率
            auto encodedPacket = encode(scaledFrame, codecCtx);
            writePacket(outputPath, encodedPacket);
        }
        
        // 4. 清理资源 (RAII风格在实际代码中很重要)
        closeResources(inputCtx, codecCtx);
    }
};

// 配合 Kafka 消费
void workerLoop() {
    while (true) {
        Task task = kafkaConsumer.poll();
        if (task) {
            Transcoder t;
            // 并行处理，利用多核CPU
            std::thread([t, task](){
                t.processChunk(task.input, task.output, task.config);
                // 上报状态到 Redis/Zookeeper
                updateStatus(task.id, "COMPLETED");
            }).detach();
        }
    }
}
```

#### 3.2 视频流播放 (Streaming)

**核心技术：DASH (Dynamic Adaptive Streaming over HTTP)**

*   **原理：** 视频被切分成几秒钟的小块（Chunks），每个块有不同的码率（Bitrate）。客户端根据当前网速，动态选择下载哪个码率的块。
*   **Manifest File (.mpd):** 这是一个索引文件，告诉客户端有哪些分辨率可选，以及每个块的URL。

**架构优化：CDN 与 边缘计算**
*   **热点视频：** 推送到 CDN 边缘节点。
*   **冷门视频（Long Tail）：** 存储在源站（Object Storage），甚至为了省钱存储在冷存储（Glacier）中，有人访问时再取出。

---

### 4. 数据库与存储设计 (Database & Storage)

#### 4.1 元数据存储 (Metadata)

视频ID、标题、上传者ID等。
**选型：** MySQL (配合 Vitess 进行分库分表) 或 Cassandra。
**理由：** 读多写少。

*   **分片策略 (Sharding):**
    *   不能仅按 UserID 分片（会导致热点用户问题，如Justin Bieber发片）。
    *   应按 **VideoID** 进行一致性哈希分片。

#### 4.2 视频二进制存储 (Blob Storage)

**选型：** 自研分布式文件系统 (类似 GFS/Haystack) 或 S3。
**优化：Erasure Coding (纠删码)**
*   对于海量视频，简单的3副本策略太浪费空间（300%开销）。
*   使用 Reed-Solomon 算法，比如 10+4 方案（10个数据块，4个校验块）。
*   **收益：** 存储开销只有 1.4倍，却能容忍丢失任意4个块。

---

### 5. 高级话题与优化 (Advanced Optimization)

#### 5.1 惊群效应与缓存击穿 (Thundering Herd)
当一个大V发布视频，数百万用户瞬间请求同一个 Metadata。
**解决方案：**
1.  **CDN 缓存：** 静态资源挡在最前面。
2.  **应用层缓存 (Redis):** 使用 "Cache Aside" 模式。
3.  **Singleflight (Go pattern):** 在应用内部，对于同一个 Key 的并发请求，只允许一个请求去数据库查询，其他请求等待结果共享。

**Go 代码示例 (Singleflight):**

```go
import (
    "fmt"
    "golang.org/x/sync/singleflight"
    "time"
)

var g singleflight.Group

// GetVideoMetadata 防止缓存击穿
func GetVideoMetadata(videoID string) (string, error) {
    // 尝试从 Redis 获取...
    
    // 如果 Redis 没有，走 Singleflight
    v, err, _ := g.Do(videoID, func() (interface{}, error) {
        // 这一段逻辑在同一时刻，无论多少并发，只会执行一次
        fmt.Println("Querying Database for", videoID)
        time.Sleep(100 * time.Millisecond) // 模拟DB耗时
        return "Video Title: System Design", nil
    })

    if err != nil {
        return "", err
    }
    return v.(string), nil
}
```

#### 5.2 推荐系统去重 (Bloom Filter)
在给用户推荐视频时，如何快速判断该视频用户是否看过？
*   **Bloom Filter (布隆过滤器):** 空间效率极高。
*   在 Redis 中维护每个用户的 Bloom Filter。
*   虽然有误判率（False Positive），但在推荐场景下，偶尔过滤掉一个没看过的视频是可以接受的。

---

### 6. 总结 (Summary)

设计 YouTube 系统的核心在于：

1.  **拆分复杂度：** 将上传（Write）和播放（Read）分离。
2.  **多语言协作：** Go 用于高并发微服务编排，C++/Rust 用于底层的编解码计算。
3.  **存储分级：** 热数据在 CDN/内存，温数据在 SSD，冷数据在 HDD/磁带，并利用纠删码降低成本。
4.  **网络优化：** 利用 DASH 协议适应不稳定的网络环境。

这个设计涵盖了从底层的 CPU 指令集优化（转码）到上层的分布式架构（微服务、CDN），展示了全栈式的系统设计能力。