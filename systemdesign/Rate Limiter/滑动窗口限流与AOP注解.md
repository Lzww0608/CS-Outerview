# 滑动窗口限流与 AOP 注解

滑动窗口限流是在任意连续时间窗口内限制请求数量，避免固定窗口在边界处放过双倍流量。面试里如果追问“怎么实现限流操作、AOP、注解”，要把算法、存储、原子性和框架接入分开说。

## 1. 滑动窗口的两种实现

### 滑动日志

每个请求记录一个时间戳，来新请求时删除窗口外时间戳，再统计窗口内数量。

Redis 常用 ZSet 实现：

```text
key: rate:api:userId
score: timestamp_ms
member: timestamp_ms + random
```

流程：

1. 删除 `now - window` 之前的记录。
2. 统计当前窗口内数量。
3. 如果数量小于阈值，写入当前请求时间戳并放行。
4. 否则拒绝或降级。

优点是精确；缺点是每个请求都保存一条记录，流量大时内存开销高。

### 滑动计数器

把窗口切成多个小桶，例如 1 分钟切成 60 个 1 秒桶。请求落到当前桶，只统计最近 60 个桶的总和。

优点是内存稳定；缺点是近似值，桶越小越精确，但维护成本越高。

## 2. Redis + Lua 原子实现

限流判断必须原子化，否则并发下多个请求都看到未超限并同时放行。

```lua
local key = KEYS[1]
local now = tonumber(ARGV[1])
local window = tonumber(ARGV[2])
local limit = tonumber(ARGV[3])
local member = ARGV[4]

redis.call('ZREMRANGEBYSCORE', key, 0, now - window)
local count = redis.call('ZCARD', key)
if count >= limit then
    return 0
end

redis.call('ZADD', key, now, member)
redis.call('PEXPIRE', key, window)
return 1
```

注意点：

1. `member` 不能只用时间戳，否则同一毫秒多个请求会覆盖。
2. `PEXPIRE` 要设置，避免冷 key 永久留在 Redis。
3. key 里要包含限流维度，如用户、IP、接口、租户。
4. Redis 故障时要决定 fail open 还是 fail closed。登录、支付等风险接口通常偏 fail closed，普通读接口可以降级放行或本地限流。

## 3. 注解和 AOP 接入

Java 项目可以用注解声明限流规则：

```java
@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.METHOD)
public @interface RateLimit {
    String key();
    int limit();
    long windowMs();
}
```

AOP 在方法执行前拦截：

```java
@Around("@annotation(rateLimit)")
public Object around(ProceedingJoinPoint pjp, RateLimit rateLimit) throws Throwable {
    String key = buildKey(rateLimit.key(), pjp);
    boolean allowed = limiter.tryAcquire(key, rateLimit.limit(), rateLimit.windowMs());
    if (!allowed) {
        throw new TooManyRequestsException();
    }
    return pjp.proceed();
}
```

`buildKey` 通常会从注解表达式、用户 ID、IP、接口名、租户 ID 中拼出限流 key。AOP 的价值是把限流作为横切逻辑统一接入，业务方法不用重复写 Redis 代码。

## 4. 和其他限流算法对比

| 算法 | 特点 | 适用场景 |
| --- | --- | --- |
| 固定窗口 | 实现最简单，边界有突刺 | 粗粒度统计 |
| 滑动日志 | 精确，内存随请求数增长 | 风控、登录、严格接口 |
| 滑动计数器 | 近似，内存稳定 | 高 QPS API |
| 令牌桶 | 允许短突发，限制平均速率 | 网关、上传下载 |
| 漏桶 | 平滑输出，不鼓励突发 | 下游保护、稳定消费 |

## 5. 真实场景

1. 短信验证码：按手机号、IP、设备指纹限流，失败时返回稍后再试。
2. 登录接口：按账号和 IP 做滑动窗口，并结合风控和验证码。
3. AI/RAG 接口：按用户、租户、模型限流，避免高成本调用失控。
4. 秒杀接口：入口层令牌桶削峰，核心库存扣减再用 Redis Lua 做原子资格校验。

## 6. 面试回答模板

滑动窗口限流是在任意连续窗口内控制请求数，避免固定窗口边界突刺。精确实现可以用 Redis ZSet 保存请求时间戳，请求到来时用 Lua 原子地删除过期记录、统计窗口内数量、判断是否放行并写入当前时间戳。Java 项目里可以用 `@RateLimit` 注解声明窗口和阈值，再用 AOP 在方法执行前构造 key 并调用限流器。要注意 key 维度、Lua 原子性、TTL、Redis 故障降级，以及滑动日志的内存开销；高 QPS 场景可以改用滑动计数器或令牌桶。
