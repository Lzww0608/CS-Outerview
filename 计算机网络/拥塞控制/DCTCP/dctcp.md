```cpp

#include "dctcp.h"
#include <algorithm>
#include <cstdint>
#include <cmath>

// Default constructor
DCTCP::DCTCP() 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::DCTCP), "DCTCP"),
      m_ssthresh(0x7fffffff),      // Initially very large
      m_cwnd(0),                    // Will be set based on MSS
      m_maxCwnd(65535),             // Default max window
      m_alpha(1.0),                 // Start with max alpha (conservative)
      m_g(DEFAULT_G),               // EWMA weight = 1/16
      m_ackedBytesEcn(0),           // No ECN bytes yet
      m_ackedBytesTotal(0),         // No total bytes yet
      m_ceState(false),             // No congestion experienced
      m_delayedAckReserved(false),  // No delayed ACK
      m_priorRcvNxt(0),             // No prior sequence
      m_priorRcvNxtFlag(0),         // No flag
      m_nextSeq(0),                 // Start sequence
      m_initialized(false),         // Not initialized
      m_ecnEchoSeq(0)               // No ECN echo
{
    InitializeDCTCP();
}

// Copy constructor
DCTCP::DCTCP(const DCTCP& other) 
    : CongestionControl(static_cast<TypeId>(CongestionAlgorithm::DCTCP), "DCTCP"),
      m_ssthresh(other.m_ssthresh),
      m_cwnd(other.m_cwnd),
      m_maxCwnd(other.m_maxCwnd),
      m_alpha(other.m_alpha),
      m_g(other.m_g),
      m_ackedBytesEcn(other.m_ackedBytesEcn),
      m_ackedBytesTotal(other.m_ackedBytesTotal),
      m_ceState(other.m_ceState),
      m_delayedAckReserved(other.m_delayedAckReserved),
      m_priorRcvNxt(other.m_priorRcvNxt),
      m_priorRcvNxtFlag(other.m_priorRcvNxtFlag),
      m_nextSeq(other.m_nextSeq),
      m_initialized(other.m_initialized),
      m_ecnEchoSeq(other.m_ecnEchoSeq)
{
}

// Destructor
DCTCP::~DCTCP() {
    // No dynamic memory to clean up
}

// Get type ID
TypeId DCTCP::GetTypeId() {
    return static_cast<TypeId>(CongestionAlgorithm::DCTCP);
}

// Get algorithm name
std::string DCTCP::GetAlgorithmName() {
    return "DCTCP";
}

// Get slow start threshold
uint32_t DCTCP::GetSsThresh(std::unique_ptr<SocketState>& socket, uint32_t bytesInFlight) {
    if (socket == nullptr) {
        return m_ssthresh;
    }

    // DCTCP: ssthresh = cwnd * (1 - α/2)
    // This is more gentle than standard TCP's cwnd/2
    m_ssthresh = static_cast<uint32_t>(socket->cwnd_ * (1.0 - m_alpha / 2.0));
    
    // Ensure minimum of 2 MSS
    m_ssthresh = std::max(m_ssthresh, 2 * socket->mss_bytes_);
    
    socket->ssthresh_ = m_ssthresh;
    return m_ssthresh;
}

// Increase congestion window based on current state
void DCTCP::IncreaseWindow(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
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
    } else if (InSlowStart()) {
        // Slow start phase - use standard TCP behavior
        m_cwnd = SlowStart(socket, segmentsAcked);
    } else {
        // Congestion avoidance phase - use DCTCP behavior
        m_cwnd = CongestionAvoidance(socket, segmentsAcked);
    }

    // Ensure we don't exceed maximum window
    m_cwnd = std::min(m_cwnd, m_maxCwnd);
    socket->cwnd_ = m_cwnd;
}

// Handle ACKed packets with ECN feedback
void DCTCP::PktsAcked(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked, const uint64_t rtt) {
    if (socket == nullptr) {
        return;
    }

    // Update RTT information
    socket->rtt_us_ = static_cast<uint32_t>(rtt);
    
    // Basic RTT variance calculation
    if (socket->rtt_var_ == 0) {
        socket->rtt_var_ = rtt / 2;
    } else {
        socket->rtt_var_ = (3 * socket->rtt_var_ + rtt) / 4;
    }

    // Update RTO
    socket->rto_us_ = socket->rtt_us_ + 4 * socket->rtt_var_;

    // Update ACK count for this window
    uint32_t ackedBytes = segmentsAcked * socket->mss_bytes_;
    m_ackedBytesTotal += ackedBytes;
    
    // Process ECN marking (simulated - in real implementation would come from packet)
    // Here we assume ECN info is available through some mechanism
    bool ecnMarked = false;  // Would be extracted from actual packet in real implementation
    
    if (ecnMarked) {
        m_ackedBytesEcn += ackedBytes;
    }
    
    // Check if we've completed a window and should update alpha
    // This happens approximately once per RTT
    if (m_ackedBytesTotal >= m_cwnd) {
        UpdateAlpha();
        ResetECNCounters();
    }
}

// Set congestion state
void DCTCP::CongestionStateSet(std::unique_ptr<SocketState>& socket, const TCPState congestionState) {
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
void DCTCP::CwndEvent(std::unique_ptr<SocketState>& socket, const CongestionEvent congestionEvent) {
    if (socket == nullptr) {
        return;
    }

    socket->congestion_event_ = congestionEvent;

    switch (congestionEvent) {
        case CongestionEvent::PacketLoss:
            // Packet loss: use DCTCP reduction
            GetSsThresh(socket, 0);
            m_cwnd = m_ssthresh;
            socket->cwnd_ = m_cwnd;
            socket->tcp_state_ = TCPState::Recovery;
            break;

        case CongestionEvent::Timeout:
            // Timeout: reset cwnd to initial window
            m_ssthresh = std::max(socket->cwnd_ / 2, 2 * socket->mss_bytes_);
            socket->ssthresh_ = m_ssthresh;
            m_cwnd = socket->mss_bytes_;
            socket->cwnd_ = m_cwnd;
            socket->tcp_state_ = TCPState::Loss;
            
            // Reset alpha to conservative value
            m_alpha = 1.0;
            ResetECNCounters();
            break;

        case CongestionEvent::ECN:
            // ECN: DCTCP's primary congestion signal
            ProcessECN(true);
            
            // Only reduce cwnd if not in slow start
            if (!InSlowStart()) {
                GetSsThresh(socket, 0);
                m_cwnd = m_ssthresh;
                socket->cwnd_ = m_cwnd;
            }
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
bool DCTCP::HasCongControl() const {
    return true;
}

// Main congestion control logic
void DCTCP::CongControl(std::unique_ptr<SocketState>& socket, 
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

// Slow start: exponential growth (standard TCP)
uint32_t DCTCP::SlowStart(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
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

// Congestion avoidance: additive increase (standard TCP)
uint32_t DCTCP::CongestionAvoidance(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr || segmentsAcked == 0) {
        return m_cwnd;
    }

    // Increase cwnd by approximately MSS per RTT
    // Formula: cwnd += MSS * MSS / cwnd (for each ACK)
    uint32_t mss = socket->mss_bytes_;
    uint32_t increment = (segmentsAcked * mss * mss) / m_cwnd;
    
    if (increment == 0 && segmentsAcked > 0) {
        increment = 1;  // Ensure at least some progress
    }

    uint32_t newCwnd = m_cwnd + increment;
    return std::min(newCwnd, m_maxCwnd);
}

// Fast recovery: maintain cwnd
uint32_t DCTCP::FastRecovery(std::unique_ptr<SocketState>& socket, uint32_t segmentsAcked) {
    if (socket == nullptr) {
        return m_cwnd;
    }

    // Inflate window for each additional duplicate ACK
    uint32_t newCwnd = m_cwnd + (segmentsAcked * socket->mss_bytes_);
    
    return std::min(newCwnd, m_maxCwnd);
}

// Update alpha (ECN fraction estimate)
void DCTCP::UpdateAlpha() {
    if (m_ackedBytesTotal == 0) {
        return;
    }
    
    // Calculate the fraction of bytes that were ECN-marked in this window
    double F = static_cast<double>(m_ackedBytesEcn) / static_cast<double>(m_ackedBytesTotal);
    
    // Update alpha using EWMA (Exponentially Weighted Moving Average)
    // α = (1 - g) * α + g * F
    m_alpha = (1.0 - m_g) * m_alpha + m_g * F;
    
    // Clamp alpha to valid range [0, 1]
    m_alpha = std::max(0.0, std::min(DCTCP_MAX_ALPHA, m_alpha));
}

// Process ECN feedback
void DCTCP::ProcessECN(bool ecnMarked) {
    if (!m_initialized) {
        InitializeDCTCP();
    }
    
    // Update CE state
    m_ceState = ecnMarked;
    
    // If this ACK has ECN marking, we'll account for it in the next update
}

// Calculate new cwnd based on alpha
uint32_t DCTCP::CalculateNewCwnd() {
    // DCTCP reduction: cwnd_new = cwnd * (1 - α/2)
    // This is more gentle than TCP's cwnd/2 when α is small
    double reduction_factor = 1.0 - m_alpha / 2.0;
    uint32_t newCwnd = static_cast<uint32_t>(m_cwnd * reduction_factor);
    
    // Ensure minimum window size
    newCwnd = std::max(newCwnd, 2 * 1460);  // At least 2 MSS
    
    return newCwnd;
}

// Reset ECN counters for new window
void DCTCP::ResetECNCounters() {
    m_ackedBytesEcn = 0;
    m_ackedBytesTotal = 0;
    m_priorRcvNxt = m_nextSeq;
}

// Initialize DCTCP parameters
void DCTCP::InitializeDCTCP() {
    m_alpha = 1.0;              // Start conservative
    m_ackedBytesEcn = 0;
    m_ackedBytesTotal = 0;
    m_ceState = false;
    m_initialized = true;
    m_priorRcvNxt = 0;
    m_nextSeq = 0;
}

// Check if in slow start
bool DCTCP::InSlowStart() const {
    return m_cwnd < m_ssthresh;
}
```



好的，我们来用中文详细解析一下这份 C++ 代码所实现的 DCTCP 算法流程及其核心思想。

DCTCP (Data Center TCP) 的设计初衷是为了解决数据中心网络中的一个关键问题：传统 TCP 在处理拥塞时反应过于“激烈”，导致网络吞吐不稳定和延迟增加。

DCTCP 的**核心思想**是：利用 **ECN (显式拥塞通知)** 机制，不仅仅是检测拥塞的**有无**，而是进一步去**衡量拥塞的程度**。然后，根据拥塞的严重程度，**按比例地**减小拥塞窗口（`cwnd`），而不是像标准 TCP 那样粗暴地将窗口减半。

这带来了更平滑的流量、更低的网络排队延迟和更高的数据中心网络吞吐量。

------



### 代码中的关键状态变量

要理解算法流程，首先需要了解几个用于追踪拥塞状态的关键变量：

- `m_alpha`: 这是 DCTCP 中**最重要**的变量。它是一个经过平滑处理的、对“**网络中发生拥塞的数据包比例**”的估计值。`m_alpha` 为 `0.0` 意味着没有拥塞，为 `1.0` 则意味着所有数据包都被标记了拥塞。
- `m_g`: 这是用于平滑计算 `m_alpha` 的**权重因子**（在 EWMA 算法中）。它的值通常很小，比如 `1/16` (`0.0625`)，这确保了 `m_alpha` 的变化是平滑的，不会因为短暂的网络抖动而剧烈变化。
- `m_ackedBytesTotal`: 计数器，记录在当前观测窗口（大约一个 RTT）内，总共收到了多少字节的 ACK 确认。
- `m_ackedBytesEcn`: 计数器，记录在上述总确认字节中，有多少是被标记了 ECN 拥塞的。

------



### DCTCP 算法流程（代码视角）

该算法在一个事件驱动的循环中运行。下面是它处理主要事件的方式：

#### 1. “理想路径”：窗口增长（无拥塞时）

当网络畅通时，DCTCP 的行为与标准 TCP 非常相似。这部分逻辑由 `IncreaseWindow()` 函数处理。

- 慢启动（Slow Start） (InSlowStart() 为 true):

  如果 m_cwnd 小于慢启动阈值 m_ssthresh，算法处于慢启动阶段。此时调用 SlowStart() 函数，cwnd 会呈指数级增长 (m_cwnd + (segmentsAcked * mss_bytes_))。这使得连接能迅速地利用可用带宽。

- 拥塞避免（Congestion Avoidance） (InSlowStart() 为 false):

  一旦 cwnd 超过 m_ssthresh，算法进入拥塞避免阶段。CongestionAvoidance() 函数会使 cwnd 线性、缓慢地增加（大约每个 RTT 增加一个 MSS），以温和地探测更多可用带宽。



#### 2. 接收 ACK 并收集 ECN 数据

这是 DCTCP 与标准 TCP 开始分道扬镳的地方。`PktsAcked()` 函数负责收集衡量拥塞程度所需的原始数据。

1. 一个 ACK 到达，确认了 `segmentsAcked` 个数据段。
2. `m_ackedBytesTotal` 增加相应确认的字节数。
3. 代码检查这些被确认的数据包是否被路由器标记了 ECN。（**注意**: 代码中的 `bool ecnMarked = false;` 是一个占位符。在真实的实现中，这个信息会从 ACK 数据包的 TCP 头部中提取）。
4. 如果数据包被标记了 ECN，`m_ackedBytesEcn` 也会相应增加。



#### 3. 更新拥塞估计值 (`m_alpha`)

最关键的一步大约每个 RTT 发生一次。代码通过检查已确认的总字节数是否超过了当前的拥塞窗口来触发这个操作：`if (m_ackedBytesTotal >= m_cwnd)`。

当条件满足时，就意味着是时候更新我们对网络拥塞水平的“认知”了。

1. `UpdateAlpha()` 函数被调用。

2. 在 `UpdateAlpha()` 内部，首先计算当前窗口内被 ECN 标记的字节比例 `F`：`F = m_ackedBytesEcn / m_ackedBytesTotal`。

3. 然后，应用核心的 **EWMA (指数加权移动平均)** 公式来更新 `m_alpha`：

   ```cpp
   // α_新 = (1 - g) * α_旧 + g * F
   m_alpha = (1.0 - m_g) * m_alpha + m_g * F;
   ```

   这个公式起到了“平滑”作用，让 `m_alpha` 的值能够稳定地反映近期的拥塞趋势。

4. 最后，调用 `ResetECNCounters()` 将 `m_ackedBytesEcn` 和 `m_ackedBytesTotal` 清零，为下一个 RTT 的测量做准备。



#### 4. 响应拥塞事件 🚨

`CwndEvent()` 函数是处理所有网络“坏消息”的中央枢纽。

- 情况 1：ECN 信号 (CongestionEvent::ECN) - DCTCP 的核心逻辑

  这是 DCTCP 检测拥塞的主要方式。

  1. 调用 `GetSsThresh()` 函数。**DCTCP 的魔法就在这里发生**。它使用核心的窗口减小公式来计算新的 `ssthresh`：

     ```cpp
     // ssthresh = cwnd * (1 - α / 2)
     m_ssthresh = static_cast<uint32_t>(socket->cwnd_ * (1.0 - m_alpha / 2.0));
     ```

  2. 然后，拥塞窗口 `m_cwnd` 立刻减小到这个新的、更低的阈值：`m_cwnd = m_ssthresh`。

  **这个机制为什么强大？**

  - 如果拥塞很轻微（例如 `m_alpha` 是 `0.1`），窗口的减小幅度只有约 5% (`1 - 0.1 / 2`)。
  - 如果拥塞很严重（例如 `m_alpha` 是 `0.8`），窗口的减小幅度则会很大，约 40% (`1 - 0.8 / 2`)。
  - 这种**按比例的、精确的响应**正是 DCTCP 高效的原因。

- 情况 2：丢包 (CongestionEvent::PacketLoss)

  DCTCP 同样能处理传统的丢包事件。它将丢包视为一个严重的拥塞信号，并采用与 ECN 信号相同的 GetSsThresh() 公式来减小窗口。

- 情况 3：超时 (CongestionEvent::Timeout)

  超时是最严重的信号。此时连接被认为中断，DCTCP 会像标准 TCP 一样采取非常激进的重置策略。它将 ssthresh 减半，并将 cwnd 骤降到只有一个 MSS 的大小，强制重新进入慢启动。同时，它也会将 m_alpha 重置为保守值 1.0。

