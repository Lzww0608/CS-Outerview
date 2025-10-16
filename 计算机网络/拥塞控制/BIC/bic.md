```cpp
/*
@Author: Lzww
@LastEditTime: 2025-10-9 22:15:00
@Description: BIC (Binary Increase Congestion control) Algorithm Implementation
@Language: C++17
*/

#include "bic.h"
#include <algorithm>
#include <cstdint>
#include <cmath>

// Default constructor
BIC::BIC() 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::BIC), "BIC"),
      m_ssthresh(0x7fffffff),      // Initially very large
      m_cwnd(0),                    // Will be set based on MSS
      m_maxCwnd(65535),             // Default max window
      m_lastMaxCwnd(0),             // No previous max
      m_lastCwnd(0),                // No previous cwnd
      m_minWin(0),                  // Will be set on first reduction
      m_maxIncr(32),                // Smax = 32 segments (default)
      m_minIncr(1),                 // Smin = 1 segment
      m_beta(0.8),                  // Beta = 0.8 (less aggressive than 0.125)
      m_lowWindow(14),              // Low window threshold
      m_smoothPart(0),              // Smooth increase counter
      m_foundNewMax(false),         // Haven't found new max yet
      m_ackCount(0)                 // No ACKs counted yet
{
    m_epochStart = std::chrono::steady_clock::now();
}

// Copy constructor
BIC::BIC(const BIC& other) 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::BIC), "BIC"),
      m_ssthresh(other.m_ssthresh),
      m_cwnd(other.m_cwnd),
      m_maxCwnd(other.m_maxCwnd),
      m_lastMaxCwnd(other.m_lastMaxCwnd),
      m_lastCwnd(other.m_lastCwnd),
      m_minWin(other.m_minWin),
      m_maxIncr(other.m_maxIncr),
      m_minIncr(other.m_minIncr),
      m_beta(other.m_beta),
      m_lowWindow(other.m_lowWindow),
      m_smoothPart(other.m_smoothPart),
      m_foundNewMax(other.m_foundNewMax),
      m_ackCount(other.m_ackCount),
      m_epochStart(other.m_epochStart)
{
}

// Destructor
BIC::~BIC() {
    // No dynamic memory to clean up
}

// Get type ID
TypeId BIC::GetTypeId() {
    return static_cast<TypeId>(CongestionAlgorithm::BIC);
}

// Get algorithm name
std::string BIC::GetAlgorithmName() {
    return "BIC";
}

// Get slow start threshold
uint32_t BIC::GetSsThresh(std::unique_ptr<SocketState>& socket, uint32_t bytesInFlight) {
    if (socket == nullptr) {
        return m_ssthresh;
    }

    // BIC uses beta * cwnd as the new ssthresh
    m_lastMaxCwnd = socket->cwnd_;
    m_ssthresh = static_cast<uint32_t>(socket->cwnd_ * m_beta);
    
    // Ensure minimum of 2 MSS
    m_ssthresh = std::max(m_ssthresh, 2 * socket->mss_bytes_);
    
    socket->ssthresh_ = m_ssthresh;
    return m_ssthresh;
}

// Increase congestion window based on current state
void BIC::IncreaseWindow(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr || segmentsAcked == 0) {
        return;
    }

    // Update local state
    m_cwnd = socket->cwnd_;
    m_ssthresh = socket->ssthresh_;

    // Determine which phase we're in
    if (socket->tcp_state_ == TCPState::Recovery) {
        // Fast recovery
        m_cwnd = FastRecovery(socket, segmentsAcked);
    } else if (m_cwnd < m_ssthresh) {
        // Slow start phase
        m_cwnd = SlowStart(socket, segmentsAcked);
    } else {
        // Congestion avoidance phase - use BIC algorithm
        m_cwnd = CongestionAvoidance(socket, segmentsAcked);
    }

    // Ensure we don't exceed maximum window
    m_cwnd = std::min(m_cwnd, m_maxCwnd);
    socket->cwnd_ = m_cwnd;
}

// Handle ACKed packets
void BIC::PktsAcked(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked, const uint64_t rtt) {
    if (socket == nullptr) {
        return;
    }

    // Update RTT information
    socket->rtt_us_ = static_cast<uint32_t>(rtt);
    
    // Basic RTT variance calculation (simplified)
    if (socket->rtt_var_ == 0) {
        socket->rtt_var_ = rtt / 2;
    } else {
        socket->rtt_var_ = (3 * socket->rtt_var_ + rtt) / 4;
    }

    // Update RTO (simplified: RTO = RTT + 4 * RTT_VAR)
    socket->rto_us_ = socket->rtt_us_ + 4 * socket->rtt_var_;

    // Count ACKs for BIC algorithm
    m_ackCount += segmentsAcked;
}

// Set congestion state
void BIC::CongestionStateSet(std::unique_ptr<SocketState>& socket, const TCPState congestionState) {
    if (socket == nullptr) {
        return;
    }

    socket->tcp_state_ = congestionState;

    // When entering recovery or loss, adjust parameters
    if (congestionState == TCPState::Recovery || congestionState == TCPState::Loss) {
        GetSsThresh(socket, 0);
        m_minWin = m_ssthresh;
        m_foundNewMax = false;
    }
}

// Handle congestion window events
void BIC::CwndEvent(std::unique_ptr<SocketState>& socket, const CongestionEvent congestionEvent) {
    if (socket == nullptr) {
        return;
    }

    socket->congestion_event_ = congestionEvent;

    switch (congestionEvent) {
        case CongestionEvent::PacketLoss:
        case CongestionEvent::Timeout:
            // Save the last max cwnd
            if (socket->cwnd_ > m_lastMaxCwnd) {
                m_lastMaxCwnd = socket->cwnd_;
            }

            // Reduce window using BIC's beta factor
            GetSsThresh(socket, 0);
            m_minWin = m_ssthresh;
            m_foundNewMax = false;
            
            if (congestionEvent == CongestionEvent::Timeout) {
                // Timeout: reset cwnd to initial window
                m_cwnd = socket->mss_bytes_;
                socket->cwnd_ = m_cwnd;
                socket->tcp_state_ = TCPState::Loss;
                BicReset();
            } else {
                // Fast retransmit: enter recovery
                m_cwnd = m_ssthresh;
                socket->cwnd_ = m_cwnd;
                socket->tcp_state_ = TCPState::Recovery;
            }
            
            m_epochStart = std::chrono::steady_clock::now();
            m_ackCount = 0;
            break;

        case CongestionEvent::ECN:
            // ECN: similar to packet loss
            GetSsThresh(socket, 0);
            m_cwnd = m_ssthresh;
            socket->cwnd_ = m_cwnd;
            socket->tcp_state_ = TCPState::CWR;
            m_minWin = m_ssthresh;
            m_foundNewMax = false;
            break;

        case CongestionEvent::FastRecovery:
            socket->tcp_state_ = TCPState::Recovery;
            break;

        default:
            break;
    }
}

// Check if congestion control is enabled
bool BIC::HasCongControl() const {
    return true;
}

// Main congestion control logic
void BIC::CongControl(std::unique_ptr<SocketState>& socket, 
                      const CongestionEvent& congestionEvent,
                      const RTTSample& rtt) {
    if (socket == nullptr) {
        return;
    }

    // Handle the congestion event
    CwndEvent(socket, congestionEvent);

    // Update RTT if valid
    if (rtt.rtt.count() > 0) {
        PktsAcked(socket, 1, rtt.rtt.count());
    }
}

// Slow start: exponential growth (same as Reno)
uint32_t BIC::SlowStart(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr || segmentsAcked == 0) {
        return m_cwnd;
    }

    // Increase cwnd by segmentsAcked * MSS (exponential growth)
    uint32_t newCwnd = m_cwnd + (segmentsAcked * socket->mss_bytes_);
    
    // Don't exceed ssthresh in slow start
    if (newCwnd > m_ssthresh) {
        newCwnd = m_ssthresh;
    }

    return std::min(newCwnd, m_maxCwnd);
}

// Congestion avoidance: BIC algorithm
uint32_t BIC::CongestionAvoidance(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr || segmentsAcked == 0) {
        return m_cwnd;
    }

    // Update BIC state and get new window size
    BicUpdate(socket);

    return m_cwnd;
}

// Fast recovery: maintain cwnd
uint32_t BIC::FastRecovery(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr) {
        return m_cwnd;
    }

    // In recovery, maintain or slightly inflate window
    uint32_t newCwnd = m_cwnd + (segmentsAcked * socket->mss_bytes_);
    
    return std::min(newCwnd, m_maxCwnd);
}

// BIC-specific window update algorithm
void BIC::BicUpdate(std::unique_ptr<SocketState>& socket) {
    if (socket == nullptr) {
        return;
    }

    uint32_t mss = socket->mss_bytes_;
    m_ackCount++;

    // Calculate the target window size
    uint32_t targetWin = m_lastMaxCwnd;
    
    // If we haven't found a new max, use a large target
    if (!m_foundNewMax || m_lastMaxCwnd == 0) {
        targetWin = m_cwnd + m_maxIncr * mss;
    }

    // Calculate the distance to the target
    int32_t dist = (targetWin - m_cwnd) / mss;

    if (dist > static_cast<int32_t>(m_maxIncr)) {
        // We're far from target: additive increase with Smax
        m_cwnd += m_maxIncr * mss;
    } else if (dist > 0) {
        // Binary search increase
        // Use binary search to find the optimal window
        uint32_t increment;
        
        if (dist > static_cast<int32_t>(m_minIncr)) {
            // Binary search phase
            increment = (dist / 2) * mss;
            if (increment < m_minIncr * mss) {
                increment = m_minIncr * mss;
            }
        } else {
            // Linear increase near target
            increment = m_minIncr * mss;
        }
        
        m_cwnd += increment;
    } else {
        // We've reached or passed the target
        if (!m_foundNewMax) {
            // First time reaching the target
            m_foundNewMax = true;
            m_lastMaxCwnd = m_cwnd;
        }
        
        // Slow increase beyond previous max
        if (m_cwnd < m_lastMaxCwnd + m_maxIncr * mss) {
            m_cwnd += m_minIncr * mss;
        } else {
            m_cwnd += m_maxIncr * mss;
            m_lastMaxCwnd = m_cwnd;
        }
    }

    // Ensure minimum window size
    if (m_cwnd < m_minWin) {
        m_cwnd = m_minWin;
    }
}

// Reset BIC state
void BIC::BicReset() {
    m_lastMaxCwnd = 0;
    m_lastCwnd = 0;
    m_minWin = 0;
    m_foundNewMax = false;
    m_ackCount = 0;
    m_smoothPart = 0;
    m_epochStart = std::chrono::steady_clock::now();
}
```





## 核心思想：二分搜索 (Binary Search)

传统 TCP Reno 在进入拥塞避免阶段后，窗口大小是**线性增长**的（每个 RTT 增加 1个 MSS）。在带宽很高、延迟也很大的网络中，这种线性增长速度太慢了。比如，当窗口从 500 MSS 减半到 250 MSS 后，要恢复到 500 MSS 需要 250 个 RTT，这个过程会浪费大量时间，导致带宽利用率很低。

**BIC 的核心思想是：** 当发生拥塞导致窗口减小后，网络新的平衡点（最优窗口值）应该在**上一次拥塞发生时的窗口值 W_max** 和**当前窗口值 W_min** 之间。与其像 Reno 那样慢慢地线性探测，不如用**二分搜索**的方式快速逼近这个目标点。

这个过程就像猜数字游戏：

- **Reno 的方式**：目标是 500，我从 250 开始，猜 251, 252, 253... 一步步猜过去。
- **BIC 的方式**：目标在 250 到 500 之间，我先猜中间值 (250+500)/2 = 375。如果没问题，再猜 (375+500)/2 = 437... 这样能更快地接近目标。

------



## 算法阶段详解 (结合代码分析)

我们可以从代码的逻辑中清晰地看到 BIC 的几个关键阶段。整个流程的核心在 `IncreaseWindow` 函数中，当 `m_cwnd >= m_ssthresh` 时，它会调用 `CongestionAvoidance`，而后者则依赖于 `BicUpdate` 函数来实现 BIC 的增长逻辑。

### 1. 拥塞事件处理 (Multiplicative Decrease)

当检测到丢包或超时（即发生拥塞）时，会调用 `CwndEvent` 函数。

- **记录最大窗口 W_max**:

  ```cpp
// in CwndEvent function
  if (socket->cwnd_ > m_lastMaxCwnd) {
    m_lastMaxCwnd = socket->cwnd_;
  }
  ```
  
  在窗口减小之前，代码将当前的拥塞窗口 `cwnd_` 记录在 `m_lastMaxCwnd` 中。这就是 BIC 的“记忆”，记住了上次网络能够承受的最大窗口值。
  
- **窗口乘性减小**:

  ```cpp
// GetSsThresh calls this
  m_ssthresh = static_cast<uint32_t>(socket->cwnd_ * m_beta); 
m_cwnd = m_ssthresh;
  ```
  
  代码使用一个参数 m_beta (0.8) 来进行乘性减小。新的 ssthresh 和 cwnd 都被设置为当前窗口的 m_beta 倍。这个值 m_ssthresh 也就是我们前面提到的 W_min。
  
  注意: BIC 的 beta 通常是 0.8，比 Reno 的 0.5 更“温和”，减小得更少，这也是为了在高速网络下更快地恢复。



### 2. 窗口增长阶段 (Congestion Avoidance)

拥塞发生后，窗口从 `W_min` 开始增长。`BicUpdate` 函数完美地体现了 BIC 的增长策略。



#### a. 二分搜索增长 (Binary Search Increase)

这是 BIC 最具特色的阶段。当当前窗口 `m_cwnd` 还在 `m_lastMaxCwnd` （即 `W_max`）的下方时，算法采用二分搜索的方式向 `W_max` 逼近。

```cpp
// in BicUpdate function
int32_t dist = (targetWin - m_cwnd) / mss; // targetWin is m_lastMaxCwnd

if (dist > 0) {
    // Binary search increase
    uint32_t increment;
    if (dist > static_cast<int32_t>(m_minIncr)) {
        // Binary search phase
        increment = (dist / 2) * mss; // 增量是目标距离的一半
        // ...
    } else {
        // Linear increase near target
        increment = m_minIncr * mss;
    }
    m_cwnd += increment;
}
```

- **核心逻辑**: 增量 `increment` 被设置为当前窗口与目标窗口 `targetWin`（即 `m_lastMaxCwnd`）距离的一半。这使得窗口大小呈**凹形曲线 (Concave)** 快速增长。离目标越远，增长得越快；离目标越近，增长得越慢，以避免冲过头。
- 当非常接近目标时（`dist` 很小），它会切换到小的线性增长（`m_minIncr`），进行微调。



#### b. 最大值探测 (Max Probing)

当 `m_cwnd` 增长到超过了上一次的最大值 `m_lastMaxCwnd` 后，意味着当前的网络状况可能比之前更好了。此时，BIC 需要去探测一个新的、更高的 `W_max`。这个阶段分为两部分：

```cpp
// in BicUpdate, the 'else' block when dist <= 0
// We've reached or passed the target

// Slow increase beyond previous max (Additive Increase)
if (m_cwnd < m_lastMaxCwnd + m_maxIncr * mss) {
    m_cwnd += m_minIncr * mss;
} 
// Fast increase to find new max quickly (Second Additive Increase)
else {
    m_cwnd += m_maxIncr * mss;
    m_lastMaxCwnd = m_cwnd; // Found a new potential max
}
```

1. **慢速探测 (Slow Probing)**: 刚超过 `m_lastMaxCwnd` 时，算法非常谨慎，采用一个很小的增量 `m_minIncr` (通常是 1 MSS) 慢慢增加窗口，以确保稳定性。
2. **快速探测 (Fast Probing)**: 如果慢速探测了一小段距离（由 `m_maxIncr` 控制）后仍然没有发生拥塞，BIC 会变得“自信”，认为网络状况确实很好，于是切换到一个大的线性增量 `m_maxIncr` (代码中是 32 MSS) 来快速寻找新的网络上限。
   - 这个从“慢”到“快”的探测过程，使得窗口增长曲线在超过 `W_max` 后呈现**凸形 (Convex)**。

整个增长过程结合起来，就是一条**先凹后凸**的平滑曲线，实现了**稳定、公平且高效**的窗口增长。



### 3. 其他阶段

- **慢启动 (Slow Start)**: 在 `SlowStart` 函数中，BIC 的行为和标准 TCP 一样，都是指数级增长，直到 `cwnd` 达到 `ssthresh`。

  ```cpp
uint32_t newCwnd = m_cwnd + (segmentsAcked * socket->mss_bytes_);
  ```

- **快速恢复 (Fast Recovery)**: 代码中的 `FastRecovery` 阶段逻辑也比较标准，主要用于在快速重传后恢复数据传输，期间窗口也会适度膨胀。

------



## 总结：BIC 的思路与优势

1. **可扩展性 (Scalability)**: 通过二分搜索，BIC 的窗口增长不再严重依赖于 RTT。无论 RTT 多大，它都能在几次增长后快速收敛到目标窗口，因此非常适合高带宽、高延迟的网络。
2. **TCP 友好性 (TCP Friendliness)**: 当 `W_max` 很小（即在低速网络中）时，二分搜索的窗口期会很短，其行为退化得接近于标准的线性增长 TCP，因此能和 Reno 等传统算法公平共存。
3. **稳定性和公平性**: 先凹后凸的增长函数（也称 S 型增长曲线）使得算法在接近饱和点时变得保守，减少了数据包丢失的概率，并在多个 BIC 流之间提供了更好的公平性。

总而言之，**BIC 通过引入“记忆”（W_max）和“二分搜索”机制，将拥塞窗口的恢复从一个缓慢的线性搜索过程，转变为一个高效的、对数时间的搜索过程，极大地提升了 TCP 在现代高速网络环境下的性能。** 后来大名鼎鼎的 **CUBIC** 算法，就是基于 BIC 的思想，使用一个三次函数（Cubic Function）来模拟这条优美的 S 型增长曲线，从而简化了算法的实现。