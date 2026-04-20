# MessageParser TCP流解析器设计

## 1. 概述

`MessageParser` 是 ProtocolDesign 协议栈的核心组件，负责解决 TCP 流式传输中的**粘包**与**半包**问题。它将原始的字节流转换为结构化的 `Packet` 消息，确保协议消息的原子性解析。

### 1.1 设计目标

- **状态机驱动**：基于状态的解析逻辑，清晰处理各种接收场景
- **零拷贝优化**：通过内部缓冲区管理，避免不必要的数据拷贝
- **错误隔离**：协议违规立即终止解析，防止错误传播
- **资源高效**：自动压缩缓冲区，减少内存占用
- **线程安全**：单连接单解析器，无需外部同步

### 1.2 核心职责

1. **累积数据**：接收可能不完整的 TCP 数据块
2. **提取消息**：从累积数据中提取完整的协议消息包
3. **校验协议**：验证 Magic Number 和消息长度
4. **状态管理**：跟踪解析进度，处理异常情况

## 2. 状态机设计

### 2.1 状态定义

```cpp
enum class State {
    // 等待累积13字节协议头
    // 初始状态，每个消息解析成功后返回此状态
    WAIT_HEADER,

    // 协议头已解析，等待 body_length 字节的载荷
    // body_length == 0 的消息（如HEARTBEAT）立即通过
    WAIT_BODY,

    // 不可恢复的协议错误
    // 触发后所有后续操作立即返回ERROR
    POISONED,
};
```

### 2.2 状态转移图

```
                ┌──────────────┐
                │   WAIT_HEADER │◄── 消息解析成功
                └───────┬──────┘
                        │ 累积满13字节
                        ▼
                ┌──────────────┐
                │   WAIT_BODY   │◄── 解析协议头
                └───────┬──────┘
                        │ 累积满body_length字节
                        ▼
                ┌──────────────┐
                │  返回Packet   │──► WAIT_HEADER
                └──────────────┘
                        │
                        ▼ (协议违规)
                ┌──────────────┐
                │   POISONED    │
                └──────────────┘
```

### 2.3 状态转移条件

| 当前状态 | 触发条件 | 下一状态 | 说明 |
|---------|---------|---------|------|
| WAIT_HEADER | readable() < 13 | WAIT_HEADER | 数据不足，继续等待 |
| WAIT_HEADER | readable() ≥ 13 | WAIT_BODY | 开始解析协议头 |
| WAIT_BODY | readable() < body_length | WAIT_BODY | 载荷数据不足 |
| WAIT_BODY | readable() ≥ body_length | WAIT_HEADER | 提取完整消息 |
| 任意状态 | magic != 0xBEEF | POISONED | Magic校验失败 |
| 任意状态 | body_length > 64MB | POISONED | 载荷长度超限 |
| POISONED | - | POISONED | 保持中毒状态 |

## 3. API详解

### 3.1 解析结果枚举

```cpp
enum class ParseResult {
    OK,          // 成功提取一个完整Packet
    INCOMPLETE,  // 数据不足，需要更多输入
    ERROR,       // 协议违规，解析器已中毒
};
```

### 3.2 公共接口

#### `void feed(const uint8_t* data, size_t len)`

**功能**：向解析器输入原始TCP数据。

**参数**：
- `data`：指向接收数据的指针
- `len`：数据长度（字节）

**行为**：
1. 如果解析器处于`POISONED`状态，直接返回（无操作）
2. 当读取位置超过缓冲区一半时，自动压缩缓冲区
3. 将数据追加到内部缓冲区末尾

**使用场景**：
```cpp
// 从socket接收数据
uint8_t recv_buf[4096];
ssize_t n = recv(sock, recv_buf, sizeof(recv_buf), 0);
if (n > 0) {
    parser.feed(recv_buf, static_cast<size_t>(n));
}
```

#### `ParseResult next_packet(Packet& out)`

**功能**：尝试从缓冲区提取一个完整的协议包。

**参数**：
- `out`：成功时填充提取的Packet

**返回值**：
- `OK`：成功提取，`out`包含有效Packet
- `INCOMPLETE`：数据不足，需要继续接收
- `ERROR`：协议违规，解析器已中毒

**算法流程**：
```
if state == POISONED:
    return ERROR

if state == WAIT_HEADER:
    if readable() < 13:
        return INCOMPLETE
    解析协议头到pending_header_
    if magic校验失败 OR body_length > 64MB:
        state = POISONED
        return ERROR
    state = WAIT_BODY
    // 继续处理（body_length==0的消息直接通过）

if state == WAIT_BODY:
    if readable() < pending_header_.body_length:
        return INCOMPLETE
    out.header = pending_header_
    out.body = 复制body_length字节数据
    read_pos_ += body_length
    state = WAIT_HEADER
    return OK
```

#### `void reset()`

**功能**：重置解析器到初始状态。

**行为**：
1. 清空内部缓冲区
2. 重置读取位置为0
3. 状态设为`WAIT_HEADER`
4. 清空待处理协议头

**使用场景**：
- TCP连接关闭后
- 发生协议错误并希望重新开始
- 切换连接时重用解析器对象

### 3.3 私有方法

#### `size_t readable() const`

返回缓冲区中可读取的字节数：`buf_.size() - read_pos_`

#### `void compact()`

压缩缓冲区，移除已读取的数据。

**触发条件**：`read_pos_ > buf_.size() / 2 && read_pos_ > 0`

**实现**：将未读取数据移动到缓冲区开头，重置读取位置。

## 4. 内部数据结构

### 4.1 缓冲区管理

```
缓冲区布局：
+---------------------------------------------------+
| 已读取数据 | 待处理数据 (可读取) | 空闲空间          |
+---------------------------------------------------+
^           ^                     ^                 ^
0       read_pos_          buf_.size()        buf_.capacity()
```

**优化策略**：
1. **延迟压缩**：只有当读取位置超过缓冲区一半时才触发压缩
2. **向量增长**：依赖`std::vector`的指数增长策略
3. **零拷贝提取**：使用指针直接访问数据，避免中间拷贝

### 4.2 状态变量

```cpp
std::vector<uint8_t> buf_;     // 数据缓冲区
size_t               read_pos_; // 下一个读取位置
State                state_;    // 当前解析状态
Header               pending_header_; // 待处理的协议头
```

## 5. 错误处理

### 5.1 协议违规检测

**Magic校验失败**：
```cpp
if (pending_header_.magic != PROTOCOL_MAGIC) { // 0xBEEF
    state_ = State::POISONED;
    return ParseResult::ERROR;
}
```

**载荷长度超限**：
```cpp
if (pending_header_.body_length > MAX_BODY_LENGTH) { // 64 MB
    state_ = State::POISONED;
    return ParseResult::ERROR;
}
```

### 5.2 中毒状态

一旦进入`POISONED`状态：
1. 所有`feed()`调用被忽略
2. 所有`next_packet()`调用返回`ERROR`
3. 必须调用`reset()`或销毁对象才能恢复

### 5.3 使用建议

```cpp
MessageParser parser;

// 接收循环
while (connection_active) {
    // 接收数据
    uint8_t buf[8192];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n <= 0) break;
    
    parser.feed(buf, static_cast<size_t>(n));
    
    // 提取所有完整消息
    Packet pkt;
    while (true) {
        ParseResult res = parser.next_packet(pkt);
        if (res == ParseResult::INCOMPLETE) {
            break; // 等待更多数据
        }
        if (res == ParseResult::ERROR) {
            // 协议错误，关闭连接
            close(sock);
            parser.reset();
            break;
        }
        if (res == ParseResult::OK) {
            // 处理完整消息
            handle_packet(pkt);
        }
    }
}
```

## 6. 性能特性

### 6.1 时间复杂度

| 操作 | 平均复杂度 | 最坏复杂度 | 说明 |
|------|-----------|-----------|------|
| `feed()` | O(1) | O(n) | 追加数据，可能触发缓冲区压缩 |
| `next_packet()` | O(1) | O(1) | 状态检查和数据提取 |
| `reset()` | O(1) | O(1) | 重置所有状态 |

### 6.2 空间复杂度

- **基础开销**：`std::vector`控制块 + 3个指针/整数 ≈ 32字节
- **缓冲区大小**：动态调整，通常保持1-2个最大消息的大小
- **峰值内存**：`MAX_BODY_LENGTH + HEADER_SIZE` ≈ 64 MB + 13字节

### 6.3 优化策略

1. **批量处理**：单次`feed()`调用处理多个接收操作累积的数据
2. **缓冲区复用**：连接复用时可保持解析器对象，避免重复分配
3. **零拷贝**：`next_packet()`直接引用内部缓冲区数据

## 7. 线程安全与并发

### 7.1 线程安全保证

- **非线程安全**：`MessageParser`实例不应在多个线程间共享
- **典型用法**：每个TCP连接独占一个解析器实例
- **外部同步**：如需要共享，需调用方提供互斥锁

### 7.2 并发模式

```
每个连接独立解析器：
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│  线程/连接1  │    │  线程/连接2  │    │  线程/连接3  │
│  Parser A   │    │  Parser B   │    │  Parser C   │
└─────────────┘    └─────────────┘    └─────────────┘
```

## 8. 测试要点

### 8.1 单元测试覆盖

1. **正常流程**：
   - 完整消息一次到达
   - 消息分多次到达（粘包/半包）
   - 多个消息连续到达

2. **边界条件**：
   - 空载荷消息（HEARTBEAT）
   - 最大长度消息（64 MB）
   - 零长度接收（`feed(nullptr, 0)`）

3. **错误处理**：
   - Magic错误触发中毒
   - 超长载荷触发中毒
   - 中毒状态下的操作
   - 重置恢复功能

### 8.2 集成测试场景

```cpp
// 模拟TCP粘包场景
TEST(MessageParser, FragmentedPackets) {
    MessageParser parser;
    Packet pkt;
    
    // 发送第一个消息的前半部分
    parser.feed(header_data, 7); // 仅13字节头的前7字节
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::INCOMPLETE);
    
    // 发送第一个消息的剩余部分 + 第二个消息的一部分
    parser.feed(remaining_data, 100);
    // 应能提取第一个完整消息
    EXPECT_EQ(parser.next_packet(pkt), ParseResult::OK);
}
```

## 9. 扩展与定制

### 9.1 配置参数

可通过模板参数或构造函数配置：
```cpp
template <size_t MaxBodySize = 64 * 1024 * 1024>
class ConfigurableMessageParser {
    // 可配置的最大载荷长度
};
```

### 9.2 性能监控

可添加统计信息：
```cpp
struct ParserStats {
    size_t packets_parsed;
    size_t bytes_processed;
    size_t errors_detected;
    size_t buffer_compactions;
};
```

### 9.3 异步支持

未来可扩展为异步接口：
```cpp
class AsyncMessageParser {
public:
    std::future<ParseResult> async_next_packet();
    // 基于回调或协程的接口
};
```

## 10. 常见问题解答

### Q1: 为什么需要状态机而不是简单的长度前缀解析？
**A**: 状态机明确分离了协议头解析和载荷解析阶段，能更好处理部分数据到达的情况，代码更清晰，错误处理更精确。

### Q2: 中毒状态是否过于严格？能否自动恢复？
**A**: 协议违规通常意味着通信双方不一致，自动恢复可能导致更难调试的问题。显式重置要求调用方意识到错误并采取行动。

### Q3: 缓冲区压缩策略是否可能影响性能？
**A**: 延迟压缩策略（超过一半才压缩）在大多数场景下是合理的。对于高频小消息场景，可考虑更激进的压缩阈值。

### Q4: 是否支持超大消息（>64MB）？
**A**: 当前设计限制为64MB，出于安全考虑。如需支持更大消息，可修改`MAX_BODY_LENGTH`并确保有足够内存。

### Q5: 如何集成到现有网络框架中？
**A**: `MessageParser`是协议无关的，只需提供原始字节流。可轻松集成到libuv、asio、libevent等框架中。

---

*文档版本：1.0*  
*最后更新：2026-04-20*  
*基于 ProtocolDesign MessageParser 实现*