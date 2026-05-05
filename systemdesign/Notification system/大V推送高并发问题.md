# 社交媒体大V推送高并发问题详解

## 一、问题背景

### 1.1 场景描述

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  问题场景：                                                 │
│                                                             │
│  网红 A 发一条动态 ──► 1000万粉丝收到推送                │
│                                                             │
│  核心挑战：                                                 │
│  • 瞬时并发：千万级用户同时收到通知                       │
│  • 实时性要求：用户期望秒级收到                          │
│  • 资源成本：服务器、带宽、存储压力巨大                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、传统方案瓶颈

### 2.1 简单推送模型

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  传统推送架构：                                             │
│                                                             │
│  发帖 ──► 写入数据库 ──► 循环推送 ──► 千万用户          │
│                          │                                  │
│                     串行执行                               │
│                     耗时巨大                               │
│                                                             │
│  问题：                                                    │
│  • 1000万用户 × 1秒/万 = 1000秒 ≈ 17分钟                 │
│  • 数据库连接数被耗尽                                     │
│  • 推送服务被拖垮                                         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 三、核心解决方案

### 3.1 分层推送架构

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  分层推送架构：                                             │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                   消息写入层                         │  │
│  │   • 写入消息队列                                    │  │
│  │   • 异步分发                                        │  │
│  └─────────────────────────────────────────────────────┘  │
│                          │                                  │
│                          ▼                                  │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                   分片处理层                         │  │
│  │   • 按粉丝分片                                      │  │
│  │   • 并行处理                                        │  │
│  └─────────────────────────────────────────────────────┘  │
│                          │                                  │
│                          ▼                                  │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                   推送服务层                         │  │
│  │   • 连接管理                                        │  │
│  │   • 消息路由                                        │  │
│  └─────────────────────────────────────────────────────┘  │
│                          │                                  │
│                          ▼                                  │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                   用户设备                           │  │
│  │   • 手机/PC/Web                                   │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 消息队列削峰

```python
# 消息队列架构
import kafka

class MessagePublisher:
    def __init__(self):
        self.producer = kafka.KafkaProducer(
            bootstrap_servers=['kafka:9092'],
            batch_size=16384,
            linger_ms=10,
            compression_type='gzip'
        )
    
    def publish_post(self, user_id, post_id, content):
        """发布动态"""
        # 1. 写入数据库
        self.save_to_db(user_id, post_id, content)
        
        # 2. 发布到消息队列
        message = {
            'user_id': user_id,
            'post_id': post_id,
            'content': content,
            'timestamp': time.time()
        }
        
        self.producer.send(
            'fan-notification-topic',
            value=json.dumps(message),
            partition=self.get_partition(user_id)
        )
        
        return {'status': 'queued'}

# 消费者分片处理
class NotificationConsumer:
    def __init__(self, shard_id, total_shards):
        self.shard_id = shard_id
        self.consumer = kafka.KafkaConsumer(
            'fan-notification-topic',
            group_id=f'notification-consumer-{shard_id}',
            bootstrap_servers=['kafka:9092']
        )
    
    def process(self):
        for message in self.consumer:
            user_id = json.loads(message.value)['user_id']
            
            # 只处理属于当前分片的消息
            if hash(user_id) % total_shards == self.shard_id:
                self.send_to_fans(user_id, message.value)
```

---

## 四、粉丝分片策略

### 4.1 分片处理

```python
import consistent_hash

class FanSharding:
    def __init__(self, num_shards=100):
        self.num_shards = num_shards
        self.ring = consistent_hash.ConsistentHashRing()
    
    def get_shard(self, celebrity_id):
        """获取明星的粉丝分片"""
        # 使用一致性哈希确定分片
        return hash(celebrity_id) % self.num_shards
    
    def fan_iterator(self, celebrity_id, shard_id):
        """获取指定分片的粉丝"""
        start_id = shard_id * 100000
        end_id = (shard_id + 1) * 100000
        
        return self.db.query("""
            SELECT user_id, device_token 
            FROM fan_relations 
            WHERE celebrity_id = %s 
            AND fan_id >= %s AND fan_id < %s
        """, [celebrity_id, start_id, end_id])

# 启动100个消费者并行处理
for shard_id in range(100):
    consumer = NotificationConsumer(shard_id, 100)
    threading.Thread(target=consumer.process).start()
```

### 4.2 批处理优化

```python
# 批量推送
class BatchPushService:
    def __init__(self, batch_size=1000):
        self.batch_size = batch_size
        self.pending = []
    
    def add(self, user_id, device_token, message):
        self.pending.append({
            'user_id': user_id,
            'device_token': device_token,
            'message': message
        })
        
        if len(self.pending) >= self.batch_size:
            self.flush()
    
    def flush(self):
        if not self.pending:
            return
        
        # 批量发送
        messages = self.pending
        self.pending = []
        
        # APNs 批量推送（iOS）
        if self.platform == 'ios':
            self.apns.send_batch(messages)
        
        # FCM 批量推送（Android）
        else:
            self.fcm.send_batch(messages)
```

---

## 五、推送通道优化

### 5.1 设备通道选择

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  推送通道架构：                                             │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                  消息路由层                         │  │
│  │   • 设备类型识别                                    │  │
│  │   • 通道选择                                        │  │
│  └─────────────────────────────────────────────────────┘  │
│                          │                                  │
│           ┌──────────────┼──────────────┐                 │
│           ▼              ▼              ▼                 │
│     ┌─────────┐   ┌─────────┐   ┌─────────┐            │
│     │  APNs   │   │   FCM   │   │  WebPush │            │
│     │ (iOS)   │   │(Android)│   │  (Web)   │            │
│     └─────────┘   └─────────┘   └─────────┘            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 推送服务实现

```python
# 推送服务
class PushService:
    def __init__(self):
        self.apns = APNsClient()
        self.fcm = FCMClient()
        self.webpush = WebPushClient()
    
    def push(self, user_id, message):
        # 1. 获取用户设备列表
        devices = self.get_user_devices(user_id)
        
        # 2. 按平台分组
        ios_tokens = [d.token for d in devices if d.type == 'ios']
        android_tokens = [d.token for d in devices if d.type == 'android']
        web_tokens = [d.token for d in devices if d.type == 'web']
        
        # 3. 并行推送
        futures = []
        
        if ios_tokens:
            futures.append(self.apns.send_async(ios_tokens, message))
        
        if android_tokens:
            futures.append(self.fcm.send_async(android_tokens, message))
        
        if web_tokens:
            futures.append(self.webpush.send_async(web_tokens, message))
        
        # 4. 等待完成
        for f in futures:
            f.result()
```

---

## 六、热榜与延迟推送

### 6.1 分级推送策略

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  分级推送策略：                                             │
│                                                             │
│  1. 铁粉（活跃度 > 80%）                                  │
│     • 实时推送                                             │
│     • 优先通道                                             │
│                                                             │
│  2. 普通粉丝（活跃度 20%-80%）                            │
│     • 5分钟内延迟推送                                      │
│     • 普通通道                                             │
│                                                             │
│  3. 沉默粉丝（活跃度 < 20%）                              │
│     • 30分钟延迟推送或合并推送                            │
│     • 批量通道                                             │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 粉丝分层实现

```python
class FanTiers:
    def __init__(self):
        self.tiers = {
            'iron': (0.8, 1.0, 0),      # 铁粉：实时
            'normal': (0.2, 0.8, 300),   # 普通：5分钟
            'inactive': (0, 0.2, 1800),  # 沉默：30分钟
        }
    
    def get_tier(self, user_id, celebrity_id):
        # 计算活跃度
        activity = self.get_activity(user_id, celebrity_id)
        
        for tier_name, (low, high, delay) in self.tiers.items():
            if low <= activity < high:
                return {
                    'name': tier_name,
                    'delay_seconds': delay,
                    'priority': 'high' if tier_name == 'iron' else 'normal'
                }
    
    def schedule_push(self, celebrity_id, post_id, fan_tiers):
        for tier_name, tier_info in fan_tiers.items():
            if tier_info['delay_seconds'] > 0:
                # 延迟推送
                self.delay_queue.add(
                    celebrity_id, post_id,
                    delay=tier_info['delay_seconds']
                )
            else:
                # 实时推送
                self.immediate_push(celebrity_id, post_id, tier_name)
```

---

## 七、消息合并与去重

### 7.1 消息合并

```python
# 消息合并策略
class MessageCoalescing:
    def __init__(self):
        self.pending_messages = {}  # user_id -> messages
        self.merge_window = 60  # 60秒合并窗口
    
    def add_message(self, user_id, message):
        if user_id not in self.pending_messages:
            self.pending_messages[user_id] = []
        
        self.pending_messages[user_id].append(message)
        
        # 触发合并推送
        if len(self.pending_messages[user_id]) >= 10:
            self.flush_user(user_id)
    
    def flush_user(self, user_id):
        messages = self.pending_messages.get(user_id, [])
        if not messages:
            return
        
        self.pending_messages[user_id] = []
        
        # 合并为一条推送
        merged = self.merge_messages(messages)
        self.push_service.push(user_id, merged)
    
    def merge_messages(self, messages):
        """合并多条消息"""
        if len(messages) == 1:
            return messages[0]
        
        return {
            'title': f"你有 {len(messages)} 条新动态",
            'type': 'multiple',
            'posts': messages[:5],  # 最多显示5条
            'total': len(messages)
        }
```

### 7.2 去重机制

```python
# 消息去重
class DeduplicationService:
    def __init__(self, redis_client):
        self.redis = redis_client
        self.dedup_ttl = 3600  # 1小时去重窗口
    
    def is_duplicate(self, user_id, post_id, action):
        key = f"dedup:{user_id}:{post_id}:{action}"
        
        # 尝试设置，如果已存在则返回 True
        return not self.redis.set(key, '1', nx=True, ex=self.dedup_ttl)
    
    def handle_push(self, user_id, post_id):
        if self.is_duplicate(user_id, post_id, 'push'):
            return  # 跳过重复推送
        
        self.push_service.push(user_id, post_id)
```

---

## 八、效果评估

### 8.1 性能对比

| 方案 | 1000万粉丝推送耗时 | 成功率 |
|------|-------------------|--------|
| 传统串行 | ~17分钟 | 60% |
| 分片并行 | ~2分钟 | 95% |
| 分层+合并 | ~30秒 | 99% |

### 8.2 成本优化

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  成本对比：                                                 │
│                                                             │
│  传统方案：                                                 │
│  • 服务器成本：100台 × 5000元 = 50万/月                   │
│  • 推送通道费：1000万 × 0.01元 = 10万/次                 │
│                                                             │
│  优化方案：                                                 │
│  • 服务器成本：20台 × 5000元 = 10万/月                   │
│  • 推送通道费：合并后500万 × 0.01元 = 5万/次             │
│  • 节省比例：80%                                          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 九、面试常见问题

### Q1: 如何保证推送的实时性？

通过消息队列异步分发，粉丝分片并行处理，铁粉优先实时推送，普通粉丝允许一定延迟。

### Q2: 如何应对突发流量？

1. 消息队列削峰
2. 分片并行处理
3. 分级推送策略
4. 推送合并减少总量

### Q3: 如何避免推送风暴？

1. 限流保护
2. 消息去重
3. 推送合并
4. 冷却机制

### Q4: 如何选择推送通道？

iOS 用 APNs，Android 用 FCM，Web 用 WebPush，根据设备类型选择最优通道。

---

## 十、总结

1. **核心问题**：千万粉丝同时收到推送的瞬时并发
2. **解决方案**：分层推送、分片并行、消息队列削峰
3. **优化策略**：粉丝分层、消息合并、去重
4. **效果**：推送耗时从17分钟降至30秒，成功率从60%提升至99%
