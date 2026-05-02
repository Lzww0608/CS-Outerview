# Linux namespace 详解

## 一、什么是namespace
namespace（命名空间）是Linux内核提供的**全局资源隔离机制**，可以将系统全局资源封装到不同的namespace中，使得不同namespace中的进程拥有独立的资源视图，彼此之间互不感知。

它和cgroups一起构成了Linux容器技术的两大核心底层支柱：
- **namespace**：负责**资源视图隔离**（让进程看不到其他namespace的资源）
- **cgroups**：负责**资源使用限制**（限制进程最多能使用多少资源）

### 核心特性
1. **隔离性**：不同namespace中的资源完全透明，互相不可见
2. **轻量级**：内核级别的隔离，开销远低于虚拟机
3. **动态创建**：运行时可以动态创建新的namespace
4. **继承性**：子进程默认继承父进程所在的namespace
5. **安全隔离**：是实现容器安全隔离的核心机制

---

## 二、发展历史
- **2002年（Linux 2.4.19）**：第一个namespace——Mount namespace诞生，用于隔离文件系统挂载点
- **2006年（Linux 2.6.19）**：UTS namespace加入，隔离主机名和域名
- **2006年（Linux 2.6.24）**：IPC namespace加入，隔离进程间通信资源
- **2008年（Linux 2.6.24）**：PID namespace加入，隔离进程ID空间
- **2011年（Linux 3.8）**：Network namespace加入，隔离网络栈
- **2012年（Linux 3.8）**：User namespace加入，隔离用户和组ID
- **2020年（Linux 5.6）**：Time namespace加入，隔离系统时间

目前Linux内核共支持8种类型的namespace，覆盖了所有核心系统资源的隔离。

---

## 三、核心原理与8大namespace类型

### 实现原理
namespace的核心原理是**对系统全局资源进行封装，为每个namespace提供独立的资源实例**：
1. 内核为每个类型的namespace维护独立的资源实例
2. 每个进程关联一个namespace，只能看到自己namespace内的资源
3. 不同namespace中可以有相同标识的资源，彼此互不影响
4. 系统调用返回的资源ID都是相对于进程所在namespace的

每个进程的namespace信息可以在`/proc/[pid]/ns/`目录下查看：
```bash
ls -l /proc/$$/ns/
# 输出：
# lrwxrwxrwx 1 root root 0 May  1 23:00 cgroup -> 'cgroup:[4026531835]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 ipc -> 'ipc:[4026531839]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 mnt -> 'mnt:[4026531840]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 net -> 'net:[4026531992]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 pid -> 'pid:[4026531836]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 pid_for_children -> 'pid:[4026531836]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 time -> 'time:[4026531834]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 time_for_children -> 'time:[4026531834]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 user -> 'user:[4026531837]'
# lrwxrwxrwx 1 root root 0 May  1 23:00 uts -> 'uts:[4026531838]'
```
每个链接后面的数字是namespace的唯一ID，两个进程的ID相同说明它们在同一个namespace中。

---

### 8种namespace详解

#### 1. Mount namespace（mnt）
**隔离资源**：文件系统挂载点视图
**作用**：每个mnt namespace中的进程有独立的挂载点目录树，在一个namespace中挂载/卸载文件系统不会影响其他namespace。
**应用场景**：容器可以有自己的根文件系统，和宿主机完全隔离。
**示例**：
```bash
# 创建新的mount namespace并执行bash
unshare -m bash
# 在新namespace中挂载tmpfs
mount -t tmpfs tmpfs /mnt
# 查看挂载点，能看到tmpfs
mount | grep tmpfs
# 退出bash回到宿主机，宿主机看不到这个挂载
```

#### 2. UTS namespace（uts）
**隔离资源**：主机名和域名（UNIX Time-Sharing System）
**作用**：每个uts namespace可以有独立的主机名和域名，让容器看起来像独立的主机。
**应用场景**：每个Docker容器有自己的主机名。
**示例**：
```bash
unshare -u bash
hostname mycontainer
hostname # 输出mycontainer
# 宿主机主机名不变
```

#### 3. IPC namespace（ipc）
**隔离资源**：进程间通信资源（System V IPC、POSIX消息队列、信号量、共享内存）
**作用**：一个namespace中的进程只能和同namespace的进程通信，无法访问其他namespace的IPC资源。
**应用场景**：防止容器内的进程通过IPC影响宿主机或其他容器。
**注意**：不同IPC namespace中的相同key的消息队列是完全独立的。

#### 4. PID namespace（pid）
**隔离资源**：进程ID空间
**作用**：每个PID namespace有独立的PID编号空间，不同namespace中的进程可以有相同的PID号。
**特点**：
- PID namespace是层级结构，父namespace可以看到子namespace中的所有进程
- 子namespace看不到父namespace中的进程
- 每个PID namespace的PID从1开始，作为该namespace的init进程
- 进程在所有祖先namespace中都有对应的PID号
**应用场景**：容器内的PID 1进程和宿主机的PID 1完全独立，容器内的进程看不到宿主机的进程列表。
**示例**：
```bash
unshare -p --fork --mount-proc bash
ps aux # 只能看到当前namespace中的进程，PID从1开始
```

#### 5. Network namespace（net）
**隔离资源**：整个网络栈
**作用**：每个network namespace有独立的：
- 网络设备（网卡、lo接口）
- IP地址、路由表、端口号
- 防火墙规则、netfilter、socket
- 网络命名空间之间可以通过veth pair虚拟网卡通信
**应用场景**：每个容器有独立的网络栈，自己的IP和端口，不会和其他容器端口冲突。
**示例**：
```bash
# 创建新的network namespace
ip netns add mynet
# 查看namespace中的网络设备（只有lo接口）
ip netns exec mynet ip addr
# 给namespace添加veth网卡
ip link add veth0 type veth peer name veth1
ip link set veth1 netns mynet
# 配置IP
ip addr add 192.168.1.1/24 dev veth0
ip link set veth0 up
ip netns exec mynet ip addr add 192.168.1.2/24 dev veth1
ip netns exec mynet ip link set veth1 up
ip netns exec mynet ip link set lo up
# 互相ping通
ping 192.168.1.2
ip netns exec mynet ping 192.168.1.1
```

#### 6. User namespace（user）
**隔离资源**：用户ID和组ID空间、能力（capabilities）
**作用**：
- 同一个用户ID在不同namespace中可以有不同的UID/GID
- 可以将namespace内的root用户映射到namespace外的普通用户
- 进程在namespace内拥有root权限，但在namespace外只有普通用户权限
**应用场景**：容器安全的核心机制，防止容器内的root权限溢出到宿主机。
**示例**：
```bash
# 创建user namespace，将内部root(0)映射到外部的1000用户
unshare -r bash
id # 输出uid=0(root) gid=0(root) groups=0(root)
# 但在宿主机上看这个进程的uid还是1000
```

#### 7. Cgroup namespace（cgroup）
**隔离资源**：cgroup根目录视图
**作用**：每个cgroup namespace中的进程只能看到自己所在cgroup路径的内容，看不到完整的cgroup层级结构。
**应用场景**：防止容器内的进程感知到自己运行在cgroup限制中，增强隔离性。

#### 8. Time namespace（time）
**隔离资源**：系统时间
**作用**：每个time namespace可以有独立的系统时间，可以修改namespace内的时间而不影响宿主机和其他namespace。
**应用场景**：容器内的时间可以独立调整，用于测试时间相关的业务逻辑。

---

## 四、核心API与使用方式
namespace的操作主要通过三个系统调用完成：

### 1. clone() - 创建新进程并进入新namespace
```c
int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...);
```
通过flags参数指定要创建的namespace类型：
| Flag | 对应namespace |
|------|---------------|
| CLONE_NEWNS | Mount namespace |
| CLONE_NEWUTS | UTS namespace |
| CLONE_NEWIPC | IPC namespace |
| CLONE_NEWPID | PID namespace |
| CLONE_NEWNET | Network namespace |
| CLONE_NEWUSER | User namespace |
| CLONE_NEWCGROUP | Cgroup namespace |
| CLONE_NEWTIME | Time namespace |

示例：创建同时拥有独立UTS、PID、Network namespace的进程
```c
clone(child_func, stack, CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNET | SIGCHLD, NULL);
```

### 2. unshare() - 当前进程脱离原有namespace，进入新namespace
```c
int unshare(int flags);
```
和clone类似，但不会创建新进程，而是让当前进程进入新的namespace。我们平时用的`unshare`命令就是对这个系统调用的封装。

### 3. setns() - 让进程加入已有的namespace
```c
int setns(int fd, int nstype);
```
fd是`/proc/[pid]/ns/`目录下对应namespace文件的文件描述符，nstype指定namespace类型。
这个调用是`docker exec`命令的核心实现原理：让新的进程加入容器的namespace中，就可以进入容器内部执行命令。

---

## 五、典型应用场景

### 1. 容器技术（最核心应用）
Docker、Kubernetes等容器技术的底层就是组合使用多种namespace实现隔离：
| Docker隔离能力 | 对应namespace |
|----------------|----------------|
| 独立的根文件系统 | Mount namespace |
| 独立的主机名 | UTS namespace |
| 独立的进程间通信 | IPC namespace |
| 独立的进程ID空间 | PID namespace |
| 独立的网络栈 | Network namespace |
| 独立的用户权限 | User namespace |
| 独立的cgroup视图 | Cgroup namespace |
| 独立的系统时间 | Time namespace |

每个运行中的Docker容器本质就是一组隔离的namespace加上cgroups资源限制。

### 2. 沙箱环境
- **浏览器沙箱**：Chrome、Firefox等浏览器使用namespace隔离不同标签页的进程，防止恶意代码突破浏览器限制访问系统资源
- **安全沙箱**：运行不可信代码时使用namespace隔离，即使被攻破也不会影响主机系统
- **CI/CD沙箱**：持续集成的构建任务运行在独立namespace中，不同构建任务互不影响

### 3. 多租户隔离
- **PaaS平台**：云平台上不同租户的应用运行在独立namespace中，实现资源和安全隔离
- **虚拟主机**：共享主机上不同用户的服务运行在独立namespace中，互相看不到对方的进程和文件
- **Serverless平台**：每个函数实例运行在独立namespace中，实现强隔离

### 4. 测试与调试
- **网络测试**：创建独立network namespace测试网络配置，不会影响主机网络
- **软件兼容性测试**：在独立namespace中安装不同版本的依赖库，测试软件兼容性
- **时间测试**：使用time namespace修改系统时间，测试定时任务等时间敏感逻辑，不会影响主机时间

### 5. 进程安全加固
- **服务隔离**：将高危服务（如数据库、Web服务）运行在独立namespace中，即使被入侵也只能访问namespace内的资源
- **最小权限原则**：给服务只分配需要的namespace权限，减少攻击面
- **root权限降权**：使用user namespace将内部root映射为外部普通用户，防止权限溢出

---

## 六、常见问题与最佳实践

### 1. 常见问题
#### Q: namespace和虚拟机的区别是什么？
| 特性 | namespace（容器） | 虚拟机 |
|------|-------------------|--------|
| 隔离级别 | 内核级资源隔离 | 硬件级全虚拟化 |
| 开销 | 极低，几乎和原生进程一样 | 较高，需要模拟硬件 |
| 启动速度 | 毫秒级 | 秒级到分钟级 |
| 资源利用率 | 高，共享内核 | 低，每个VM有独立OS |
| 隔离强度 | 中等，共享同一个内核 | 强，完全独立 |
| 内核兼容性 | 只能运行和宿主机同内核的系统 | 可以运行任意操作系统 |

#### Q: namespace是完全安全的吗？
A: 不是，namespace的隔离强度不如虚拟机，因为共享同一个内核，存在内核漏洞逃逸的风险。需要配合其他安全机制（如Seccomp、AppArmor、SELinux）增强容器安全。

#### Q: 进程可以同时属于多个同类型的namespace吗？
A: 不可以，每个进程同一时间只能属于每个类型的一个namespace。

#### Q: namespace会被回收吗？
A: 当一个namespace中的所有进程都退出，并且没有其他引用（没有打开的文件描述符、没有bind mount）时，内核会自动回收这个namespace。

### 2. 最佳实践
1. **启用所有namespace**：生产环境运行容器时尽量启用所有类型的namespace，最大化隔离性
2. **使用User namespace**：一定要开启User namespace，将容器内root映射为外部普通用户，防止权限逃逸
3. **最小权限原则**：只给容器分配需要的能力（capabilities），不要使用--privileged特权模式
4. **配合安全机制**：结合Seccomp过滤系统调用，AppArmor/SELinux做强制访问控制
5. **定期更新内核**：namespace相关的安全漏洞大多通过内核补丁修复，保持内核版本更新
6. **避免共享namespace**：除非必要，不要让容器共享宿主机的PID、Network、IPC等namespace

### 3. 常见面试题
- Q: Docker的核心底层技术是什么？
  A: 主要是namespace实现资源隔离，cgroups实现资源限制，联合文件系统实现镜像管理。
- Q: User namespace的作用是什么？为什么能提高容器安全性？
  A: User namespace隔离了用户ID空间，可以将容器内的root用户映射到宿主机的普通用户，即使容器被攻破，攻击者在宿主机上也只有普通用户权限，无法对宿主机造成破坏。
- Q: PID namespace是层级结构吗？父namespace能看到子namespace的进程吗？
  A: 是的，PID namespace是层级的，父namespace可以看到子namespace中的所有进程，子namespace看不到父namespace的进程。
- Q: 容器内可以看到宿主机的进程吗？为什么？
  A: 默认不可以，因为容器有独立的PID namespace，只能看到自己namespace内的进程。如果启动容器时使用--pid=host共享宿主机PID namespace，就可以看到宿主机进程。
