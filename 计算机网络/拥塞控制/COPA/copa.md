

```cpp
/*
@Author: Lzww
@LastEditTime: 2025-10-12 17:18:37
@Description: Copa (Delay-based Congestion control) Algorithm Implementation
@Language: C++17
*/

#include "copa.h"
#include <algorithm>
#include <cstdint>
#include <cmath>

// Default constructor
Copa::Copa() 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::RENO), "Copa"),
      m_ssthresh(0x7fffffff),      // Initially very large
      m_cwnd(0),                    // Will be set based on MSS
      m_maxCwnd(65535),             // Default max window
      m_mode(CopaMode::SLOW_START), // Start in slow start
      m_minRTT(0xFFFFFFFF),         // Maximum initial value
      m_standingRTT(0),             // No standing RTT yet
      m_delta(DEFAULT_DELTA),       // Target 0.5 RTT queueing delay
      m_velocity(0.0),              // No velocity yet
      m_targetRate(0),              // Will be calculated
      m_useCompetitiveMode(false),  // Default to non-competitive
      m_competitiveDelta(1),        // 1 packet competitive delta
      m_lastRateUpdate(0),          // No update yet
      m_deliveredBytes(0),          // No bytes delivered
      m_ssExitThreshold(SS_EXIT_THRESHOLD_US),
      m_inSlowStart(true),
      m_prevDirection(0),           // No previous direction
      m_prevQueueingDelay(0.0),     // No previous delay
      m_rttCount(0),
      m_totalAckedBytes(0)
{
    InitializeParameters();
}

// Copy constructor
Copa::Copa(const Copa& other) 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::RENO), "Copa"),
      m_ssthresh(other.m_ssthresh),
      m_cwnd(other.m_cwnd),
      m_maxCwnd(other.m_maxCwnd),
      m_mode(other.m_mode),
      m_rttSamples(other.m_rttSamples),
      m_minRTT(other.m_minRTT),
      m_standingRTT(other.m_standingRTT),
      m_minRTTTimestamp(other.m_minRTTTimestamp),
      m_delta(other.m_delta),
      m_velocity(other.m_velocity),
      m_targetRate(other.m_targetRate),
      m_useCompetitiveMode(other.m_useCompetitiveMode),
      m_competitiveDelta(other.m_competitiveDelta),
      m_lastRateUpdate(other.m_lastRateUpdate),
      m_deliveredBytes(other.m_deliveredBytes),
      m_rttStart(other.m_rttStart),
      m_ssExitThreshold(other.m_ssExitThreshold),
      m_inSlowStart(other.m_inSlowStart),
      m_prevDirection(other.m_prevDirection),
      m_prevQueueingDelay(other.m_prevQueueingDelay),
      m_rttCount(other.m_rttCount),
      m_totalAckedBytes(other.m_totalAckedBytes)
{
}

// Destructor
Copa::~Copa() {
    // No dynamic memory to clean up
}

// Get type ID
TypeId Copa::GetTypeId() {
    return static_cast<TypeId>(CongestionAlgorithm::RENO);  // Using RENO as placeholder
}

// Get algorithm name
std::string Copa::GetAlgorithmName() {
    return "Copa";
}

// Get slow start threshold
uint32_t Copa::GetSsThresh(std::unique_ptr<SocketState>& socket, uint32_t bytesInFlight) {
    if (socket == nullptr) {
        return m_ssthresh;
    }

    // Copa: reduce to current cwnd * (1 - delta/2)
    m_ssthresh = static_cast<uint32_t>(socket->cwnd_ * (1.0 - m_delta / 2.0));
    
    // Ensure minimum of 2 MSS
    m_ssthresh = std::max(m_ssthresh, 2 * socket->mss_bytes_);
    
    socket->ssthresh_ = m_ssthresh;
    return m_ssthresh;
}

// Increase congestion window based on Copa model
void Copa::IncreaseWindow(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr || segmentsAcked == 0) {
        return;
    }

    // Update local state
    m_cwnd = socket->cwnd_;
    m_ssthresh = socket->ssthresh_;

    if (m_mode == CopaMode::SLOW_START) {
        // Slow start: exponential growth
        m_cwnd += segmentsAcked * socket->mss_bytes_;
        
        // Check if should exit slow start
        if (ShouldExitSlowStart()) {
            EnterVelocityMode();
        }
    } else {
        // Velocity mode or competitive mode: adjust based on rate
        UpdateCwndFromRate(socket);
    }

    // Ensure we don't exceed maximum window
    m_cwnd = std::min(m_cwnd, m_maxCwnd);
    m_cwnd = std::max(m_cwnd, 2 * socket->mss_bytes_);  // Minimum 2 MSS
    
    socket->cwnd_ = m_cwnd;
}

// Handle ACKed packets - core Copa logic
void Copa::PktsAcked(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked, const uint64_t rtt) {
    if (socket == nullptr || segmentsAcked == 0) {
        return;
    }

    // Update RTT information
    socket->rtt_us_ = static_cast<uint32_t>(rtt);
    
    // Update RTT variance
    if (socket->rtt_var_ == 0) {
        socket->rtt_var_ = rtt / 2;
    } else {
        socket->rtt_var_ = (3 * socket->rtt_var_ + rtt) / 4;
    }
    socket->rto_us_ = socket->rtt_us_ + 4 * socket->rtt_var_;
    
    // Calculate delivered bytes
    uint32_t ackedBytes = segmentsAcked * socket->mss_bytes_;
    m_deliveredBytes += ackedBytes;
    m_totalAckedBytes += ackedBytes;
    m_rttCount++;
    
    // Run Copa main update logic
    CopaUpdate(socket, ackedBytes, rtt);
}

// Set congestion state
void Copa::CongestionStateSet(std::unique_ptr<SocketState>& socket, const TCPState congestionState) {
    if (socket == nullptr) {
        return;
    }

    socket->tcp_state_ = congestionState;

    // When entering recovery or loss, adjust parameters
    if (congestionState == TCPState::Recovery || congestionState == TCPState::Loss) {
        GetSsThresh(socket, 0);
    }
}

// Handle congestion window events
void Copa::CwndEvent(std::unique_ptr<SocketState>& socket, const CongestionEvent congestionEvent) {
    if (socket == nullptr) {
        return;
    }

    socket->congestion_event_ = congestionEvent;

    switch (congestionEvent) {
        case CongestionEvent::PacketLoss:
            // Copa: moderate response to packet loss
            m_cwnd = static_cast<uint32_t>(m_cwnd * (1.0 - m_delta / 2.0));
            m_cwnd = std::max(m_cwnd, 4 * socket->mss_bytes_);
            socket->cwnd_ = m_cwnd;
            
            // Reset velocity
            m_velocity = 0.0;
            m_prevDirection = 0;
            break;

        case CongestionEvent::Timeout:
            // Timeout: reset to conservative state
            m_cwnd = 4 * socket->mss_bytes_;
            socket->cwnd_ = m_cwnd;
            socket->tcp_state_ = TCPState::Loss;
            EnterSlowStart();
            break;

        case CongestionEvent::ECN:
            // ECN: similar to packet loss
            m_cwnd = static_cast<uint32_t>(m_cwnd * (1.0 - m_delta / 2.0));
            m_cwnd = std::max(m_cwnd, 4 * socket->mss_bytes_);
            socket->cwnd_ = m_cwnd;
            socket->tcp_state_ = TCPState::CWR;
            break;

        case CongestionEvent::FastRecovery:
            socket->tcp_state_ = TCPState::Recovery;
            break;

        default:
            break;
    }
}

// Check if congestion control is enabled
bool Copa::HasCongControl() const {
    return true;
}

// Main congestion control logic
void Copa::CongControl(std::unique_ptr<SocketState>& socket, 
                       const CongestionEvent& congestionEvent,
                       const RTTSample& rtt) {
    if (socket == nullptr) {
        return;
    }

    // Handle the congestion event
    CwndEvent(socket, congestionEvent);

    // Update with RTT if valid
    if (rtt.rtt.count() > 0) {
        PktsAcked(socket, 1, rtt.rtt.count());
    }
}

// Enter slow start mode
void Copa::EnterSlowStart() {
    m_mode = CopaMode::SLOW_START;
    m_inSlowStart = true;
    m_velocity = 0.0;
    m_prevDirection = 0;
}

// Enter competitive mode
void Copa::EnterCompetitiveMode() {
    m_mode = CopaMode::COMPETITIVE;
    m_inSlowStart = false;
    m_velocity = 0.0;
}

// Enter velocity mode
void Copa::EnterVelocityMode() {
    m_mode = CopaMode::VELOCITY;
    m_inSlowStart = false;
    m_velocity = 0.0;
    m_prevQueueingDelay = GetQueueingDelay();
}

// Copa main update logic
void Copa::CopaUpdate(std::unique_ptr<SocketState>& socket, uint32_t ackedBytes, uint64_t rtt) {
    // Update RTT measurements
    UpdateRTT(rtt);
    
    // Clean up old samples
    CleanupOldRTTSamples();
    
    // Check for mode transitions
    CheckModeTransition();
    
    if (m_mode == CopaMode::VELOCITY || m_mode == CopaMode::COMPETITIVE) {
        // Calculate velocity based on queueing delay
        m_velocity = CalculateVelocity();
        
        // Calculate target rate
        m_targetRate = CalculateTargetRate();
    }
}

// Update RTT measurements
void Copa::UpdateRTT(uint64_t rtt) {
    if (rtt == 0) {
        return;
    }
    
    uint32_t rtt_us = static_cast<uint32_t>(rtt);
    
    // Add new sample
    m_rttSamples.push_back(CopaRTTMeasurement(rtt_us));
    
    // Keep only recent samples
    while (m_rttSamples.size() > RTT_SAMPLE_WINDOW) {
        m_rttSamples.pop_front();
    }
    
    // Update min RTT if this is smaller
    if (rtt_us < m_minRTT) {
        m_minRTT = rtt_us;
        m_minRTTTimestamp = std::chrono::steady_clock::now();
    }
    
    // Update standing RTT (recent average)
    if (!m_rttSamples.empty()) {
        uint64_t sum = 0;
        for (const auto& sample : m_rttSamples) {
            sum += sample.rtt_us;
        }
        m_standingRTT = static_cast<uint32_t>(sum / m_rttSamples.size());
    }
}

// Get minimum RTT
uint32_t Copa::GetMinRTT() const {
    return m_minRTT != 0xFFFFFFFF ? m_minRTT : 10000;  // Default 10ms if unknown
}

// Get standing queue delay
uint32_t Copa::GetStandingQueueDelay() const {
    if (m_minRTT == 0xFFFFFFFF || m_standingRTT == 0) {
        return 0;
    }
    
    // Queue delay = standing RTT - min RTT
    return m_standingRTT > m_minRTT ? (m_standingRTT - m_minRTT) : 0;
}

// Get queueing delay in units of RTTs
double Copa::GetQueueingDelay() const {
    uint32_t queueDelay = GetStandingQueueDelay();
    uint32_t minRTT = GetMinRTT();
    
    if (minRTT == 0) {
        return 0.0;
    }
    
    // Return as fraction of RTT
    return static_cast<double>(queueDelay) / static_cast<double>(minRTT);
}

// Calculate velocity for rate adjustment
double Copa::CalculateVelocity() {
    double currentQueueingDelay = GetQueueingDelay();
    
    // Calculate the direction of change
    int32_t direction = 0;
    if (currentQueueingDelay < m_delta) {
        direction = 1;   // Increase rate (less than target delay)
    } else if (currentQueueingDelay > m_delta) {
        direction = -1;  // Decrease rate (more than target delay)
    }
    
    // Velocity update based on Copa algorithm
    // v(t) = v(t-1) + δ * direction_change
    double velocityUpdate = 0.0;
    
    if (direction != m_prevDirection && m_prevDirection != 0) {
        // Direction changed, adjust velocity
        velocityUpdate = m_delta * direction;
    } else if (direction != 0) {
        // Same direction, continue with small adjustment
        velocityUpdate = m_delta * direction * 0.5;
    }
    
    double newVelocity = m_velocity + velocityUpdate * VELOCITY_GAIN;
    
    // Clamp velocity to reasonable bounds
    newVelocity = std::max(-1.0, std::min(1.0, newVelocity));
    
    m_prevDirection = direction;
    m_prevQueueingDelay = currentQueueingDelay;
    
    return newVelocity;
}

// Calculate target sending rate
uint32_t Copa::CalculateTargetRate() {
    if (m_minRTT == 0xFFFFFFFF || m_minRTT == 0) {
        return m_cwnd * 1000;  // Default rate
    }
    
    // Calculate current rate: cwnd / RTT
    // Rate in bytes per second
    double currentRate = static_cast<double>(m_cwnd) * 1000000.0 / static_cast<double>(m_minRTT);
    
    // Apply velocity: rate(t+1) = rate(t) * (1 + v(t) * δ)
    double rateMultiplier = 1.0 + m_velocity * m_delta;
    double targetRate = currentRate * rateMultiplier;
    
    // Ensure minimum rate
    targetRate = std::max(targetRate, 1000.0);  // At least 1 KB/s
    
    return static_cast<uint32_t>(targetRate);
}

// Update cwnd from target rate
void Copa::UpdateCwndFromRate(std::unique_ptr<SocketState>& socket) {
    if (socket == nullptr) {
        return;
    }
    
    if (m_targetRate == 0 || m_minRTT == 0xFFFFFFFF) {
        return;
    }
    
    // cwnd = rate * RTT
    // Convert: rate (bytes/sec) * RTT (microsec) / 1,000,000
    uint64_t newCwnd = (static_cast<uint64_t>(m_targetRate) * m_minRTT) / 1000000;
    
    // Smooth transition
    if (newCwnd > m_cwnd) {
        m_cwnd = std::min(static_cast<uint32_t>(newCwnd), m_cwnd + socket->mss_bytes_);
    } else if (newCwnd < m_cwnd) {
        m_cwnd = std::max(static_cast<uint32_t>(newCwnd), m_cwnd - socket->mss_bytes_);
    }
}

// Check if should exit slow start
bool Copa::ShouldExitSlowStart() {
    if (!m_inSlowStart) {
        return false;
    }
    
    // Exit if queueing delay exceeds threshold
    uint32_t queueDelay = GetStandingQueueDelay();
    return queueDelay > m_ssExitThreshold;
}

// Check for mode transitions
void Copa::CheckModeTransition() {
    // Check if min RTT measurement is stale
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - m_minRTTTimestamp);
    
    if (elapsed.count() >= MIN_RTT_WINDOW_SEC) {
        // Min RTT is stale, need to re-measure
        // Could transition to a probe mode here
    }
    
    // Transition from slow start to velocity mode
    if (m_mode == CopaMode::SLOW_START && ShouldExitSlowStart()) {
        EnterVelocityMode();
    }
}

// Clean up old RTT samples
void Copa::CleanupOldRTTSamples() {
    auto now = std::chrono::steady_clock::now();
    
    // Remove samples older than 10 seconds
    while (!m_rttSamples.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_rttSamples.front().timestamp);
        if (age.count() > 10) {
            m_rttSamples.pop_front();
        } else {
            break;
        }
    }
}

// Initialize parameters
void Copa::InitializeParameters() {
    m_minRTTTimestamp = std::chrono::steady_clock::now();
    m_rttStart = std::chrono::steady_clock::now();
}
```





## 核心原则：设定一个明确的排队延迟目标

Copa背后的基本思想虽然简单但非常强大：它试图在瓶颈路由器的缓冲区中维持一个小的、恒定的数据量。它既不希望缓冲区是空的（这意味着链路利用率不足），也不希望它是满的（这会导致高延迟，即所谓的“缓冲区膨胀” Bufferbloat）。

Copa的机制围绕一个由参数 `δ` (delta) 控制的目标排队延迟展开。该算法的目标是使测量到的排队延迟等于最小往返时间（RTT）的 `1/δ` 倍。`m_delta` 默认设置为 `0.5`，这意味着 **目标排队延迟是基础RTT的一半**。

> **打个比方：** 想象一下，你正在管理进入一条隧道（瓶颈链路）的交通流量。
>
> - **基于丢包的算法（如Reno）** 会不断地派更多的车进去，直到发生撞车（丢包），然后它们会大幅减速。
> - **Copa** 则在隧道入口处安装了一个传感器，用来测量车辆的等待时间。它的目标是始终保持一个小的、特定长度的等待队列（例如，总是有5辆车在排队）。如果队列增长到7辆车，它会稍微调慢红绿灯的频率。如果队列缩短到3辆车，它就会加快红绿灯。这样既能保持隧道满负荷高效运行，又不会造成大规模的交通堵塞。

------



## 关键指标及其计算方式

Copa依赖于精确的RTT测量来推断排队延迟。代码中计算并使用了几个关键指标：

1. **最小RTT (m_minRTT):** 这代表了传播延迟——即一个数据包在*没有任何排队等待*的情况下，往返所需的时间。它是RTT的基准线。
   - **在代码中:** `UpdateRTT` 函数会跟踪迄今为止观察到的最低RTT。它还有一个老化机制 (`MIN_RTT_WINDOW_SEC`)，以确保这个值不会因为网络路径变化而过时。
2. **常设RTT (m_standingRTT):** 这是近期的、平滑化的RTT值，它既包含了传播延迟，也包含了任何排队延迟。
   - **在代码中:** `UpdateRTT` 维护了一个近期RTT样本的列表 (`m_rttSamples`)，并将 `m_standingRTT` 计算为它们的平均值。这提供了一个稳定且能反映当前状况的RTT测量值。
3. **排队延迟 (Queueing Delay):** 这是Copa用来决策的关键信号。它是当前RTT与基础RTT之间的差值。
   - **在代码中:** `GetStandingQueueDelay` 函数通过 `m_standingRTT - m_minRTT` 来简单计算得出。而 `GetQueueingDelay` 函数则通过将这个延迟除以 `m_minRTT` 来进行归一化。

------



## 算法流程与运行模式

### 1. 慢启动 (`CopaMode::SLOW_START`)

和大多数TCP算法一样，Copa以慢启动开始，以快速探测可用带宽。

- **过程:** 拥塞窗口 (`m_cwnd`) 随着每个被确认的数据包指数级增长。
  - **在代码中:** 在 `IncreaseWindow` 函数中，当 `m_mode == CopaMode::SLOW_START` 时，窗口增长逻辑为： `m_cwnd += segmentsAcked * socket->mss_bytes_;`。
- **退出条件:** 当测量到的排队延迟超过某个阈值时，Copa会退出慢启动。这是与Reno（在丢包时退出）的关键区别。通过基于延迟退出，Copa避免了造成一个巨大的队列并引发首次丢包。
  - **在代码中:** `ShouldExitSlowStart` 函数检查 `GetStandingQueueDelay() > m_ssExitThreshold` 是否成立。如果为真，则调用 `EnterVelocityMode` 进入下一个模式。



### 2. 拥塞避免 (`CopaMode::VELOCITY`)

这是Copa稳态运行的核心。一旦它对可用带宽有了大致的了解，就会切换到基于“速度 (velocity)”的微调模式。

- **过程:** 此时的目标是收敛到能维持目标排队延迟 (`m_delta`) 的发送速率。它通过以下步骤迭代进行：
  1. **确定方向:** 它将当前的排队延迟与目标 `m_delta` 进行比较。
     - 如果 `当前延迟 < m_delta`，方向为 `+1` (我们需要加速)。
     - 如果 `当前延迟 > m_delta`，方向为 `-1` (我们需要减速)。
  2. **更新速度 (m_velocity):** “速度”是一个控制变量，它决定了改变发送速率的激进程度。如果方向发生反转（例如，之前在加速，但现在需要减速了），这意味着我们已经超调了目标，此时速度会发生更显著的变化。如果继续保持相同的方向，调整则会更加温和。这使得算法能够稳定地收敛。
     - **在代码中:** `CalculateVelocity` 函数实现了这个逻辑。它根据 `direction` 以及该方向自上次测量以来是否发生了变化来更新 `m_velocity`。
  3. **计算目标速率:** 新的发送速率是根据当前速率和“速度”共同计算出来的。
     - **在代码中:** `CalculateTargetRate` 使用公式 `targetRate = currentRate * (1.0 + m_velocity * m_delta)` 来计算新速率。
  4. **更新拥塞窗口:** 最后，算法使用公式 `cwnd = rate * RTT` 将这个目标发送速率转换回拥塞窗口大小。
     - **在代码中:** `UpdateCwndFromRate` 执行此转换，并平滑地调整 `m_cwnd`，每次RTT最多增加或减少一个MSS，以防止剧烈振荡。



### 3. 处理丢包和超时

尽管Copa是基于延迟的，但它仍然必须对丢包做出反应，因为丢包是严重拥塞的信号。

- **丢包/ECN:** 当发生丢包时，Copa会减小其窗口，但没有Reno那么激进。减小的幅度与 `δ` 成正比。
  - **在代码中:** 在 `CwndEvent` 中，对于 `PacketLoss` 或 `ECN` 事件，窗口会减小： `m_cwnd = static_cast<uint32_t>(m_cwnd * (1.0 - m_delta / 2.0));`。当 `delta = 0.5` 时，这是25%的降窗，比Reno的50%要温和。同时，速度 `m_velocity` 也会被重置为 `0`。
- **超时:** 超时是一个更严重的事件。Copa的反应是急剧减小窗口并重新进入慢启动。
  - **在代码中:** 对于 `Timeout` 事件，`m_cwnd` 被重置为一个很小的值 (`4 * mss`)，并调用 `EnterSlowStart()`。

------



## 代码与概念映射总结

| Copa核心概念     | C++ 实现细节                                                 |
| ---------------- | ------------------------------------------------------------ |
| **目标排队延迟** | `m_delta` 成员变量，默认值为 `0.5`。                         |
| **RTT测量**      | `m_minRTT` 用于基准线，`m_standingRTT` 用于近期平均值。在 `UpdateRTT` 中计算。 |
| **推断排队延迟** | `GetStandingQueueDelay()` 函数 (`m_standingRTT - m_minRTT`)。 |
| **慢启动阶段**   | `CopaMode::SLOW_START`，在 `IncreaseWindow` 中实现指数级 `m_cwnd` 增长。 |
| **退出慢启动**   | `ShouldExitSlowStart()` 检查延迟是否超过 `m_ssExitThreshold`。 |
| **拥塞避免**     | `CopaMode::VELOCITY`，算法的主要控制循环。                   |
| **速度控制**     | `m_velocity` 变量，在 `CalculateVelocity` 中根据延迟与 `m_delta` 的比较进行更新。 |
| **速率调整**     | `CalculateTargetRate()` 和 `UpdateCwndFromRate()` 根据速度调整 `m_cwnd`。 |
| **丢包响应**     | `CwndEvent` 函数处理 `CongestionEvent::PacketLoss`，进行适度的窗口减小。 |



## 结论

这份C++代码有效地实现了Copa算法，其重点在于其核心原则：**主动管理排队延迟，而不是被动地响应丢包**。通过持续测量RTT、推断排队延迟，并使用基于“速度”的控制机制，它能够智能地调整发送速率，使其在网络的最佳工作点附近徘徊。这使其能够在实现高带宽利用率的同时，将延迟保持在持续较低的水平，使其成为现代对延迟敏感的应用的绝佳选择。