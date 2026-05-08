# cgroups 网络带宽限制能力分析

## 1. 问题背景

cgroups 是 Linux 内核的资源管理框架，可以对进程组的 CPU、内存、磁盘 I/O 等资源进行限制。用户疑问：cgroups 能否描述和实现网络带宽限制？

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  核心结论：                                                │
│                                                             │
│  cgroups 本身不直接实现网络带宽限制                        │
│                                                             │
│  网络带宽限制需要配合 tc (Traffic Control) 实现           │
│                                                             │
│  cgroups 的作用：                                         │
│  • 标记（tag）来自特定 cgroup 的网络包                    │
│  • 与 tc 结合实现带宽控制                                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 问题现象

| 疑问 | 回答 |
|------|------|
| cgroups 能限制网络带宽吗？ | 不能直接限制，需要配合 tc |
| cgroups 网络相关子系统有哪些？ | net_cls、net_prio、net (v2) |
| tc 和 cgroups 是什么关系？ | tc 负责带宽限制，cgroups 负责流量标记 |
| 为什么不直接用 iptables？ | iptables 主要用于防火墙，带宽控制推荐 tc |

---

## 3. 影响范围

- 容器网络隔离（Docker/Kubernetes）
- 多租户资源公平分配
- 服务质量（QoS）保障
- 带宽抢占控制

---

## 4. 原因分析

### 4.1 cgroups 网络相关子系统

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  cgroups 网络相关子系统：                                  │
│                                                             │
│  1. net_cls (cgroups v1)                                  │
│     • 为网络包打上 classid 标记                            │
│     • 配合 tc 进行带宽限制                                  │
│                                                             │
│  2. net_prio (cgroups v1)                                 │
│     • 设置网络包的优先级                                    │
│     • 用于 QoS 优先级调度                                   │
│                                                             │
│  3. net (cgroups v2)                                       │
│     • v2 中统一的网络资源控制                              │
│     • 支持更多精细化控制                                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 为什么 cgroups 不能直接限制带宽

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  架构对比：                                                │
│                                                             │
│  cgroups 架构：                                           │
│  进程 ──► cgroup ──► 资源限制                            │
│                                                             │
│  cgroups 只负责：                                         │
│  • 资源分配和隔离                                         │
│  • 统计和限制                                             │
│  • 优先级标记                                             │
│                                                             │
│  但网络带宽限制需要：                                      │
│  • 流量整形（Shaping）                                    │
│  • 队列调度（Scheduling）                                 │
│  • 丢包策略（Dropping）                                   │
│                                                             │
│  这些是 tc (Traffic Control) 的职责                       │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.3 net_cls 工作原理

```bash
# net_cls 标记流程

# 1. 进程加入 cgroup
echo $PID > /sys/fs/cgroup/net_cls/limit_100m/tasks

# 2. 设置 classid（格式：major:minor）
# 0x100001 = 1:1 (主要:次要)
echo 0x100001 > /sys/fs/cgroup/net_cls/limit_100m/net_cls.classid

# 3. 进程发送的所有网络包都会带上这个标记
```

### 4.4 tc 配合 cgroups 实现带宽限制

```bash
# tc 限制带宽流程

# 1. 创建 HTB 队列规则
tc qdisc add dev eth0 root handle 1: htb default 10

# 2. 创建类（带宽限制）
tc class add dev eth0 parent 1: classid 1:1 htb rate 100mbit

# 3. 创建过滤器（匹配 cgroups 标记）
tc filter add dev eth0 parent 1: protocol ip handle 1: cgroup

# 4. 结果：
# cgroup 中的进程 → net_cls 标记 → tc 匹配 → 带宽限制
```

---

## 5. 解决方案

### 5.1 完整配置示例

```bash
#!/bin/bash
# cgroups + tc 实现网络带宽限制

# 配置参数
DEVICE="eth0"
CGROUP_NAME="limit_100m"
RATE="100mbit"
BURST="15k"

# 1. 创建 cgroup
mkdir -p /sys/fs/cgroup/net_cls/$CGROUP_NAME

# 2. 设置 classid (0x100001 = 1:1)
echo 0x100001 > /sys/fs/cgroup/net_cls/$CGROUP_NAME/net_cls.classid

# 3. 初始化 tc（避免重复配置）
tc qdisc del dev $DEVICE root 2>/dev/null

# 4. 添加 HTB 队列
tc qdisc add dev $DEVICE root handle 1: htb default 10

# 5. 添加主类
tc class add dev $DEVICE parent 1: classid 1:1 htb rate $RATE ceil ${RATE}

# 6. 添加过滤器匹配 cgroup classid
tc filter add dev $DEVICE parent 1: protocol ip prio 1 handle 1: cgroup

echo "配置完成：$CGROUP_NAME 限制为 $RATE"
```

### 5.2 Python 脚本封装

```python
#!/usr/bin/env python3
"""
cgroups 网络带宽限制配置工具
"""

import os
import subprocess
import sys

class CgroupNetworkLimiter:
    """
    cgroups + tc 网络带宽限制
    """
    
    def __init__(self, cgroup_name, rate_mbit, device='eth0'):
        self.cgroup_name = cgroup_name
        self.rate = f"{rate_mbit}mbit"
        self.device = device
        self.cgroup_path = f"/sys/fs/cgroup/net_cls/{cgroup_name}"
    
    def setup_cgroup(self):
        """创建 cgroup 并设置 classid"""
        os.makedirs(self.cgroup_path, exist_ok=True)
        
        # classid 格式: major:minor，这里用 1:1
        classid = 0x100001
        with open(f"{self.cgroup_path}/net_cls.classid", 'w') as f:
            f.write(hex(classid))
        
        print(f"✅ cgroup 创建完成: {self.cgroup_path}")
    
    def setup_tc(self):
        """配置 tc 带宽限制"""
        # 清理旧配置
        subprocess.run(
            f"tc qdisc del dev {self.device} root 2>/dev/null",
            shell=True
        )
        
        # 添加 HTB 队列
        subprocess.run(
            f"tc qdisc add dev {self.device} root handle 1: htb default 10",
            shell=True
        )
        
        # 添加带宽限制类
        subprocess.run(
            f"tc class add dev {self.device} parent 1: classid 1:1 "
            f"htb rate {self.rate} ceil {self.rate}",
            shell=True
        )
        
        # 添加过滤器
        subprocess.run(
            f"tc filter add dev {self.device} parent 1: protocol ip "
            f"prio 1 handle 1: cgroup",
            shell=True
        )
        
        print(f"✅ tc 配置完成: 限制 {self.rate}")
    
    def add_process(self, pid):
        """将进程加入限制组"""
        with open(f"{self.cgroup_path}/tasks", 'w') as f:
            f.write(str(pid))
        print(f"✅ 进程 {pid} 已加入限制组")
    
    def apply(self, pid=None):
        """应用所有配置"""
        self.setup_cgroup()
        self.setup_tc()
        if pid:
            self.add_process(pid)

# 使用示例
if __name__ == '__main__':
    limiter = CgroupNetworkLimiter(
        cgroup_name='limit_100m',
        rate_mbit=100,
        device='eth0'
    )
    
    limiter.apply(pid=int(sys.argv[1]) if len(sys.argv) > 1 else os.getpid())
```

---

## 6. 可选方案对比

| 方案 | 精度 | 复杂度 | 灵活性 | 适用场景 |
|------|------|--------|--------|----------|
| cgroups + tc | 高 | 中 | 高 | 生产环境推荐 |
| iptables limit | 中 | 低 | 低 | 简单限速 |
| tc 单独使用 | 高 | 中 | 中 | 已有流量标记 |
| 第三方工具 | 高 | 低 | 中 | 快速部署 |

---

## 7. 推荐方案

### 7.1 生产环境推荐

```bash
# 生产环境 cgroups + tc 配置

# 1. systemd 服务配置（/etc/systemd/system/bandwidth-limit.service）
[Unit]
Description=Network Bandwidth Limiter

[Service]
Type=oneshot
ExecStart=/usr/local/bin/bandwidth-limit.sh

# 2. 带宽限制脚本
#!/bin/bash
RATE=${RATE:-100mbit}
DEVICE=${DEVICE:-eth0}

/usr/sbin/tc qdisc add dev $DEVICE root handle 1: htb default 10
/usr/sbin/tc class add dev $DEVICE parent 1: classid 1:1 htb rate $RATE

# 3. Kubernetes NetworkPolicy 配合
# 使用 CNI 插件（如 Calico、Weave）实现更细粒度控制
```

---

## 8. 实施步骤

### 8.1 验证环境

```bash
# 1. 检查内核支持
cat /proc/filesystems | grep cgroup
ls /sys/fs/cgroup/

# 2. 检查 tc 可用性
tc qdisc show

# 3. 检查网络设备
ip link show
```

### 8.2 实施流程

```text
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  cgroups + tc 带宽限制实施步骤：                          │
│                                                             │
│  Step 1: 创建 cgroup                                      │
│  mkdir -p /sys/fs/cgroup/net_cls/limit_100m              │
│                                                             │
│  Step 2: 设置 classid 标记                                │
│  echo 0x100001 > /sys/fs/cgroup/net_cls/limit_100m/...  │
│                                                             │
│  Step 3: 配置 tc 队列和过滤器                              │
│  tc qdisc add ... && tc class add ... && tc filter add...│
│                                                             │
│  Step 4: 将进程加入 cgroup                                 │
│  echo $PID > /sys/fs/cgroup/net_cls/limit_100m/tasks     │
│                                                             │
│  Step 5: 验证带宽限制效果                                  │
│  iperf3 -c target -p 9001                                 │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 9. 风险与注意事项

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| tc 配置错误 | 网络中断 | 使用低优先级 filter |
| classid 冲突 | 限制失效 | 确保 classid 唯一 |
| cgroup 删除 | 进程丢失标记 | 先移除进程再删除 |
| v1/v2 混用 | 功能异常 | 确认系统 cgroups 版本 |

---

## 10. 验证方式

```bash
# 1. 验证 cgroup 配置
cat /sys/fs/cgroup/net_cls/limit_100m/net_cls.classid
cat /sys/fs/cgroup/net_cls/limit_100m/tasks

# 2. 验证 tc 配置
tc class show dev eth0
tc filter show dev eth0

# 3. 带宽测试
iperf3 -s -p 9001 &          # 服务端
iperf3 -c localhost -p 9001  # 客户端

# 4. 观察 tc 统计
tc -s class show dev eth0
tc -s qdisc show dev eth0
```

---

## 11. 总结

1. **核心结论**：cgroups 本身不能直接限制网络带宽，但可以通过 net_cls 标记流量，配合 tc 实现带宽限制
2. **作用分工**：cgroups 负责标记（tag），tc 负责整形（shape）
3. **实现方式**：net_cls.classid → tc filter → tc class → 带宽限制
4. **生产推荐**：cgroups + tc HTB 队列 + 过滤器组合使用
5. **替代方案**：如果只需要简单限速，也可以使用 iptables 的 limit 模块或 tc 直接配置
