# Huge Page（巨页）

## 定义与作用
巨页使用大于默认 4KB 的内存页面（通常 2MB 或 1GB），减少页表级数和 TLB Miss 次数，解决大内存工作负载下地址翻译开销过高的问题。

## 核心原理
- x86 默认 4KB 页面需要 4 级页表（PML4→PDPT→PD→PT），一次内存访问实际需要 5 次内存访问（4 次查页表 + 1 次取数据）
- TLB（Translation Lookaside Buffer）缓存页表翻译结果，典型 L1 TLB 只能缓存约 64 个条目，覆盖 64×4KB=256KB 内存
- 使用 2MB 巨页时，同样 64 个 TLB 条目可覆盖 128MB 内存，TLB Miss 率从 1-5% 降到 0.1% 以下
- 1GB 巨页只需 2 级页表，TLB Miss 几乎完全消除，但仅用于超大内存工作负载（如数据库 Buffer Pool）

## 软硬件协同点
1. **硬件页表遍历支持**：CPU CR3 寄存器指向 PML4，硬件自动完成多级页表遍历；巨页通过设置页表项中的 PS（Page Size）位跳过中间层级
2. **TLB 分层结构**：现代 CPU 有独立的 4KB TLB、2MB TLB 和 1GB TLB，使用不同巨页大小可利用对应层次的 TLB 缓存
3. **透明巨页（THP）**：内核在后台自动合并 4KB 页面为 2MB 巨页，对应用完全透明；但 THP 的页拆分（Split）会导致不可预测的延迟，对延迟敏感应用建议关闭
4. **内存预留与对齐**：巨页需要物理连续内存，系统启动时通过 `hugepagesz=2M hugepages=1024` 预留，运行时分配通过 hugetlbfs 文件系统，地址天然对齐到巨页大小

## 典型应用场景
1. **MySQL Buffer Pool**：MySQL InnoDB 将 Buffer Pool 全部配置为 2MB 巨页，TLB Miss 从 3% 降到 0.05%，随机查询 QPS 提升 20-40%
2. **DPDK 报文缓冲区**：DPDK 使用 1GB 巨页分配 mbuf，报文地址翻译零开销，DMA 直接操作物理地址，报文转发吞吐提升 30%
