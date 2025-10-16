

TCP Vegas 是一种**前瞻性 (Proactive)** 的拥塞控制算法，与我们熟知的 TCP Reno/Cubic 这类**反应性 (Reactive)** 算法有本质区别。反应性算法通常以**丢包**作为网络拥塞的信号，而 Vegas 则试图在丢包发生**之前**就预见到拥塞，并主动调整发送速率。

------



## 核心思想：通过RTT测量网络“缓冲”

Vegas 的核心思想是：**通过精确测量往返时间 (RTT)，估算网络路径中路由器的缓冲区里积压了多少数据，并试图将这个积压的数据量维持在一个稳定且较小的范围内。**

想象一下一条高速公路：

- **BaseRTT**：深夜时分，路上几乎没车，你从A点到B点再返回所需的最短时间。这代表了网络路径的**固有传播延迟**，没有任何排队等待。
- **CurrentRTT**：高峰时段，路上车很多，你在收费站和拥堵路段需要排队，花费的时间更长。这代表了当前数据包的实际 RTT，包含了**传播延迟 + 排队延迟**。
- **排队延迟** (`CurrentRTT - BaseRTT`)：这个差值直接反映了路上（网络缓冲区中）有多“堵”。

Vegas 就是通过监控这个“排队延迟”来判断网络状况的。

------



## 关键指标与计算

从代码中，我们可以看到 Vegas 依赖几个核心变量和计算逻辑。



### 1. BaseRTT (最小往返时间)

这是 Vegas 算法的基石。

- **定义**：`m_baseRTT` 变量存储了迄今为止观测到的最小 RTT。理论上，它代表了数据包在没有任何排队的情况下往返所需的时间。
- **代码实现**：
  - 在 `PktsAcked` 函数中，每次收到 ACK，都会更新当前的 RTT (`m_currentRTT`)。
  - `UpdateBaseRTT` 函数负责维护 `m_baseRTT`。它并不仅仅是取历史最小值，为了适应网络路径的变化，它会定期（例如代码中的 `BASE_RTT_WINDOW_SEC`）检查 `m_baseRTT` 是否“过时”，如果过时了，就会从最近的一批 RTT 样本 (`m_rttSamples`) 中重新选出最小值作为新的 `BaseRTT`。这使得 Vegas 对网络路径变化具有更好的适应性。



### 2. 期望吞吐率 vs 实际吞吐率

Vegas 通过比较“期望能达到的速率”和“实际达到的速率”来量化网络中的缓冲数据。

- **期望速率 (Expected Rate)**：如果没有排队延迟，网络能达到的最大速率。
  - 计算公式：`Expected = cwnd / BaseRTT`
- **实际速率 (Actual Rate)**：当前实际测量到的数据传输速率。
  - 计算公式：`Actual = cwnd / CurrentRTT`



### 3. 核心差值 `Diff`

这是 Vegas 做出决策的核心依据。

- **定义**：`Diff = Expected - Actual`

- **物理意义**：这个差值 `Diff` 正比于网络路径中**额外积压的数据包数量**。

  - `Diff = cwnd/BaseRTT - cwnd/CurrentRTT = cwnd * (CurrentRTT - BaseRTT) / (BaseRTT * CurrentRTT)`
  - 由于 `CurrentRTT` 和 `BaseRTT` 通常很接近，可以简化为：`Diff ≈ cwnd * (CurrentRTT - BaseRTT) / BaseRTT`

- **代码实现**：`CalculateDiff` 函数完美地实现了这个逻辑。

  ```cpp
  int32_t Vegas::CalculateDiff() {
      // ... 省略边界检查 ...
      int32_t rttDiff = m_currentRTT - m_baseRTT;
      uint32_t cwndSegments = m_cwnd / 1460; // 将窗口大小从字节转换为段
      int32_t diff = (cwndSegments * rttDiff) / m_baseRTT;
      return diff;
  }
  ```

  这个 `diff` 的计算结果，单位就是“数据段 (segments)”，直观地表示了网络中多缓冲了大约多少个数据包。

------



## 拥塞控制阶段分析

Vegas 在不同的网络阶段有不同的策略，这些策略都围绕着 `Diff` 值展开。

### 1. 慢启动 (Slow Start)

在慢启动阶段，`cwnd` 会指数级增长。但与传统 TCP 不同，Vegas 会利用 `gamma` 阈值来**提前退出慢启动**，避免因窗口增长过快而导致的丢包。

- **代码实现**：在 `SlowStart` 函数中，除了标准的指数增长逻辑，还有一段关键代码：

  ```cpp
  if (m_enableSlowStart && m_doingVegasNow && ShouldExitSlowStart()) {
      // 提前退出慢启动
      m_ssthresh = m_cwnd;
      return m_cwnd;
  }
  ```

- `ShouldExitSlowStart()` 函数的判断依据就是 `diff > m_gamma`。`m_gamma` 通常是一个较小的值（例如1或2个数据段）。

- **思路**：当 Vegas 检测到网络中开始出现少量积压数据（`Diff > gamma`）时，它就认为带宽已经基本被填满了，继续指数增长会立刻导致拥塞。因此，它会立即停止慢启动，进入拥塞避免阶段，从而实现平滑过渡，有效避免了传统慢启动结束时常发生的“过冲(overshoot)”和丢包。



### 2. 拥塞避免 (Congestion Avoidance)

这是 Vegas 算法最核心的部分，它通过两个阈值 `alpha` 和 `beta` 来精细地调节 `cwnd`，试图将网络中的额外缓冲数据量维持在 `[alpha, beta]` 这个区间内。

- **代码实现**：在 `VegasUpdate` 函数中：

  ```cpp
  int32_t diff = CalculateDiff();
  
  if (diff < static_cast<int32_t>(m_alpha)) {
      // 积压数据太少，网络未充分利用 -> 增加 cwnd
      m_cwnd += mss;
  } else if (diff > static_cast<int32_t>(m_beta)) {
      // 积压数据太多，有拥塞风险 -> 减少 cwnd
      m_cwnd -= mss;
  }
  // else: diff 在 alpha 和 beta 之间，状态理想 -> 保持 cwnd 不变
  ```

- **决策逻辑**：

  - **Diff < alpha**：表示网络中的缓冲数据过少，链路带宽可能没有被充分利用。Vegas 会在下一个 RTT 将 `cwnd` **线性增加** 1个 MSS。
  - **Diff > beta**：表示网络中的缓冲数据过多，拥塞的风险正在增加。Vegas 会在下一个 RTT 将 `cwnd` **线性减小** 1个 MSS。
  - **alpha <= Diff <= beta**：表示网络处于一个非常理想的平衡状态，既充分利用了带宽，又没有造成过多的排队延迟。此时 Vegas 会**保持 cwnd 不变**。

  通过这种方式，Vegas 的 `cwnd` 会在一个理想的工作点附近进行微调，而不是像 Reno 那样持续增长直到发生丢包。



### 3. 丢包处理 (Packet Loss Handling)

尽管 Vegas 尽力避免丢包，但它仍然需要处理丢包事件（比如网络瞬时拥堵）。

- **代码实现**：在 `CwndEvent` 函数中，当发生 `PacketLoss` 或 `Timeout` 时，Vegas 的行为与 Reno 非常相似：
  - 将慢启动阈值 `m_ssthresh` 减半。
  - 大幅降低 `m_cwnd`。
  - **关键点**：调用 `DisableVegas()` 或 `ResetVegasState()`，暂时禁用 Vegas 的延迟判断机制，退回到传统的、更保守的恢复模式。这是因为一旦发生丢包，说明基于延迟的预测已经失败，网络状况非常糟糕，此时必须采取更激进的措施来缓解拥塞。

------



## 思路总结

结合代码，我们可以总结出 Vegas 算法的整体思路：

1. **主动探测，而非被动等待**：它不等待丢包，而是通过 RTT 的变化主动探测网络缓冲区的占用情况。
2. **量化拥塞程度**：通过 `Diff` 值，它将抽象的“网络拥塞”量化为具体的“额外缓冲数据包数量”，使得决策更加精确。
3. **三区精细控制**：在拥塞避免阶段，通过 `alpha` 和 `beta` 两个阈值，将网络状态划分为“利用不足”、“理想”和“趋于拥塞”三个区域，并采取不同的增、减、不变策略，力求将系统稳定在最高效的运行点。
4. **智能慢启动**：通过 `gamma` 阈值提前结束慢启动，避免了传统算法的“过冲”问题，使网络进入稳定状态的过程更平滑。
5. **保守的失败回退**：当主动预测失败（发生丢包）时，它会立刻退回到类似 Reno 的反应式模式，保证了网络的稳定性和安全性。

总的来说，Vegas 是一种非常精巧和智能的拥塞控制算法。它的核心优势在于能够**在不丢包的情况下充分利用网络带宽**，从而获得更高的吞吐量和更低的时延。