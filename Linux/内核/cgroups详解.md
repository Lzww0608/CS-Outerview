# Linux cgroups 详解

## 一、什么是cgroups
cgroups（全称**Control Groups**，控制组）是Linux内核提供的一套**资源管理框架**，可以对进程组的CPU、内存、磁盘I/O、网络等系统资源进行**隔离、限制、分配和审计**。

它是Linux容器技术（Docker、Kubernetes）的核心底层技术之一，和namespace一起构成了容器资源隔离的基础。

### 核心特点
- **组级管理**：以进程组为单位进行资源控制，而不是单个进程
- **多资源支持**：支持CPU、内存、I/O、网络、PID等多种资源的限制
- **层次化结构**：cgroup可以嵌套继承，形成树形的层级结构
- **无侵入性**：不需要修改应用程序代码，对应用透明
- **动态调整**：可以在运行时动态修改资源限制，不需要重启进程

---

## 二、发展历史与版本

### 1. cgroups v1（2008年，Linux 2.6.24）
- 第一个稳定版本，由Google工程师发起
- 每个子系统（cpu、memory等）独立实现，灵活性高但复杂度也高
- 不同子系统可以有独立的层级结构，管理起来比较复杂
- 目前广泛应用于生产环境，Docker默认使用v1

### 2. cgroups v2（2016年，Linux 4.5）
- 统一的层级结构，所有子系统共享同一个cgroup树
- 简化了管理和配置，解决了v1中多个层级的混乱问题
- 增强了安全性和隔离性，支持更多新特性
- 新版本的Kubernetes、systemd已经开始支持v2
- 目前逐渐成为主流，但生产环境普及还需要时间

### v1和v2的核心区别
| 特性 | cgroups v1 | cgroups v2 |
|------|------------|------------|
| 层级结构 | 每个子系统独立层级 | 所有子系统共享统一层级 |
| 配置复杂度 | 高，多个子系统需要分别配置 | 低，统一配置 |
| 进程管理 | 进程可以属于多个不同子系统的cgroup | 进程只能属于一个cgroup |
| 特性支持 | 功能完善，支持所有子系统 | 部分子系统还在完善中 |
| 性能 | 较高 | 更高，减少了内核开销 |

---

## 三、核心原理与架构

### 1. 核心概念
cgroups主要由三个核心概念组成：
#### （1）子系统（Subsystem）
子系统是内核提供的资源控制器，每个子系统对应一类资源的管理：
| 子系统 | 功能描述 |
|--------|----------|
| `cpu` | 限制进程组的CPU使用时间、CPU核心绑定、优先级 |
| `cpuacct` | 统计进程组的CPU使用情况 |
| `cpuset` | 绑定进程组到指定的CPU核心和内存节点 |
| `memory` | 限制进程组的内存使用量、swap使用量，统计内存使用 |
| `blkio` | 限制进程组的块设备I/O速率（磁盘、SSD等） |
| `net_cls` | 标记进程组的网络数据包，配合tc做流量控制 |
| `net_prio` | 设置进程组网络流量的优先级 |
| `pid` | 限制进程组的最大PID数量，隔离进程树 |
| `devices` | 控制进程组可以访问的设备（允许/禁止） |
| `freezer` | 冻结/恢复整个进程组的所有进程 |
| `hugetlb` | 限制进程组的大页内存使用量 |

#### （2）层级（Hierarchy）
层级是cgroup的树形组织结构：
- 每个层级由一个或多个子系统组成
- 树的每个节点是一个cgroup，可以包含子cgroup
- 子cgroup会继承父cgroup的资源限制
- 一个系统可以有多个层级，每个层级对应不同的子系统组合（v1特性）

#### （3）任务（Task）
任务就是系统中的进程/线程：
- 每个任务只能属于一个cgroup
- 任务创建的子进程默认属于父进程所在的cgroup
- 可以在运行时将任务移动到其他cgroup

### 2. 工作原理
1. **文件系统接口**：cgroups通过虚拟文件系统`cgroupfs`向用户态暴露接口，所有配置和管理都是通过文件操作完成
2. **资源跟踪**：内核为每个cgroup跟踪其下所有进程的资源使用情况
3. **限制 enforcement**：当进程使用的资源超过cgroup限制时，内核会触发相应的动作（限速、OOM杀死、返回错误等）
4. **层级继承**：子cgroup的资源限制不能超过父cgroup的限制，是一个自顶向下的约束体系

### 3. 内核实现机制
cgroups在内核中主要通过以下机制实现：
- **钩子函数**：在资源分配的关键路径上插入钩子，检查cgroup限制
- **计数器**：为每个cgroup维护资源使用计数器
- **回调机制**：当资源使用超过限制时，调用对应的回调函数处理
- **per-cgroup数据结构**：每个cgroup对应内核中的一个结构体，保存配置和统计信息

---

## 四、核心功能

### 1. 资源限制
可以限制进程组的最大资源使用量：
- CPU限制：最多使用2个核心，CPU使用率不超过50%
- 内存限制：最多使用1GB内存，swap不超过256MB
- I/O限制：磁盘读写速率不超过100MB/s
- PID限制：最多创建1024个进程

### 2. 优先级分配
可以给不同cgroup分配不同的资源权重：
- 高优先级业务分配更多CPU时间片
- 关键业务的I/O优先级高于后台任务
- 生产环境容器比测试环境容器获得更多资源

### 3. 资源审计
可以统计进程组的资源使用情况：
- CPU累计使用时间
- 内存峰值使用量
- 磁盘I/O读写总量
- 网络流量统计
- 用于计费、监控、性能分析

### 4. 任务隔离
实现进程组的资源隔离：
- 不同容器之间的资源互不影响
- 一个容器的OOM不会影响其他容器
- 防止单个进程耗尽系统所有资源

### 5. 进程控制
可以对整个进程组执行统一操作：
- 冻结/恢复整个组的所有进程
- 批量杀死组内所有进程
- 挂起/恢复整个业务系统

---

## 五、使用示例

### 1. 查看cgroups挂载情况
```bash
# 查看v1挂载
mount | grep cgroup

# 输出示例：
# cgroup on /sys/fs/cgroup/cpu type cgroup (rw,nosuid,nodev,noexec,relatime,cpu)
# cgroup on /sys/fs/cgroup/memory type cgroup (rw,nosuid,nodev,noexec,relatime,memory)
# cgroup on /sys/fs/cgroup/blkio type cgroup (rw,nosuid,nodev,noexec,relatime,blkio)

# 查看v2挂载
mount | grep cgroup2
# 输出：cgroup2 on /sys/fs/cgroup/unified type cgroup2 (rw,nosuid,nodev,noexec,relatime)
```

### 2. 创建并配置cgroup（v1示例）
#### （1）限制CPU使用
```bash
# 1. 创建cpu子系统的cgroup
mkdir /sys/fs/cgroup/cpu/my_app

# 2. 限制CPU使用率为50%（单个核心）
# cpu.cfs_quota_us: 周期内可用的CPU时间，单位微秒
# cpu.cfs_period_us: 调度周期，默认100000微秒（100ms）
echo 50000 > /sys/fs/cgroup/cpu/my_app/cpu.cfs_quota_us
echo 100000 > /sys/fs/cgroup/cpu/my_app/cpu.cfs_period_us

# 3. 将进程加入cgroup
echo 1234 > /sys/fs/cgroup/cpu/my_app/tasks
```
此时PID为1234的进程最多只能使用50%的单个CPU核心。

#### （2）限制内存使用
```bash
# 1. 创建memory子系统的cgroup
mkdir /sys/fs/cgroup/memory/my_app

# 2. 限制最大内存为1GB，swap为256MB
echo 1G > /sys/fs/cgroup/memory/my_app/memory.limit_in_bytes
echo 256M > /sys/fs/cgroup/memory/my_app/memory.memsw.limit_in_bytes

# 3. 开启OOM控制，超过内存限制就杀死进程
echo 1 > /sys/fs/cgroup/memory/my_app/memory.oom_control

# 4. 加入进程
echo 1234 > /sys/fs/cgroup/memory/my_app/tasks
```

#### （3）限制磁盘I/O
```bash
# 1. 创建blkio子系统的cgroup
mkdir /sys/fs/cgroup/blkio/my_app

# 2. 限制磁盘读速率为100MB/s，写速率为50MB/s
# 设备号可以通过lsblk查看，比如8:0对应/dev/sda
echo "8:0 104857600" > /sys/fs/cgroup/blkio/my_app/blkio.throttle.read_bps_device
echo "8:0 52428800" > /sys/fs/cgroup/blkio/my_app/blkio.throttle.write_bps_device

# 3. 加入进程
echo 1234 > /sys/fs/cgroup/blkio/my_app/tasks
```

### 3. 使用systemd管理cgroup
现代Linux系统使用systemd管理cgroup更方便：
```ini
# /etc/systemd/system/my_app.service
[Unit]
Description=My App Service

[Service]
# CPU限制：最多使用2个核心
CPUQuota=200%
# 内存限制：最多1GB
MemoryLimit=1G
# 块设备IO写限制：100MB/s
IOReadBandwidthMax=/dev/sda 100M
IOWriteBandwidthMax=/dev/sda 50M
# 进程数限制
TasksMax=1024

ExecStart=/path/to/my_app
User=appuser
Group=appgroup

[Install]
WantedBy=multi-user.target
```
启动服务后，systemd会自动创建对应的cgroup并应用限制。

---

## 六、典型应用场景

### 1. 容器技术（最主要应用）
Docker、Kubernetes等容器技术的底层核心：
- **Docker**：每个容器对应一个cgroup，限制容器的CPU、内存、I/O资源
- **Kubernetes**：通过cgroups实现Pod的资源requests和limits配置
- **资源QoS**：Kubernetes根据cgroups实现不同优先级Pod的服务质量保证

Kubernetes资源配置示例：
```yaml
resources:
  requests:
    cpu: "100m" # 申请0.1核CPU
    memory: "256Mi" # 申请256MB内存
  limits:
    cpu: "500m" # 最多使用0.5核CPU
    memory: "1Gi" # 最多使用1GB内存
```
这些配置最终都是通过cgroups实现的。

### 2. 云原生与Serverless
- **多租户隔离**：云平台上不同租户的应用通过cgroups隔离，防止互相影响
- **按需分配**：根据用户购买的规格分配对应的资源
- **弹性扩缩容**：动态调整cgroup限制实现资源弹性伸缩
- **Serverless函数**：每个函数实例运行在独立的cgroup中，严格限制资源使用

### 3. 服务资源隔离
在物理机/虚拟机上部署多个服务时：
- 前端服务、后端服务、数据库服务分别放在不同cgroup
- 防止某个服务突发流量占满所有资源影响其他服务
- 核心业务资源优先级高于非核心业务

### 4. 高性能计算（HPC）
- 给不同计算任务分配固定的CPU、内存、GPU资源
- 保证关键计算任务的资源独占
- 防止计算任务之间互相抢占资源导致性能下降

### 5. 批量作业调度
- 批量作业系统（如Slurm、YARN）通过cgroups管理作业资源
- 每个作业分配指定的CPU和内存配额
- 作业完成后自动释放资源给其他作业使用

### 6. 系统安全与故障隔离
- 限制潜在恶意进程的资源使用，防止DoS攻击
- 对不可信程序运行在受限cgroup中，防止耗尽系统资源
- 单个服务OOM只会杀死该cgroup内的进程，不会影响系统其他进程

---

## 七、常见问题与最佳实践

### 1. 常见问题
#### Q: cgroup的资源限制是硬限制还是软限制？
A: 默认是硬限制，超过限制会被内核阻止或OOM杀死，也可以配置软限制（如memory.soft_limit_in_bytes），超过软限制会优先回收该cgroup的内存，但不会直接杀死。

#### Q: 进程属于多个cgroup吗？
A: v1中进程可以属于不同子系统的多个cgroup，v2中进程只能属于一个cgroup。

#### Q: 子进程会继承父进程的cgroup吗？
A: 是的，子进程默认和父进程属于同一个cgroup，除非手动迁移。

#### Q: cgroup本身会占用内存吗？
A: 会，内核需要为每个cgroup维护数据结构，内存占用不大，一般每个cgroup几KB到几十KB。

### 2. 最佳实践
1. **分层配置**：按照业务层级配置cgroup，比如/system、/app、/test，方便统一管理
2. **避免过度限制**：资源限制要留有余量，防止业务高峰时被OOM杀死
3. **监控资源使用**：采集cgroup的资源统计数据，做监控和告警
4. **优先使用systemd管理**：比手动操作cgroupfs更安全可靠，配置也更简单
5. **逐步迁移到v2**：新系统优先考虑使用cgroups v2，获得更好的性能和安全性
6. **合理设置OOM优先级**：核心进程OOM优先级设置低一些，优先杀死非核心进程

### 3. 常见面试题
- Q: Docker容器的资源隔离是怎么实现的？
  A: 主要通过namespace做访问隔离，cgroups做资源限制。
- Q: cgroups和ulimit的区别是什么？
  A: ulimit是针对单个进程的资源限制，cgroups是针对进程组的，功能更强大，支持更多资源类型，支持层级继承。
- Q: 怎么检测cgroup的内存泄漏？
  A: 查看memory.usage_in_bytes的变化趋势，如果持续增长不下降说明可能有内存泄漏。
- Q: cgroups v2相比v1有什么优势？
  A: 统一层级结构，管理更简单，性能更高，安全性更好，支持新特性。
