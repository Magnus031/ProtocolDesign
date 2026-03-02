# ProtocolDesign — 基于 UDP 的自定义可靠传输协议

## 目录

1. [项目简介](#1-项目简介)
2. [系统架构](#2-系统架构)
3. [目录结构](#3-目录结构)
4. [协议设计](#4-协议设计)
5. [核心数据结构](#5-核心数据结构)
6. [各模块详解](#6-各模块详解)
7. [关键算法](#7-关键算法)
8. [配置参数](#8-配置参数)
9. [编译与运行](#9-编译与运行)
10. [设计亮点与已知局限](#10-设计亮点与已知局限)

---

## 1. 项目简介

本项目是一个用 **C++17** 编写的、基于 **UDP** 的自定义通信协议演示系统。

### 它能做什么？

- **图像传输**：服务端将本地图片拆包后通过 UDP 发送给客户端，客户端收齐后重组并保存。
- **输入事件流**：客户端将鼠标/键盘事件实时发送给服务端。
- **可靠性保障**：在不可靠的 UDP 之上，通过 ACK 确认 + 超时重传 + 主动请求补包，实现类似 TCP 的可靠传输。
- **网关路由**：所有流量经过一个中间网关转发，客户端和服务端不直接通信。

### 适用场景

- 学习自定义网络协议设计
- 理解 UDP 可靠化改造思路
- 远程桌面/屏幕共享原型

---

## 2. 系统架构

### 拓扑结构

```
┌──────────────┐          ┌──────────────┐          ┌──────────────┐
│   Client     │◄────────►│   Gateway    │◄────────►│  ServerApp   │
│  127.0.0.1   │   UDP    │  127.0.0.1   │   UDP    │  127.0.0.1   │
│  Port 9001   │          │  Port 9000   │          │  Port 9002   │
└──────────────┘          └──────────────┘          └──────────────┘
```

### 数据流向

| 方向 | 内容 |
|------|------|
| Client → Gateway → ServerApp | 鼠标/键盘事件、ACK 确认、补包请求 |
| ServerApp → Gateway → Client | 图像数据包 |

### 线程模型

**Client 端：**
```
主线程          ──► 接收 UDP 包，处理图像数据
维护线程        ──► 监控传输进度，请求缺失包
（输入模拟线程）──► 生成随机输入事件（当前已禁用）
```

**ServerApp 端：**
```
主线程          ──► 接收 UDP 包，分发到队列
事件处理线程    ──► 消费输入事件队列，记录日志
图像发送线程    ──► 顺序发送图像包
传输监控线程    ──► 检测超时，触发重传
目录扫描线程    ──► 每 60 秒重新扫描图像目录
```

---

## 3. 目录结构

```
protocoldesign_/
├── Protocol.h          # 协议消息类型、头部结构、序列化/反序列化接口
├── Protocol.cpp        # 协议序列化/反序列化实现
├── UDPSocket.h         # 跨平台 UDP Socket 封装接口
├── UDPSocket.cpp       # UDP Socket 实现（支持 Windows/Linux）
├── ImageHandler.h      # 图像文件读写、拆包/合包接口
├── ImageHandler.cpp    # 图像处理实现
├── Gateway.cpp         # 网关：消息路由转发（独立可执行程序）
├── Client.cpp          # 客户端：接收图像、发送输入事件（独立可执行程序）
└── ServerApp.cpp       # 服务端：发送图像、接收输入事件（独立可执行程序）
```

> 三个 `.cpp` 文件（Gateway、Client、ServerApp）各自包含 `main()` 函数，分别编译为三个独立的可执行程序。

---

## 4. 协议设计

### 4.1 消息类型

```cpp
enum class MessageType : uint8_t {
    MOUSE_EVENTS    = 0x01,  // 鼠标事件
    KEYBOARD_EVENT  = 0x02,  // 键盘事件
    IMAGE_DATA      = 0x03,  // 图像数据包
    ACK             = 0x04,  // 确认应答
    RETRANSMIT_REQ  = 0x05,  // 补包请求
    ERROR           = 0xFF   // 错误消息
};
```

### 4.2 通用消息格式

所有消息均以一个 **9 字节固定头部** 开头，后跟可变长度载荷：

```
┌─────────────────────────────────────────────────────────┐
│  Byte 0   │  Bytes 1-4    │  Bytes 5-8                  │
│  type     │  messageId    │  dataLength                 │
│  (1 byte) │  (4 bytes BE) │  (4 bytes BE)               │
└─────────────────────────────────────────────────────────┘
│  Payload (dataLength bytes)                             │
└─────────────────────────────────────────────────────────┘
```

> **BE = Big-Endian（大端序/网络字节序）**，所有多字节整数均使用大端序。

### 4.3 各类型载荷格式

**鼠标事件（MOUSE_EVENTS）载荷，9 字节：**
```
┌──────────────┬──────────────────┬──────────────────┐
│ eventType    │ x                │ y                │
│ (1 byte)     │ (4 bytes BE)     │ (4 bytes BE)     │
└──────────────┴──────────────────┴──────────────────┘
```

鼠标事件类型：`MOVE=0x01`、`LEFT_CLICK=0x02`、`RIGHT_CLICK=0x03`、`SCROLL=0x04`

**键盘事件（KEYBOARD_EVENT）载荷，3 字节：**
```
┌──────────────────┬──────────────┐
│ keyCode          │ isPressed    │
│ (2 bytes BE)     │ (1 byte)     │
└──────────────────┴──────────────┘
```

**图像数据（IMAGE_DATA）载荷，格式如下：**
```
┌────────────┬──────────────┬─────────────┬────────────────┬──────────────┬──────────┐
│ imageId    │ totalPackets │ packetIndex │ filenameLength │ filename     │ data     │
│ (4 bytes)  │ (4 bytes)    │ (4 bytes)   │ (4 bytes)      │ (variable)   │(variable)│
└────────────┴──────────────┴─────────────┴────────────────┴──────────────┴──────────┘
```

**ACK / 补包请求（ACK / RETRANSMIT_REQ）载荷，8 字节：**
```
┌────────────┬──────────────┐
│ imageId    │ packetIndex  │
│ (4 bytes)  │ (4 bytes)    │
└────────────┴──────────────┘
```

### 4.4 可靠性机制

UDP 本身不保证可靠性，本协议通过以下机制弥补：

| 机制 | 说明 |
|------|------|
| **ACK 确认** | 客户端每收到一个图像包，立即回复 ACK |
| **超时重传** | 服务端若 2 秒内未收到某包的 ACK，主动重发 |
| **主动补包** | 客户端维护线程发现缺包时，主动发送 RETRANSMIT_REQ |
| **自适应频率** | 补包请求频率随传输进度动态加快（见下节） |
| **总超时保护** | 单次图像传输超过 5 分钟则放弃 |

---

## 5. 核心数据结构

### Protocol.h 中的结构体

```cpp
// 消息头（9字节，内存对齐关闭）
struct MessageHeader {
    MessageType type;      // 1 byte
    uint32_t    messageId; // 4 bytes，大端序
    uint32_t    dataLength;// 4 bytes，大端序
};

// 图像数据包
struct ImageData {
    uint32_t              imageId;
    uint32_t              totalPackets;   // 该图像总包数
    uint32_t              packetIndex;    // 当前包序号（从 0 开始）
    uint32_t              filenameLength;
    std::string           filename;
    std::vector<uint8_t>  data;           // 本包的图像字节
};

// ACK / 补包请求
struct AckData {
    uint32_t imageId;
    uint32_t packetIndex;
};
```

### Client.cpp 中的接收状态

```cpp
struct ImageReceiveState {
    uint32_t imageId;
    uint32_t totalPackets;
    std::string filename;
    std::map<uint32_t, std::vector<uint8_t>> packets;      // 已收到的包
    std::set<uint32_t> receivedPackets;                    // 已收到的包序号
    std::set<uint32_t> acknowledgedPackets;                // 已 ACK 的包序号
    std::chrono::steady_clock::time_point lastUpdate;      // 最后更新时间
};
```

### ServerApp.cpp 中的发送状态

```cpp
struct ImageSendState {
    uint32_t imageId;
    std::string imagePath;
    std::string filename;
    std::vector<std::vector<uint8_t>> packets;             // 所有分包数据
    std::set<uint32_t> sentPackets;                        // 已发送的包
    std::set<uint32_t> acknowledgedPackets;                // 已确认的包
    std::map<uint32_t, time_point> lastSentTime;           // 每包最后发送时间
    std::chrono::steady_clock::time_point startTime;
    bool completed;
};
```

---

## 6. 各模块详解

### 6.1 Gateway（网关）

**文件：** [Gateway.cpp](Gateway.cpp)

最简单的一个组件，只做一件事：**根据发送方端口决定转发目标**。

```
收到来自 9001（Client）的包  →  转发到 9002（ServerApp）
收到来自 9002（ServerApp）的包 →  转发到 9001（Client）
```

网关不解析消息内容，不做任何修改，纯透明转发。

---

### 6.2 UDPSocket（UDP 封装）

**文件：** [UDPSocket.h](UDPSocket.h) / [UDPSocket.cpp](UDPSocket.cpp)

跨平台的 UDP Socket 封装，屏蔽 Windows（Winsock2）和 Linux（POSIX）的差异。

**主要接口：**

| 方法 | 说明 |
|------|------|
| `Initialize()` | 初始化（Windows 下初始化 Winsock） |
| `Create()` | 创建 UDP socket |
| `Bind(port)` | 绑定本地端口 |
| `SetNonBlocking(bool)` | 设置非阻塞模式 |
| `SendTo(data, ip, port)` | 发送 UDP 数据包 |
| `CheckForData()` | 用 `select()` 检查是否有数据可读 |
| `HandleIncomingData(callback)` | 接收数据并通过回调函数处理 |

---

### 6.3 ImageHandler（图像处理）

**文件：** [ImageHandler.h](ImageHandler.h) / [ImageHandler.cpp](ImageHandler.cpp)

负责图像文件的读写和分包/合包。

**主要接口：**

| 方法 | 说明 |
|------|------|
| `LoadImageFile(path)` | 读取图像文件为字节数组 |
| `SaveImageFile(path, data)` | 将字节数组写入图像文件 |
| `ScanImageDirectory(dir)` | 扫描目录，返回所有图像文件路径 |
| `SplitImageData(data, maxSize)` | 将图像数据拆分为最大 8192 字节的包 |
| `MergeImageData(packets, total, out)` | 将所有包合并为完整图像 |

**支持的图像格式：** `.jpg`、`.jpeg`、`.png`、`.bmp`、`.gif`

---

### 6.4 Protocol（协议序列化）

**文件：** [Protocol.h](Protocol.h) / [Protocol.cpp](Protocol.cpp)

负责消息的**序列化**（结构体 → 字节流）和**反序列化**（字节流 → 结构体）。

所有多字节整数在序列化时转换为大端序，反序列化时转回主机字节序。

---

### 6.5 ServerApp（服务端）

**文件：** [ServerApp.cpp](ServerApp.cpp)

**启动流程：**
1. 初始化 UDP Socket，绑定 9002 端口
2. 扫描 `E:\image` 目录，加载所有图像
3. 启动各工作线程
4. 主线程进入接收循环

**图像发送流程：**
```
扫描目录 → 读取图像文件 → SplitImageData 拆包
→ 逐包发送（通过 Gateway 到 Client）
→ 等待 ACK → 超时重传 → 全部 ACK 后标记完成
→ 发送下一张图像
```

**接收处理：**
- 收到 `ACK`：标记对应包已确认
- 收到 `RETRANSMIT_REQ`：立即重发指定包
- 收到 `MOUSE_EVENTS` / `KEYBOARD_EVENT`：放入事件队列，由事件线程处理

---

### 6.6 Client（客户端）

**文件：** [Client.cpp](Client.cpp)

**启动流程：**
1. 初始化 UDP Socket，绑定 9001 端口
2. 启动维护线程
3. 主线程进入接收循环

**图像接收流程：**
```
收到 IMAGE_DATA 包 → 记录到 ImageReceiveState
→ 发送 ACK → 检查是否收齐所有包
→ 收齐后调用 MergeImageData 合包
→ SaveImageFile 保存到 D:\image
```

**维护线程职责：**
- 定期检查各图像的接收进度
- 对缺失的包发送 `RETRANSMIT_REQ`
- 根据进度自适应调整请求频率
- 超过 5 分钟未完成则放弃该图像

---

## 7. 关键算法

### 7.1 图像拆包

```
图像总大小 / 8192 = 包数（向上取整）

例：一张 20000 字节的图像：
  包 0：字节 0    ~ 8191   （8192 字节）
  包 1：字节 8192 ~ 16383  （8192 字节）
  包 2：字节 16384~ 19999  （3616 字节，最后一包可能更小）
```

### 7.2 自适应补包频率

客户端维护线程根据当前接收进度动态调整等待间隔：

| 接收进度 | 补包请求间隔 |
|----------|-------------|
| < 20%    | 2000 ms     |
| 20% ~ 50% | 1000 ms   |
| 50% ~ 80% | 500 ms    |
| > 80%    | 200 ms      |

> 越接近完成，请求越频繁，加速收尾阶段。

### 7.3 图像合包验证

合包时进行以下验证：
1. 已收到的包数量 == totalPackets
2. 包序号连续（0, 1, 2, ... totalPackets-1），无缺失
3. 合并后总大小 > 0

---

## 8. 配置参数

所有配置均为代码中的编译期常量（无配置文件）：

### 网络配置

| 参数 | 值 | 说明 |
|------|----|------|
| GATEWAY_IP | 127.0.0.1 | 网关 IP |
| GATEWAY_PORT | 9000 | 网关端口 |
| CLIENT_PORT | 9001 | 客户端端口 |
| APP_PORT | 9002 | 服务端端口 |

### 协议参数

| 参数 | 值 | 说明 |
|------|----|------|
| MAX_PACKET_SIZE | 8192 字节 | 单包最大数据量 |
| ACK_TIMEOUT | 2000 ms | 服务端等待 ACK 超时 |
| PACKET_TIMEOUT | 1000 ms | 包级别超时 |
| TRANSFER_TIMEOUT | 300000 ms（5分钟）| 整体传输超时 |
| SCAN_INTERVAL | 60000 ms（1分钟）| 目录重扫间隔 |

### 文件路径

| 参数 | 值 | 说明 |
|------|----|------|
| 服务端图像源目录 | `E:\image` | 服务端读取图像的目录（Windows 路径） |
| 客户端图像保存目录 | `D:\image` | 客户端保存图像的目录（Windows 路径） |

---

## 9. 编译与运行

### 依赖

- C++17 编译器（MSVC / GCC / Clang）
- Windows：Winsock2（系统自带）
- Linux：POSIX threads（`-lpthread`）

### Windows（MSVC）

```bat
:: 编译网关
cl /EHsc /std:c++17 Gateway.cpp Protocol.cpp UDPSocket.cpp /Fe:Gateway.exe

:: 编译客户端
cl /EHsc /std:c++17 Client.cpp Protocol.cpp UDPSocket.cpp ImageHandler.cpp /Fe:Client.exe

:: 编译服务端
cl /EHsc /std:c++17 ServerApp.cpp Protocol.cpp UDPSocket.cpp ImageHandler.cpp /Fe:ServerApp.exe
```

### Linux（g++）

```bash
# 编译网关
g++ -std=c++17 -pthread Gateway.cpp Protocol.cpp UDPSocket.cpp -o Gateway

# 编译客户端
g++ -std=c++17 -pthread Client.cpp Protocol.cpp UDPSocket.cpp ImageHandler.cpp -o Client

# 编译服务端
g++ -std=c++17 -pthread ServerApp.cpp Protocol.cpp UDPSocket.cpp ImageHandler.cpp -o ServerApp
```

### 运行顺序

必须按以下顺序启动（网关需最先运行）：

```bash
# 终端 1：先启动网关
./Gateway

# 终端 2：启动服务端（确保 E:\image 目录存在且有图像）
./ServerApp

# 终端 3：启动客户端（确保 D:\image 目录存在）
./Client
```

---

## 10. 设计亮点与已知局限

### 设计亮点

| 亮点 | 说明 |
|------|------|
| 模块化设计 | 协议、Socket、图像处理各自独立，职责清晰 |
| 跨平台支持 | UDPSocket 封装了 Windows/Linux 差异 |
| UDP 可靠化 | ACK + 超时重传 + 主动补包，三重保障 |
| 自适应策略 | 补包频率随进度动态调整，提升传输效率 |
| 非阻塞 I/O | 使用 `select()` 避免阻塞，提升响应性 |
| 线程安全 | 共享状态均使用 mutex 保护 |

### 已知局限

| 局限 | 说明 |
|------|------|
| 硬编码路径 | 图像目录为 Windows 绝对路径，跨平台需修改 |
| 无配置文件 | 所有参数为编译期常量，修改需重新编译 |
| 无数据校验 | 没有 CRC/校验和，无法检测数据损坏 |
| 无加密认证 | 数据明文传输，无身份验证 |
| 仅支持本机 | IP 硬编码为 127.0.0.1，无法跨网络使用 |
| 内存占用 | 整张图像完整加载到内存 |
| 网关单线程 | 高并发下网关可能成为瓶颈 |
