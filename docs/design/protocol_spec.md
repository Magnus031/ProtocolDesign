# ProtocolDesign 通信协议规范

## 1. 概述

ProtocolDesign 是一个基于 TCP 的应用级 UI 虚拟化通信协议系统，采用 **Client → Gateway → AppHost** 三层架构。本协议定义了三方之间所有通信消息的二进制格式、序列化规则以及状态机解析方法。

### 1.1 设计原则

- **纯 TCP 长连接**：保证指令级可靠传输，避免 UDP 的乱序与丢包问题
- **固定长度 Header**：解决 TCP 流式传输的**粘包**与**半包**问题
- **Magic Number 校验**：过滤非法数据包，确保协议一致性  
- **Session ID 路由**：支持多客户端并发接入同一端口，Gateway 按 Session 精准路由
- **大端序（网络字节序）**：所有多字节整数均使用大端序传输
- **原子性指令解析**：每个消息包包含完整的指令语义

### 1.2 通信流程

```
Client ──SPAWN_APP──► Gateway ──fork/exec──► AppHost
Client ◄─SESSION_ACK─ Gateway ◄─TCP连接───── AppHost
Client ◄─PIXEL_DATA─ Gateway ◄─DoDraw 捕获── AppHost
Client ──INPUT_EVENT─ Gateway ──路由转发───► AppHost
```

## 2. 协议头格式

所有消息均以一个 **固定长度 13 字节的 Header** 开头，后跟可变长度的载荷（Body）。

### 2.1 内存布局

```
偏移量  长度      字段         说明
0-1     2字节    Magic       协议标识，固定 0xBEEF
2-5     4字节    SessionID   会话标识（大端序）
6       1字节    CmdType     指令类型枚举
7-10    4字节    BodyLength  载荷字节长度（大端序）
11-12   2字节    Reserved    保留字段，始终为 0
```

### 2.2 C++ 结构体定义

```cpp
#pragma once
#include <cstdint>

enum class CmdType : uint8_t {
    HEARTBEAT     = 0x01,  // 心跳包，Body 为空
    SPAWN_APP     = 0x02,  // Client → Gateway：请求启动 AppHost
    SESSION_ACK   = 0x03,  // Gateway → Client：分配 Session ID
    APPHOST_READY = 0x04,  // AppHost → Gateway：绑定 AppHost 连接到 Session ID
    PIXEL_DATA    = 0x10,  // AppHost → Client：脏矩形像素数据
    INPUT_EVENT   = 0x20,  // Client → AppHost：鼠标/键盘事件
    CLOSE_SESSION = 0xFE,  // 任意方向：关闭会话
    ERROR_RESP    = 0xFF,  // 错误通知
};

struct Header {
    uint16_t magic;        // 固定 PROTOCOL_MAGIC (0xBEEF)
    uint32_t session_id;   // 会话 ID
    CmdType  cmd_type;     // 指令类型
    uint32_t body_length;  // 载荷长度
    uint16_t reserved;     // 保留字段，始终为 0
};

static constexpr uint16_t PROTOCOL_MAGIC       = 0xBEEF;
static constexpr size_t   HEADER_SIZE          = 13;
static constexpr uint32_t MAX_BODY_LENGTH      = 64u * 1024u * 1024u;  // 64 MB
```

### 2.3 字节序转换规则

所有多字节整数（`magic`、`session_id`、`body_length`、`reserved`）在序列化时**必须转换为大端序（网络字节序）**，反序列化时再转回主机字节序。

序列化示例（`session_id = 0x01020304`）：
```
buf[2] = 0x01  // 最高有效字节 (MSB)
buf[3] = 0x02
buf[4] = 0x03  
buf[5] = 0x04  // 最低有效字节 (LSB)
```

## 3. 指令类型详解

### 3.1 HEARTBEAT (0x01)

**方向**：双向  
**载荷**：无（`body_length = 0`）  
**功能**：维持 TCP 连接活性，检测对端存活状态。客户端与网关、网关与 AppHost 之间定期发送。

### 3.2 SPAWN_APP (0x02)

**方向**：Client → Gateway  
**功能**：请求启动指定的 AppHost 进程。

**载荷格式**：
```
┌──────────────────┬──────────────────────┐
│ appNameLength    │ appName              │
│ (4 bytes BE)     │ (variable, UTF-8)    │
└──────────────────┴──────────────────────┘
```

- `appNameLength`：应用名长度（字节数），大端序 32 位整数
- `appName`：逻辑应用名称，UTF-8 编码，不含终止符；不是任意 `.so/.dll` 文件路径

Gateway 必须通过本地 allowlist / 配置表把 `appName` 映射到实际插件路径，例如：

```
demo_app -> ./plugins/demo_app/libdemo_app.so
```

Client 不直接传 `plugin_path`，避免请求任意本地文件路径。

**处理流程**：
1. Gateway 解析 `appName`，检查名称格式和 allowlist 映射
2. Gateway 分配 Session ID，创建 SPAWNING 状态的会话记录
3. 通过 `fork()` + `exec()` 拉起独立的 AppHost 进程，并通过启动参数传入 Session ID
4. Gateway 等待 AppHost 建立 TCP 连接并发送 `APPHOST_READY`
5. Gateway 建立双向路由映射
6. Gateway 向 Client 返回 `SESSION_ACK`

### 3.3 SESSION_ACK (0x03)

**方向**：Gateway → Client  
**功能**：确认会话建立，返回分配的 Session ID。

**载荷格式**：Body 为空，分配的 Session ID 写在 Header 的 `session_id` 字段中。

- `header.session_id`：Gateway 分配的会话标识，与 Client 的 TCP socket 绑定

**后续通信**：Client 与 AppHost 的所有后续消息必须携带此 `sessionId`，Gateway 依此进行路由转发。

### 3.4 APPHOST_READY (0x04)

**方向**：AppHost → Gateway
**功能**：声明当前 AppHost TCP 连接属于哪个 Session。

**载荷格式**：Body 为空，`session_id` 写在 Header 中。

- `session_id`：Gateway 分配并通过 AppHost 启动参数传入的会话标识

**处理流程**：
1. AppHost 进程启动后连接 Gateway 的内部端口
2. AppHost 发送 `APPHOST_READY`，Header 中携带 `session_id`
3. Gateway 将当前 AppHost 连接绑定到对应的 `ClientSession`
4. 绑定完成后，Gateway 才能按 `session_id` 双向转发 `INPUT_EVENT` / `PIXEL_DATA`

### 3.5 PIXEL_DATA (0x10)

**方向**：AppHost → Client  
**功能**：传输脏矩形区域的像素数据。

**载荷格式**：
```
┌────────────┬────────────┬────────────┬────────────┬────────────┬────────────┬────────────┐
│ frameSeq   │ rectLeft   │ rectTop    │ rectRight  │ rectBottom │ dataLen    │ pixelData  │
│ (4 BE)     │ (4 BE)     │ (4 BE)     │ (4 BE)     │ (4 BE)     │ (4 BE)     │ (variable) │
└────────────┴────────────┴────────────┴────────────┴────────────┴────────────┴────────────┘
```

- `frameSeq`：帧序列号（大端序），用于客户端选择性丢弃过期帧
- `rectLeft`, `rectTop`, `rectRight`, `rectBottom`：脏矩形坐标（大端序 32 位整数）
- `dataLen`：像素数据长度（字节数，大端序）
- `pixelData`：`rect` 区域内的 ARGB 像素数据，行优先排列，可选压缩

**像素格式**：每个像素为 32 位 `0xAARRGGBB` 格式（Alpha, Red, Green, Blue），行优先连续存储。

**压缩支持**：`dataLen` 可能小于 `rectWidth×rectHeight×4`，表示数据已压缩。客户端需根据情况解压。

### 3.5 INPUT_EVENT (0x20)

**方向**：Client → AppHost  
**功能**：传输鼠标或键盘输入事件。

**载荷格式**（通用头部）：
```
┌──────────────┬──────────────────┐
│ eventType    │ eventData        │
│ (1 byte)     │ (variable)       │
└──────────────┴──────────────────┘
```

`eventType` 定义：
- `0x01`：鼠标移动 (MOUSE_MOVE)
- `0x02`：鼠标左键按下 (MOUSE_LEFT_DOWN)  
- `0x03`：鼠标左键释放 (MOUSE_LEFT_UP)
- `0x04`：鼠标右键按下 (MOUSE_RIGHT_DOWN)
- `0x05`：鼠标右键释放 (MOUSE_RIGHT_UP)
- `0x06`：鼠标滚轮 (MOUSE_SCROLL)
- `0x10`：键盘按下 (KEY_DOWN)
- `0x11`：键盘释放 (KEY_UP)

#### 鼠标事件载荷 (`eventType = 0x01~0x06`)

```
┌──────────────┬──────────────────┬──────────────────┬──────────────────┐
│ eventType    │ x                │ y                │ timestamp        │
│ (1 byte)     │ (4 bytes BE)     │ (4 bytes BE)     │ (8 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┴──────────────────┘
```

- `x`, `y`：鼠标坐标（相对于窗口左上角，大端序 32 位整数）
- `timestamp`：事件时间戳（微秒，大端序 64 位整数）

对于滚轮事件 (`eventType = 0x06`)，`y` 字段表示滚轮增量（正数为向上/远离用户）。

#### 键盘事件载荷 (`eventType = 0x10~0x11`)

```
┌──────────────┬──────────────────┬──────────────────┐
│ eventType    │ keyCode          │ timestamp        │
│ (1 byte)     │ (2 bytes BE)     │ (8 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┘
```

- `keyCode`：平台无关的按键码（大端序 16 位整数），遵循 GKC 框架定义
- `timestamp`：事件时间戳（微秒，大端序 64 位整数）

### 3.6 CLOSE_SESSION (0xFE)

**方向**：任意方向  
**功能**：通知对端关闭会话。

**载荷格式**：
```
┌──────────────────┐
│ reasonCode       │
│ (1 byte)         │
└──────────────────┘
```

`reasonCode` 定义：
- `0x00`：正常关闭（应用退出）
- `0x01`：客户端主动断开
- `0x02`：AppHost 进程崩溃
- `0x03`：心跳超时
- `0xFF`：未知原因

收到此消息后，双方应清理与会话相关的资源，关闭 TCP 连接。

### 3.7 ERROR_RESP (0xFF)

**方向**：任意方向  
**功能**：传输错误信息。

**载荷格式**：
```
┌──────────────────┬──────────────────────┐
│ errorCode        │ errorMessage         │
│ (2 bytes BE)     │ (variable, UTF-8)    │
└──────────────────┴──────────────────────┘
```

- `errorCode`：错误码（大端序 16 位整数）
- `errorMessage`：错误描述文本，UTF-8 编码

常见错误码：
- `0x0001`：非法协议头（Magic 不匹配）
- `0x0002`：Session 不存在
- `0x0003`：应用插件加载失败
- `0x0004`：内存分配失败
- `0x0005`：像素缓冲区溢出

## 4. 序列化与反序列化

### 4.1 协议头序列化

```cpp
// 将 Header 序列化为 13 字节大端网络字节序写入 buf
// buf 必须至少有 HEADER_SIZE 字节可写
void serialize_header(const Header& header, uint8_t* buf);

// 从 buf 的 13 字节反序列化还原 Header  
// buf 必须至少有 HEADER_SIZE 字节可读
void deserialize_header(const uint8_t* buf, Header& header);
```

实现确保：
1. 多字节整数正确转换为大端序
2. `reserved` 字段始终序列化为 0
3. 内存访问无越界

### 4.2 载荷序列化

每种指令类型的载荷需要独立的序列化/反序列化函数，遵循以下原则：
1. 整数使用大端序
2. 变长字符串前缀长度字段
3. 结构体成员紧密排列（无填充）

示例序列化伪代码：
```cpp
// 序列化 SPAWN_APP 载荷
void serialize_spawn_app(const std::string& app_name, std::vector<uint8_t>& body) {
    uint32_t len = htobe32(static_cast<uint32_t>(app_name.size()));
    body.insert(body.end(), reinterpret_cast<uint8_t*>(&len), 
                reinterpret_cast<uint8_t*>(&len) + 4);
    body.insert(body.end(), app_name.begin(), app_name.end());
}
```

## 5. TCP 粘包/半包处理

由于 TCP 是流式协议，接收端必须实现**状态机解析器**来正确处理消息边界。

### 5.1 解析状态机

```
                ┌──────────┐
                │  IDLE    │
                └────┬─────┘
                     │ 接收数据
                     ▼
           ┌────────────────────┐
           │  READ_HEADER       │ ◄── 累积读满 13 字节 Header
           │  (缓冲区累积)       │
           └────────┬───────────┘
                    │ Header 完整
                    ▼
         ┌──────────────────────┐
         │ VALIDATE_MAGIC       │ ◄── 校验 Magic == 0xBEEF
         │                      │     失败则丢弃并重新同步
         └────────┬─────────────┘
                  │ 校验通过
                  ▼
         ┌──────────────────────┐
         │  READ_BODY           │ ◄── 根据 body_length 继续累积
         │  (缓冲区累积)         │
         └────────┬─────────────┘
                  │ Body 完整
                  ▼
         ┌──────────────────────┐
         │  DISPATCH            │ ◄── 按 cmd_type 分发处理
         └──────────────────────┘
```

### 5.2 实现要点

1. **环形缓冲区**：避免数据拷贝，支持滑动窗口
2. **超时保护**：长时间未收到完整报文应断开连接
3. **长度校验**：`body_length` 超过 `MAX_BODY_LENGTH` 视为非法包
4. **快速失败**：Magic 校验失败立即丢弃并尝试重新同步

### 5.3 重新同步策略

当 Magic 校验失败时，解析器应：
1. 向后滑动 1 字节，继续尝试匹配 Magic
2. 记录连续失败次数，超过阈值则断开连接
3. 成功匹配后，验证后续字段合理性（如 `body_length` 范围）

## 6. 会话管理与路由

### 6.1 Session ID 分配规则

1. Gateway 为每个 Client 连接分配唯一 Session ID
2. Session ID 从 `0x00000001` 开始递增分配
3. Session ID `0x00000000` 保留给未建立会话的连接（如 SPAWN_APP 请求）
4. Session 超时或关闭后，ID 可回收重用

### 6.2 Gateway 路由表

Gateway 维护双向映射表：
```
Client_Socket  ↔  Session_ID  ↔  AppHost_Socket
```

路由逻辑：
1. Client → Gateway：根据 Source Socket 查找 Session ID，附加到报文头部
2. Gateway → AppHost：根据 Session ID 查找目标 AppHost Socket
3. AppHost → Gateway：根据 Source Socket 反向查找 Session ID
4. Gateway → Client：根据 Session ID 查找目标 Client Socket

### 6.3 心跳与超时

- **心跳间隔**：建议 30 秒
- **会话超时**：建议 90 秒（3 次心跳未响应）
- **实现方式**：`HEARTBEAT` 指令，空载荷

## 7. 性能与可靠性设计

### 7.1 帧序列号机制

`PIXEL_DATA` 携带 `frameSeq` 字段，客户端据此：
1. **按序重绘**：正常情况按帧序列号顺序应用脏矩形
2. **选择性丢弃**：网络拥塞时，如果收到更新的帧（`frameSeq` 更大），可安全丢弃旧帧的待处理矩形
3. **乱序处理**：允许帧乱序到达，但最终以最新帧为准

### 7.2 脏矩形来源

AppHost 的 `ui_host_impl` 拦截 GKC 的 `DoDraw` 回调，从 `pDraw->rcPaint` 直接读取本次重绘的脏矩形坐标（GKC 框架自行维护），无需逐帧像素差分。每次 `DoDraw` 触发时只传输 `rcPaint` 指定区域的像素数据。

### 7.3 压缩策略

`PIXEL_DATA` 支持多种压缩算法：
1. **无损压缩**：zlib/gzip，适用于文本/图形界面
2. **有损压缩**：JPEG，适用于自然图像内容
3. **算法协商**：可通过扩展协议协商压缩算法

当前实现使用基础压缩，`dataLen` 字段反映压缩后大小。

## 8. 扩展性设计

### 8.1 版本兼容

协议通过以下机制保证向前/向后兼容：
1. **Magic 字段**：未来协议版本可使用不同 Magic 值
2. **Reserved 字段**：保留供未来扩展
3. **可选字段**：新版本可在载荷末尾添加可选字段，旧版本忽略

### 8.2 指令类型扩展

新增 `CmdType` 值必须遵循：
1. `0x00` 保留
2. `0x01-0x0F`：会话管理类指令
3. `0x10-0x1F`：像素传输类指令  
4. `0x20-0x2F`：输入事件类指令
5. `0xF0-0xFE`：控制类指令
6. `0xFF`：错误响应

### 8.3 载荷扩展

新增指令类型或扩展现有指令时，应在载荷末尾添加新字段，并更新 `body_length` 计算。

## 9. 安全考虑

### 9.1 输入验证

1. **长度校验**：所有长度字段必须在合理范围内
2. **整数溢出**：计算缓冲区大小时预防整数溢出
3. **字符串终止**：变长字符串必须显式长度前缀，避免依赖终止符

### 9.2 资源限制

1. **最大载荷**：`MAX_BODY_LENGTH = 64 MB`，防止内存耗尽攻击
2. **连接数限制**：Gateway 应限制最大并发连接数
3. **速率限制**：防止客户端 flood 攻击

### 9.3 认证与加密（未来扩展）

当前版本未包含认证与加密，适用于可信网络环境。未来可通过：
1. TLS 隧道封装整个 TCP 连接
2. 在协议层添加认证握手阶段
3. 载荷字段级加密

## 10. 参考实现

### 10.1 关键文件

- `src/protocol/protocol.h`：协议头与指令类型定义
- `src/protocol/protocol.cpp`：序列化/反序列化实现  
- `src/protocol/message_parser.h`：TCP 粘包状态机（已实现）
- `src/common/byte_order.h`：字节序转换工具

### 10.2 测试覆盖

协议层的单元测试验证：
1. 序列化/反序列化的正确性与字节序转换
2. 边界条件（零值、最大值、非法输入）
3. 内存安全性（缓冲区边界）

---

*文档版本：1.0*  
*最后更新：2026-04-20*  
*基于 ProtocolDesign 代码实现与架构设计文档*
