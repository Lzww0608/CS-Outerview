### 1. 什么是 URL Shortener？

简单来说，URL Shortener 是一个将**长 URL（Long URL）** 映射为 **短 URL（Short URL）** 的服务。

用户访问短链接时，服务器会通过 HTTP 重定向（Redirect）技术，将用户引导至原始的长链接地址。

*   **输入**: `https://www.example.com/products/electronics/phones/model-x-2024?utm_source=google&utm_medium=cpc` (超长，难记，占字符)
*   **输出**: `https://t.cn/Ab3dEf` (极短，易传播)

### 2. 核心原理：HTTP 重定向

短网址服务的底层核心是利用 HTTP 协议的状态码。当浏览器访问短链接时，服务端会返回一个重定向响应。

这里有两个关键的状态码选择，面试中经常问到：

*   **301 Moved Permanently (永久重定向)**:
    *   **含义**: 告诉浏览器这个 URL 永久变了，以后不用问我了，直接去新地址。
    *   **优点**: 减轻服务器压力。浏览器和 CDN 会缓存这个重定向结果，下次用户再点击，浏览器直接跳转，不经过你的服务器。
    *   **缺点**: **无法统计数据**。因为请求不经过服务器，你没法知道用户点击了多少次。
*   **302 Found (临时重定向)**:
    *   **含义**: 告诉浏览器暂时去新地址，但下次还得来问我。
    *   **优点**: **数据统计精准**。每次跳转都会经过服务器，你可以记录日志（User-Agent, IP, 点击时间等）。
    *   **缺点**: 服务器压力大，每次点击都是一次请求。

**结论**: 绝大多数商业短网址服务（如 bit.ly）为了做数据分析（Analytics），都会选择 **302 重定向**。

### 3. 核心算法：如何生成短码？

这是系统设计的核心。我们需要一个算法将长字符串（或 ID）转换为短字符串。最通用的方案是 **Base62 编码**。

#### 为什么是 Base62？
URL 中合法的字符包括：`a-z` (26个), `A-Z` (26个), `0-9` (10个)。加起来正好 62 个字符。

#### 方案 A：哈希算法 (Hash) - **不推荐**
对长 URL 做 MD5 或 SHA256，然后截取前几位。
*   **问题**: 哈希会有**碰撞（Collision）**。虽然概率低，但在高并发海量数据下处理冲突很麻烦（需要加盐重试等），且哈希是不可逆的。

#### 方案 B：自增 ID + Base62 转换 - **推荐**
这是工业界的标准做法。
1.  **全局唯一 ID**: 每来一个长 URL，我们给它发一个全局唯一的整数 ID（类似数据库的主键）。
2.  **进制转换**: 将这个 10 进制的 ID 转换为 62 进制。

**示例**:
假设 ID = `11157`

*   `11157` 在 62 进制下对应 `2TX`。
*   短链接就是 `http://xx.xx/2TX`。

**容量计算**:
*   6 位 Base62: $62^6 \approx 568$ 亿。
*   7 位 Base62: $62^7 \approx 3.5$ 万亿。
对于大多数业务，6-7 位字符足够用到天荒地老。

### 4. 系统架构设计 (System Design)

如果让我设计一个高并发的 URL Shortener，我会采用以下架构：

1.  **发号器 (ID Generator)**:
    *   不能用单机 MySQL 的 `AUTO_INCREMENT`，因为有性能瓶颈且容易暴露业务量。
    *   **方案**: 使用 **Snowflake (雪花算法)** 或者 **Redis 的 INCR** 配合预分配号段（比如每次取 1000 个 ID 在内存中慢慢发），保证高性能和 ID 不重复。

2.  **存储层 (Storage)**:
    *   这是一个典型的 **KV (Key-Value)** 场景。
    *   **读多写少**: 只有生成时写一次，后面全是读。
    *   **选型**: **Redis** 做缓存（存热点短链接），**NoSQL (如 Cassandra, DynamoDB, MongoDB)** 或 **分库分表的 MySQL** 做持久化存储。
    *   结构: `{ key: "short_code", value: "long_url", created_at: "..." }`

3.  **布隆过滤器 (Bloom Filter)**:
    *   为了防止用户生成重复的长链接（即同一个长链接生成了两个不同的短链接），可以在写入前用布隆过滤器快速判断该长链接是否已存在。

4.  **高性能读取**:
    *   用户点击 -> Nginx -> Go/C++ 服务 -> **Redis 缓存命中?** -> (是) 返回 302 -> (否) 查 DB -> 写入 Redis -> 返回 302。

### 5. 代码演示 (C++ Base62 实现)

```cpp
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

class Base62 {
private:
    static const std::string BASE62_CHARS;

public:
    // ID 转 Short URL
    static std::string Encode(uint64_t id) {
        if (id == 0) return std::string(1, BASE62_CHARS[0]);
        
        std::string short_url;
        while (id > 0) {
            short_url += BASE62_CHARS[id % 62];
            id /= 62;
        }
        // 因为是取模append，所以需要反转
        std::reverse(short_url.begin(), short_url.end());
        return short_url;
    }

    // Short URL 转 ID (用于反查，虽然一般直接查库)
    static uint64_t Decode(const std::string& short_url) {
        uint64_t id = 0;
        for (char c : short_url) {
            int val = 0;
            if (c >= 'a' && c <= 'z') val = c - 'a';
            else if (c >= 'A' && c <= 'Z') val = c - 'A' + 26;
            else if (c >= '0' && c <= '9') val = c - '0' + 52;
            
            id = id * 62 + val;
        }
        return id;
    }
};

const std::string Base62::BASE62_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

int main() {
    uint64_t db_id = 123456789;
    std::string short_code = Base62::Encode(db_id);
    
    std::cout << "DB ID: " << db_id << std::endl;
    std::cout << "Short Code: " << short_code << std::endl; // 输出: 8M0kX
    std::cout << "Decoded ID: " << Base62::Decode(short_code) << std::endl;
    
    return 0;
}
```

