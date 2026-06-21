# NUMA（非一致性内存访问）

## 定义与作用
NUMA 是多 CPU 插槽系统的内存架构，每个 CPU 有自己的本地内存，访问本地内存比访问其他 CPU 的远程内存快 2-3 倍，NUMA 优化解决跨节点内存访问导致的性能下降问题。

## 核心原理
- 传统 UMA（一致内存访问）架构中，所有 CPU 共享同一条内存总线，CPU 增多时总线成为瓶颈
- NUMA 将系统划分为多个节点（Node），每个节点包含一组 CPU 和一块本地内存，节点间通过高速互联（Intel QPI / AMD Infinity Fabric）连接
- 本地内存访问延迟约 70ns，远程访问延迟约 150-200ns，带宽也有 2-3 倍差距
- 操作系统通过 NUMA 策略（本地分配优先、交织分配等）控制内存分配行为

## 软硬件协同点
1. **NUMA 硬件拓扑发现**：内核通过 ACPI SRAT（System Resource Affinity Table）表检测硬件 NUMA 拓扑，用户空间通过 `numactl`、`libnuma` 查询和配置
2. **页迁移（Page Migration）**：Linux 支持在运行时将页面从一个 NUMA 节点迁移到另一个节点，当 `numad` 检测到大量远程访问时自动触发迁移
3. **NUMA 感知的调度器**：CFS 调度器在选择线程运行的 CPU 时，优先考虑线程最近运行过的 NUMA 节点，尽量减少跨节点迁移
4. **透明巨页（THP）与 NUMA**：THP 分配时会考虑 NUMA 节点，2MB 大页在节点边界对齐，一个大页不会跨两个节点，保证大页访问也是本地的

## 典型应用场景
1. **大型数据库 NUMA 分区**：Oracle/SQL Server 支持 NUMA 分区，每个 NUMA 节点运行独立的 Buffer Pool 和调度器，4 节点系统的 TPC-C 性能比不感知 NUMA 高 2.5 倍
2. **虚拟机 NUMA 拓扑透传**：KVM 支持将宿主机 NUMA 拓扑透传给虚拟机，虚拟机内部的应用可按本机 NUMA 策略优化，MySQL 性能提升 30% 以上
