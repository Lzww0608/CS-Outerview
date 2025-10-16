```cpp
/*
@Author: Lzww
@LastEditTime: 2025-10-11 20:56:57
@Description: CUBIC Congestion Control Algorithm Implementation
@Language: C++17
*/

#include "cubic.h"
#include <algorithm>
#include <cstdint>
#include <cmath>

// Default constructor
Cubic::Cubic() 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::CUBIC), "CUBIC"),
      m_ssthresh(0x7fffffff),      // Initially very large
      m_cwnd(0),                    // Will be set based on MSS
      m_maxCwnd(65535),             // Default max window
      m_lastMaxCwnd(0),             // No previous max (W_max)
      m_lastCwnd(0),                // No previous cwnd
      m_k(0.0),                     // Time to reach W_max
      m_cubicBeta(0.7),             // Beta = 0.7 (CUBIC standard)
      m_cubicC(0.4),                // C = 0.4 (CUBIC standard)
      m_fastConvergence(true),      // Fast convergence enabled
      m_tcpFriendly(true),          // TCP-friendly mode enabled
      m_tcpCwnd(0),                 // TCP Reno estimate
      m_lastTime(0.0),              // No time elapsed yet
      m_ackCount(0),                // No ACKs counted yet
      m_cntRtt(0),                  // RTT counter
      m_delayMin(0xFFFFFFFF),       // Maximum initial value
      m_hystartEnabled(true),       // Hystart enabled by default
      m_hystartAckDelta(2),         // ACK delta threshold
      m_hystartDelayMin(0xFFFFFFFF),// Minimum delay in round
      m_hystartDelayMax(0)          // Maximum delay in round
{
    m_epochStart = std::chrono::steady_clock::now();
}

// Copy constructor
Cubic::Cubic(const Cubic& other) 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::CUBIC), "CUBIC"),
      m_ssthresh(other.m_ssthresh),
      m_cwnd(other.m_cwnd),
      m_maxCwnd(other.m_maxCwnd),
      m_lastMaxCwnd(other.m_lastMaxCwnd),
      m_lastCwnd(other.m_lastCwnd),
      m_k(other.m_k),
      m_cubicBeta(other.m_cubicBeta),
      m_cubicC(other.m_cubicC),
      m_fastConvergence(other.m_fastConvergence),
      m_tcpFriendly(other.m_tcpFriendly),
      m_tcpCwnd(other.m_tcpCwnd),
      m_epochStart(other.m_epochStart),
      m_lastTime(other.m_lastTime),
      m_ackCount(other.m_ackCount),
      m_cntRtt(other.m_cntRtt),
      m_delayMin(other.m_delayMin),
      m_hystartEnabled(other.m_hystartEnabled),
      m_hystartAckDelta(other.m_hystartAckDelta),
      m_hystartDelayMin(other.m_hystartDelayMin),
      m_hystartDelayMax(other.m_hystartDelayMax)
{
}

// Destructor
Cubic::~Cubic() {
    // No dynamic memory to clean up
}

// Get type ID
TypeId Cubic::GetTypeId() {
    return static_cast<TypeId>(CongestionAlgorithm::CUBIC);
}

// Get algorithm name
std::string Cubic::GetAlgorithmName() {
    return "CUBIC";
}

// Get slow start threshold
uint32_t Cubic::GetSsThresh(std::unique_ptr<SocketState>& socket, uint32_t bytesInFlight) {
    if (socket == nullptr) {
        return m_ssthresh;
    }

    // CUBIC uses beta * cwnd as the new ssthresh
    m_lastCwnd = socket->cwnd_;
    
    // Fast convergence: if cwnd < last_max_cwnd, further reduce W_max
    if (m_fastConvergence && socket->cwnd_ < m_lastMaxCwnd) {
        m_lastMaxCwnd = static_cast<uint32_t>(socket->cwnd_ * (2.0 - m_cubicBeta) / 2.0);
    } else {
        m_lastMaxCwnd = socket->cwnd_;
    }
    
    m_ssthresh = static_cast<uint32_t>(socket->cwnd_ * m_cubicBeta);
    
    // Ensure minimum of 2 MSS
    m_ssthresh = std::max(m_ssthresh, 2 * socket->mss_bytes_);
    
    socket->ssthresh_ = m_ssthresh;
    
    // Calculate K (time to reach W_max)
    CalculateK();
    
    return m_ssthresh;
}

// Increase congestion window based on current state
void Cubic::IncreaseWindow(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
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
        // Congestion avoidance phase - use CUBIC algorithm
        m_cwnd = CongestionAvoidance(socket, segmentsAcked);
    }

    // Ensure we don't exceed maximum window
    m_cwnd = std::min(m_cwnd, m_maxCwnd);
    socket->cwnd_ = m_cwnd;
}

// Handle ACKed packets
void Cubic::PktsAcked(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked, const uint64_t rtt) {
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

    // Track minimum delay
    if (rtt > 0 && rtt < m_delayMin) {
        m_delayMin = static_cast<uint32_t>(rtt);
    }

    // Hystart delay tracking
    if (m_hystartEnabled && m_cwnd < m_ssthresh) {
        if (rtt < m_hystartDelayMin) {
            m_hystartDelayMin = static_cast<uint32_t>(rtt);
        }
        if (rtt > m_hystartDelayMax) {
            m_hystartDelayMax = static_cast<uint32_t>(rtt);
        }
        
        // Check if we should exit slow start early (Hystart)
        if (m_hystartDelayMax - m_hystartDelayMin > m_hystartAckDelta) {
            // Exit slow start - set ssthresh to current cwnd
            m_ssthresh = m_cwnd;
            socket->ssthresh_ = m_ssthresh;
        }
    }

    // Count ACKs for CUBIC algorithm
    m_ackCount += segmentsAcked;
    m_cntRtt++;
}

// Set congestion state
void Cubic::CongestionStateSet(std::unique_ptr<SocketState>& socket, const TCPState congestionState) {
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
void Cubic::CwndEvent(std::unique_ptr<SocketState>& socket, const CongestionEvent congestionEvent) {
    if (socket == nullptr) {
        return;
    }

    socket->congestion_event_ = congestionEvent;

    switch (congestionEvent) {
        case CongestionEvent::PacketLoss:
        case CongestionEvent::Timeout:
            // Reduce window using CUBIC's beta factor
            GetSsThresh(socket, 0);
            
            if (congestionEvent == CongestionEvent::Timeout) {
                // Timeout: reset cwnd to initial window
                m_cwnd = socket->mss_bytes_;
                socket->cwnd_ = m_cwnd;
                socket->tcp_state_ = TCPState::Loss;
                CubicReset();
            } else {
                // Fast retransmit: enter recovery
                m_cwnd = m_ssthresh;
                socket->cwnd_ = m_cwnd;
                socket->tcp_state_ = TCPState::Recovery;
            }
            
            // Reset epoch
            m_epochStart = std::chrono::steady_clock::now();
            m_lastTime = 0.0;
            m_ackCount = 0;
            m_tcpCwnd = 0;
            
            // Reset Hystart
            m_hystartDelayMin = 0xFFFFFFFF;
            m_hystartDelayMax = 0;
            break;

        case CongestionEvent::ECN:
            // ECN: similar to packet loss
            GetSsThresh(socket, 0);
            m_cwnd = m_ssthresh;
            socket->cwnd_ = m_cwnd;
            socket->tcp_state_ = TCPState::CWR;
            m_epochStart = std::chrono::steady_clock::now();
            m_lastTime = 0.0;
            break;

        case CongestionEvent::FastRecovery:
            socket->tcp_state_ = TCPState::Recovery;
            break;

        default:
            break;
    }
}

// Check if congestion control is enabled
bool Cubic::HasCongControl() const {
    return true;
}

// Main congestion control logic
void Cubic::CongControl(std::unique_ptr<SocketState>& socket, 
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

// Slow start: exponential growth (same as Reno, with optional Hystart)
uint32_t Cubic::SlowStart(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr || segmentsAcked == 0) {
        return m_cwnd;
    }

    // Increase cwnd by segmentsAcked * MSS (exponential growth)
    uint32_t newCwnd = m_cwnd + (segmentsAcked * socket->mss_bytes_);
    
    // Don't exceed ssthresh in slow start
    if (newCwnd > m_ssthresh) {
        newCwnd = m_ssthresh;
        // Reset Hystart when exiting slow start
        m_hystartDelayMin = 0xFFFFFFFF;
        m_hystartDelayMax = 0;
    }

    return std::min(newCwnd, m_maxCwnd);
}

// Congestion avoidance: CUBIC algorithm
uint32_t Cubic::CongestionAvoidance(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr || segmentsAcked == 0) {
        return m_cwnd;
    }

    // Update CUBIC state and get new window size
    CubicUpdate(socket);

    return m_cwnd;
}

// Fast recovery: maintain cwnd
uint32_t Cubic::FastRecovery(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr) {
        return m_cwnd;
    }

    // In recovery, maintain or slightly inflate window
    uint32_t newCwnd = m_cwnd + (segmentsAcked * socket->mss_bytes_);
    
    return std::min(newCwnd, m_maxCwnd);
}

// CUBIC-specific window update algorithm
void Cubic::CubicUpdate(std::unique_ptr<SocketState>& socket) {
    if (socket == nullptr) {
        return;
    }

    m_ackCount++;
    
    // Calculate elapsed time since epoch start
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_epochStart);
    double t = elapsed.count() / 1000.0;  // Convert to seconds
    
    // Calculate target cwnd using CUBIC function
    uint32_t cubicTarget = CubicWindowCalculation(t);
    
    // TCP-friendly region calculation
    if (m_tcpFriendly) {
        // Estimate TCP Reno cwnd
        // W_tcp(t) = W_max * (1 - β) + 3β/(2 - β) * t/RTT
        if (socket->rtt_us_ > 0) {
            double rtt_sec = socket->rtt_us_ / 1000000.0;
            uint32_t mss = socket->mss_bytes_;
            
            if (m_tcpCwnd == 0) {
                m_tcpCwnd = m_cwnd;
            }
            
            // Simplified TCP Reno estimate: increase by 1 MSS per RTT
            double tcp_increment = (3.0 * m_cubicBeta / (2.0 - m_cubicBeta)) * (t / rtt_sec) * mss;
            m_tcpCwnd = static_cast<uint32_t>(m_lastMaxCwnd * (1.0 - m_cubicBeta) + tcp_increment);
            
            // Use the larger of CUBIC or TCP estimate
            if (m_tcpCwnd > cubicTarget) {
                cubicTarget = m_tcpCwnd;
            }
        }
    }
    
    // Calculate the increment
    if (cubicTarget > m_cwnd) {
        // Calculate how many segments to add
        uint32_t delta = cubicTarget - m_cwnd;
        uint32_t mss = socket->mss_bytes_;
        uint32_t cnt = m_cwnd / delta;
        
        if (cnt == 0) {
            cnt = 1;
        }
        
        // Increase cwnd
        if (m_ackCount >= cnt) {
            m_cwnd += mss;
            m_ackCount = 0;
        }
    } else {
        // We're above the target, slow increase
        if (m_ackCount >= m_cwnd / socket->mss_bytes_) {
            m_cwnd += socket->mss_bytes_;
            m_ackCount = 0;
        }
    }
    
    m_lastTime = t;
}

// Calculate CUBIC window based on time
uint32_t Cubic::CubicWindowCalculation(double t) {
    // CUBIC function: W(t) = C * (t - K)^3 + W_max
    double delta_t = t - m_k;
    double cubic_term = m_cubicC * delta_t * delta_t * delta_t;
    
    // Convert to segments (assuming MSS = 1460 bytes for calculation)
    double target = m_lastMaxCwnd + cubic_term * 1460.0;
    
    // Ensure non-negative
    if (target < 0) {
        target = 0;
    }
    
    return static_cast<uint32_t>(target);
}

// Calculate the time K (inflection point)
void Cubic::CalculateK() {
    // K = ∛(W_max * β / C)
    // For W_max in bytes, convert to segments first
    if (m_lastMaxCwnd == 0 || m_cubicC == 0) {
        m_k = 0.0;
        return;
    }
    
    double w_max_segments = m_lastMaxCwnd / 1460.0;  // Assuming MSS = 1460
    double numerator = w_max_segments * (1.0 - m_cubicBeta);
    double k_cubed = numerator / m_cubicC;
    
    if (k_cubed < 0) {
        m_k = 0.0;
    } else {
        m_k = std::cbrt(k_cubed);  // Cube root
    }
}

// Reset CUBIC state
void Cubic::CubicReset() {
    m_lastMaxCwnd = 0;
    m_lastCwnd = 0;
    m_k = 0.0;
    m_ackCount = 0;
    m_cntRtt = 0;
    m_lastTime = 0.0;
    m_tcpCwnd = 0;
    m_delayMin = 0xFFFFFFFF;
    m_hystartDelayMin = 0xFFFFFFFF;
    m_hystartDelayMax = 0;
    m_epochStart = std::chrono::steady_clock::now();
}
```



### 核心思想：三次函数（Cubic Function）作为窗口增长模型

CUBIC最核心的创新在于，它不再像传统TCP那样依赖于每个RTT来增加窗口，而是**使用一个三次函数（即Cubic Function）来描述拥塞窗口随时间的变化**。这个函数的中心是上一次发生丢包时的窗口大小，记为 $W_{max}$。

- **$W_{max}$ (代码中的 m_lastMaxCwnd)**: 这是上一次发生拥塞事件（如丢包）之前的最大拥塞窗口值。CUBIC将这个值视为网络容量的一个稳定点。当发生丢包时，算法会记录下当前的 `cwnd` 作为新的 `m_lastMaxCwnd`，如代码中 `GetSsThresh` 函数所示：

  ```cpp
  // in GetSsThresh()
  m_lastMaxCwnd = socket->cwnd_; 
  ```

  这个 $W_{max}$ 成为了新一轮增长周期的目标和参考点。

- **$t$ (代码中的 t)**: 表示自上一次拥塞事件发生后所经过的时间（单位：秒）。代码在 `CubicUpdate` 函数中通过 `std::chrono` 来精确计算：

  ```cpp
  // in CubicUpdate()
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_epochStart);
  double t = elapsed.count() / 1000.0; // Convert to seconds
  ```

  这体现了CUBIC的**RTT无关性**：窗口的增长是基于真实时间的，而不是网络来回的次数（RTTs）。这使得在长延迟网络中，CUBIC也能快速增长窗口。

- K (代码中的 m_k): 这是三次函数曲线的拐点，代表了从窗口降低到恢复至 Wmax 所需的时间。它的计算公式为：

  在代码的 CalculateK 函数中实现：

  ```cpp
  // in CalculateK()
  double w_max_segments = m_lastMaxCwnd / 1460.0; // Assuming MSS = 1460
  double numerator = w_max_segments * (1.0 - m_cubicBeta);
  double k_cubed = numerator / m_cubicC;
  m_k = std::cbrt(k_cubed); // Cube root
  ```

  其中 $\beta$ 是窗口缩减因子（代码中为 `m_cubicBeta`，通常为0.7）。$K$ 的存在使得窗口在接近 $W_{max}$ 时增长非常平缓，增强了网络的稳定性。

- (代码中的 m_cubicC): 是一个常数，用于调整三次函数曲线的陡峭程度，默认值通常为0.4。



#### **窗口增长曲线的三个阶段**

CUBIC的窗口增长曲线因为这个三次函数呈现出独特的形状，可以分为三个阶段：

1. **TCP友好区域 (Concave Region)**: 当窗口大小低于 $W_{max}$ 时，三次函数的形状是凹形的。初期增长较快，但随着窗口接近 $W_{max}$，增速会显著放缓。为了保证对标准TCP的公平性，CUBIC在此阶段会计算一个理论上的TCP Reno窗口大小，并确保自己的增长速度不慢于它。这在代码 `CubicUpdate` 中体现：

   ```cpp
   // in CubicUpdate()
   if (m_tcpFriendly) {
       // ... 计算 m_tcpCwnd ...
       if (m_tcpCwnd > cubicTarget) {
           cubicTarget = m_tcpCwnd;
       }
   }
   ```

2. **稳定区域 (Plateau)**: 当时间 $t$ 接近 $K$ 时，窗口大小在 $W_{max}$ 附近，此时三次函数曲线非常平坦，窗口增长几乎停滞。这使得网络流量在已知的网络容量点附近保持稳定，避免了因过快增长而立即引发新的拥塞。

3. **网络探测区域 (Convex Region)**: 当窗口大小超过 $W_{max}$ 后，三次函数的形状变为凸形，窗口增长开始缓慢，然后越来越快。这是一种**主动的网络带宽探测**行为。CUBIC小心翼翼地增加窗口，然后加速，试图找到新的网络瓶颈上限。如果探测成功（没有丢包），窗口会继续快速增长；如果探测失败（发生丢包），则进入下一轮循环，将当前的 `cwnd` 记录为新的 $W_{max}$。

------



### 算法流程与关键机制分析

#### **1. 慢启动 (Slow Start)**

和标准TCP一样，CUBIC开始时也处于慢启动阶段，以指数方式快速增加窗口大小 (`m_cwnd`)，直到达到慢启动阈值 (`m_ssthresh`)。

```cpp
// in SlowStart()
uint32_t newCwnd = m_cwnd + (segmentsAcked * socket->mss_bytes_);
```

代码中还实现了一个名为 **Hystart (Hybrid Slow Start)** 的优化。它在慢启动期间监测RTT的变化。如果发现RTT突然显著增加（`m_hystartDelayMax - m_hystartDelayMin > m_hystartAckDelta`），则认为网络开始出现排队，于是提前退出慢启动，进入拥塞避免阶段。这可以有效防止慢启动过冲（Overshooting）导致的大量丢包。



#### **2. 拥塞避免 (Congestion Avoidance)**

一旦 `cwnd` 超过 `ssthresh`，就进入拥塞避免阶段，此时窗口增长由上述的**三次函数全权接管**。`CongestionAvoidance` 函数调用 `CubicUpdate`，`CubicUpdate` 再调用 `CubicWindowCalculation` 来计算当前时间点 `t` 对应的目标窗口大小 `cubicTarget`，然后平滑地将当前窗口 `m_cwnd` 向目标窗口调整。



#### **3. 拥塞事件处理**

当检测到丢包（例如，收到3个重复ACK或超时）时，CUBIC会执行以下操作，这在 `CwndEvent` 和 `GetSsThresh` 函数中有清晰的体现：

1. **记录 $W_{max}$**: 将当前 `cwnd` 记录为 `m_lastMaxCwnd`。这是下一轮CUBIC增长曲线的峰值点。

2. **窗口乘法减小 (Multiplicative Decrease)**: 将 `ssthresh` 设置为当前 `cwnd` 乘以一个缩减因子 $\beta$ (`m_cubicBeta`，通常为0.7）。

   ```cpp
   // in GetSsThresh()
   m_ssthresh = static_cast<uint32_t>(socket->cwnd_ * m_cubicBeta);
   ```

3. **设置新窗口**: 将 `cwnd` 也降低到新的 `ssthresh` 值。

4. **重置周期**: 重置纪元开始时间 `m_epochStart`，开始新一轮的CUBIC函数增长。

此外，代码中还有一个 **快速收敛 (Fast Convergence)** 机制。如果在 `cwnd` 还没增长到上一个 `m_lastMaxCwnd` 时就又发生了丢包，说明网络可用带宽可能已经下降。此时，算法会进一步下调 `m_lastMaxCwnd` 的值，使其更快地收敛到新的网络平衡点。

```cpp
// in GetSsThresh()
if (m_fastConvergence && socket->cwnd_ < m_lastMaxCwnd) {
    m_lastMaxCwnd = static_cast<uint32_t>(socket->cwnd_ * (2.0 - m_cubicBeta) / 2.0);
}
```

------



### 总结

总而言之，CUBIC算法的核心思路可以概括为以下几点：

- **RTT无关性**: 窗口增长依赖于真实时间而非RTT，使其在长延迟网络下表现出色且更加公平。
- **基于三次函数的增长模型**: 使用一个数学函数来精确控制窗口增长，使其在稳定点附近温和，在探测带宽时积极。
- **将 $W_{max}$ 作为记忆**: 算法“记住”了上一次拥塞发生时的网络容量，并以此为中心进行稳定和探测，而不是像Reno那样盲目地线性增长。
- **稳定与探测的平衡**: 三次函数的独特形状，在 $W_{max}$ 附近增长平缓以保证稳定性，在远离 $W_{max}$ 时快速增长以有效探测可用带宽。

这份C++代码非常完整地实现了CUBIC的核心逻辑，包括慢启动、Hystart、拥塞避免阶段的三次函数计算、TCP友好性、快速收敛以及对不同拥塞事件的响应，是一个很好的学习和分析范本。