# GKC Source Reading Roadmap

> 当前 roadmap 基于 `third_party/GKC` submodule 快照 **`51049a4`**。
> 相较此前的 `1152096`，这次上游更新最直接的变化是：
> - 新增 `public/include/base/GkcCga.h` 以及 `_cga_/` 下的基础 CGA / raster 头文件
> - 在基础层补充了 multi-dimensional array 相关类型（`MdaInfo`、`MdaView`、`MdaIterator`、`MdaHelper` 等）

## 背景

当前项目中，`src/` 目录下的业务代码已经做过亲自阅读和停下来整理；真正缺失的一手理解，集中在导师提供的 `third_party/GKC` 上。

过去主要依赖 agent 帮忙阅读 GKC 并整理文档，这种方式在前期提速有效，但它天然会带来几个问题：

- 知道 API 名称和大致用途，但不知道它背后的对象模型和约束。
- 知道“怎么调用”，但不知道“为什么这么设计”。
- 对 `IoPool`、`LiteCom`、GUI host、平台层边界缺乏自己的判断，后续设计容易被库牵着走。
- 出现问题时，只能继续追问 agent，难以独立逆推到底层实现。

因此，接下来需要做的不是“通读整个 GKC”，而是建立一个有边界、有主线、有产出的源码阅读计划，把二手理解变成一手理解。

## 核心目标

这轮阅读的目标不是掌握 GKC 的全部能力，而是回答以下 8 个问题：

1. GKC 的整体分层结构是什么？
2. `System / Runtime / Application` 三层是如何划分的？
3. `LiteCom`、接口结构体、helper 类之间分别承担什么角色？
4. `IoPool` 的对象模型、回调模型和生命周期是什么？
5. `IUiHost`、`GkcGui.h`、`util/private/include/ui`、`util/gui/uihost` 分别处在什么层次？
6. GUI 相关抽象如何把业务代码接到平台窗口系统上？
7. 这次上游新增的 `GkcCga` 与 multi-dimensional array 需要了解到什么程度？
8. 当前项目究竟依赖了 GKC 的哪些能力，没依赖哪些能力？

只要这 8 个问题能够用自己的话回答清楚，这轮阅读就已经成功。

## 阅读边界

### 不做的事

本轮不追求：

- 通读 `third_party/GKC` 全部代码。
- 深入所有 `Compiler`、`tools`、`util`、`references` 子项目的实现。
- 把所有平台分支同时读透。
- 一开始就钻进细节实现。

### 重点范围

本轮主线只聚焦以下三层：

- 接口与类型层
  - `third_party/GKC/public/include/base/*`
  - `third_party/GKC/public/include/sys/*`
  - 其中最重要的是：
    - `GkcDef.h`
    - `GkcFrame.h`
    - `GkcGui.h`
    - `GkcMath.h`
    - `GkcCga.h`
- 运行时实现层
  - `third_party/GKC/RT/GkcSys/public/_GkcSys.h`
  - `third_party/GKC/RT/GkcSys/include/*`
  - `third_party/GKC/RT/GkcSys/src/*`
- 与本项目直接相关的 GUI / IO 路径
  - `GkcGui`
  - `IoPool`
  - `util/private/include/ui`
  - `util/gui/uihost`
  - `util/gui/LocalDesk`
  - 必要的 Linux 平台实现

### 当前版本下的优先级判断

基于 `51049a4` 这版 GKC，推荐把阅读对象分成三档：

- 第一优先级：直接关系你课题的
  - `GkcDef.h`
  - `GkcGui.h`
  - `GkcSys.h`
  - `_GkcSys.h`
  - `RT/GkcSys/src/pool/IoPool.cpp`
  - `util/private/include/ui/*`
  - `util/gui/uihost/*`
  - `util/gui/LocalDesk/*`
- 第二优先级：帮助理解对象模型和框架风格的
  - `GkcFrame.h`
  - `GkcMath.h`
  - `doxygen/development/base.dox`
  - `doxygen/development/sys.dox`
- 第三优先级：只建立存在感，不深读
  - `GkcCga.h`
  - `_cga_/*`
  - multi-dimensional array 相关类型

原因很简单：这次新增的 CGA / MDA 对你当前 ProtocolDesign 的直接影响很小，知道它们已经进入公共基础层、未来可能扩展图形和矩阵处理就够了，不值得抢走 `IoPool` 和 UI host 的阅读时间。

### 辅助范围

以下目录只作为验证理解和查找用法的材料，不作为主线：

- `third_party/GKC/references/*`
- `third_party/GKC/util/*`
- `third_party/GKC/test/*`

## 总体策略

整体阅读顺序遵循四条原则：

1. 先建立全局地图，再看接口，再看关键实现。
2. 先读与当前课题强相关的薄片，不横向扩张。
3. 先回答“它是什么”和“边界在哪里”，再回答“细节如何实现”。
4. 每读完一个模块，都要产出自己的笔记，而不是继续堆积二手摘要。

另外再补一条当前版本最重要的原则：

5. 先区分“公共接口定义在哪里”和“真实平台实现在哪里”，不要把 `GkcGui.h` 和 `uihost` 可执行程序混成一层。

## 阶段划分

## 第 0 阶段：先定问题和范围

目标：防止阅读失控。

任务：

- 明确这轮阅读对象是 `third_party/GKC`，不是项目业务代码。
- 明确主问题是对象模型、生命周期、框架边界，而不是所有 API 细节。
- 记录当前项目里已经依赖到的 GKC 能力，例如：
  - `IoPool`
  - GUI 抽象
  - 基础类型与字符串体系
  - 可能使用到的组件模型

产出：

- 一页简短的“本轮只读什么、不读什么”清单。

## 第 1 阶段：先建立 GKC 全局地图

目标：先知道这套库大致怎么分层，而不是一上来陷在实现细节里。

建议阅读文件：

- [third_party/GKC/doxygen/index.dox](/home/magnus/dev/ProtocolDesign/third_party/GKC/doxygen/index.dox)
- [third_party/GKC/doxygen/intro.dox](/home/magnus/dev/ProtocolDesign/third_party/GKC/doxygen/intro.dox)
- [third_party/GKC/doxygen/development/base.dox](/home/magnus/dev/ProtocolDesign/third_party/GKC/doxygen/development/base.dox)
- [third_party/GKC/doxygen/development/sys.dox](/home/magnus/dev/ProtocolDesign/third_party/GKC/doxygen/development/sys.dox)

这一阶段只回答这些问题：

- GKC 是按什么思想分层的？
- 哪些目录属于 `System`？
- 哪些目录属于 `Runtime`？
- 哪些目录属于 `Application`？
- 当前项目 include 到的头文件主要来自哪一层？
- `public/include`、`RT/GkcSys`、`util/private/include/ui`、`util/gui/uihost` 分别是什么关系？

产出：

- 一张 GKC 分层图。
- 一页“当前项目依赖了 GKC 哪一层”的简表。

建议你在这一阶段就先画出下面这张最小地图：

```text
public/include/base/GkcDef.h
  -> 定义基础类型、UiMessage、IUiHost、LiteCom/RefPtr/UniquePtr

public/include/base/GkcGui.h
  -> 在 IUiHost 之上提供 _Window / ToplevelImpl / WorkImpl / TimerImpl

public/include/sys/GkcSys.h
  -> 对 _GkcSys.h 的 helper 包装

RT/GkcSys/public/_GkcSys.h
  -> runtime 暴露给上层的 C 风格接口

util/private/include/ui/*
  -> UI host 的平台私有公共基底

util/gui/uihost/*
  -> 真正的 UI host 可执行程序与平台实现拼装

util/gui/LocalDesk/*
  -> 最小 GUI 应用样例
```

## 第 2 阶段：建立对象模型和接口词汇表

目标：先读对外接口，建立语言体系，避免后面看实现时反复卡住。

建议阅读文件：

- [third_party/GKC/public/include/base/GkcDef.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/base/GkcDef.h)
- [third_party/GKC/public/include/base/GkcFrame.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/base/GkcFrame.h)
- [third_party/GKC/public/include/base/GkcMath.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/base/GkcMath.h)
- [third_party/GKC/public/include/base/GkcGui.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/base/GkcGui.h)
- [third_party/GKC/public/include/base/GkcCga.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/base/GkcCga.h)
- [third_party/GKC/public/include/sys/GkcSys.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/sys/GkcSys.h)
- [third_party/GKC/RT/GkcSys/public/_GkcSys.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/public/_GkcSys.h)

重点理解的概念：

- `CallResult`
- `ConstStringS` 及字符体系
- `UniquePtr`
- `RefPtr`
- `LiteCom`
- helper 类与底层接口结构的关系
- `pContext + function pointer` 这种接口风格的含义
- `UiMessageHandler`
- `IUiWindow / IUiToplevel / IUiHost`
- `ToplevelImpl<T> / WorkImpl<T> / TimerImpl<T>`

这一阶段不要求完全掌握实现，只要求回答：

- 对象所有权是谁在管？
- 哪些抽象是“给用户直接用”的？
- 哪些抽象是“runtime 暴露的接口”？
- helper 是真正的抽象边界，还是只是薄封装？
- `IUiHost` 为什么定义在 `GkcDef.h` 而不是 `GkcGui.h`？
- `GkcCga.h` 的定位是什么，为什么它被放进公共基础头而不是独立工具目录？

产出：

- 一份“GKC 词汇表”。
- 一张“对象模型关系图”。

这一阶段的阅读顺序建议改成：

1. `GkcDef.h`
2. `GkcFrame.h`
3. `GkcGui.h`
4. `GkcSys.h`
5. `_GkcSys.h`
6. `GkcMath.h`
7. `GkcCga.h`

原因：

- 先读 `GkcDef.h`，你才能真正看懂 `IUiHost`、`UiMessage*` 和所有权模型。
- 先读 `GkcGui.h`，再去读 `uihost`，不然你会把接口包装和平台实现混在一起。
- `GkcCga.h` 应该放在这阶段最后，只做存在性确认，不要提前钻进去。

## 第 3 阶段：专攻 IoPool 和系统运行时

目标：读透当前项目最依赖的运行时薄片。

建议阅读文件：

- [third_party/GKC/RT/GkcSys/public/_GkcSys.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/public/_GkcSys.h)
- [third_party/GKC/RT/GkcSys/include/base/system/sys_io.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/include/base/system/sys_io.h)
- [third_party/GKC/RT/GkcSys/include/base/system/sys_work.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/include/base/system/sys_work.h)
- [third_party/GKC/RT/GkcSys/src/pool/IoPool.cpp](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/src/pool/IoPool.cpp)
- [third_party/GKC/RT/GkcSys/src/pool/WorkPool.cpp](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/src/pool/WorkPool.cpp)

Linux 分支可继续阅读：

- [third_party/GKC/RT/GkcSys/include/base/system/Linux/_util_/_x_epoll.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/include/base/system/Linux/_util_/_x_epoll.h)
- [third_party/GKC/RT/GkcSys/include/base/system/Linux/_util_/_x_socket.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/include/base/system/Linux/_util_/_x_socket.h)
- [third_party/GKC/RT/GkcSys/include/base/system/Linux/sys_io.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/RT/GkcSys/include/base/system/Linux/sys_io.h)

这一阶段重点回答：

- `IoPool` 是单例、组件还是普通对象？
- 获取、启动、注册回调、关闭的调用顺序是什么？
- 回调在哪个线程触发？
- 连接上下文由谁持有？
- `Disable` 的语义是什么？
- 接收和发送缓冲的边界由谁控制？
- `GkcSys.h` 里的 helper 和 `_GkcSys.h` 的原始接口分别适合在哪一层使用？

产出：

- 一张 `IoPool` 生命周期图。
- 一份 `IoPool` 使用约束清单。
- 一页“当前项目中 AppHost / Gateway / Client 如何借用 IoPool”的对照说明。

## 第 4 阶段：专攻 GUI 抽象和宿主模型

目标：理解 GKC 为什么适合作为当前课题的 UI host 基础。

建议阅读文件：

- [third_party/GKC/public/include/base/GkcGui.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/base/GkcGui.h)
- [third_party/GKC/public/include/base/GkcGui.cpp](/home/magnus/dev/ProtocolDesign/third_party/GKC/public/include/base/GkcGui.cpp)
- [third_party/GKC/util/private/include/ui/UIDef.h](/home/magnus/dev/ProtocolDesign/third_party/GKC/util/private/include/ui/UIDef.h)
- [third_party/GKC/util/private/include/ui/GkcUIMain.cpp](/home/magnus/dev/ProtocolDesign/third_party/GKC/util/private/include/ui/GkcUIMain.cpp)
- `third_party/GKC/util/private/include/ui/system/*`
- `third_party/GKC/util/gui/uihost/*`
- `third_party/GKC/util/gui/LocalDesk/*`

这一阶段重点回答：

- `IUiHost` 的公共接口在哪里定义？
- UI host 的平台私有基底在哪里？
- `uihost` 可执行程序具体做了什么？
- `ToplevelImpl` 的职责是什么？
- 绘制回调和输入事件如何进入业务对象？
- UI host 提供了什么能力，业务对象又负责什么？
- 平台层、宿主层、业务层的边界在哪里？
- 当前项目的 apphost / client host 模式与 GKC 的 GUI 结构在哪些地方天然对齐？

产出：

- 一张 GUI host 分层图。
- 一页“GKC GUI 抽象如何映射到本项目”的说明。

建议这一阶段严格按下面顺序读：

1. `GkcDef.h` 里 `UiMessage*`、`IUiWindow*`、`IUiHost`
2. `GkcGui.h` 里 `_Window`、`_WindowImpl`、`ToplevelImpl`、`WorkImpl`、`TimerImpl`
3. `util/private/include/ui/UIDef.h`
4. `util/private/include/ui/GkcUIMain.cpp`
5. `util/gui/uihost/include/base/SysDef.h`
6. `util/gui/uihost/include/base/SysDef.cpp`
7. `util/gui/uihost/src/Main.cpp`
8. `util/gui/LocalDesk/src/view/MainWindow.h`
9. `util/gui/LocalDesk/src/Main.cpp`

这条顺序非常重要，因为它恰好对应：

- 接口定义
- 包装层
- 平台私有公共层
- 实际宿主进程
- 最小用户程序

## 第 5 阶段：用 references 和 util 做反向验证

目标：不是继续扩读，而是验证之前的理解是否正确。

使用方式：

- 每读完一个关键抽象，就到 `references` 或 `util` 中找真实用法。
- 重点看“这个抽象在真实程序里如何接线”，而不是继续横向读更多模块。
- 如果样例中的用法和自己的理解不一致，回到接口层重新校正。

优先验证的对象：

- `LiteCom`
- `IoPool`
- GUI host / `ToplevelImpl`
- 字符串与路径体系
- `uihost` 到 `_SA_UIMain` 的调用链

产出：

- 每个关键抽象对应一个“真实使用范式”示例。

当前版本下，验证样例优先用：

- `util/gui/LocalDesk`
- `util/gui/scripts/Linux/LocalDesk.sh`
- `util/gui/uihost/src/Main.cpp`

`references/*` 仍然可以看，但不作为 GUI 主线的第一选择，因为 `util/gui` 这条链更贴近你项目现在的插件宿主模式。

## 第 6 阶段：重新映射回本项目

目标：让这轮阅读最终服务当前课题，而不是停留在库本身。

任务：

- 回到项目 `src/`，逐处标记：
  - 这里依赖了 GKC 的什么机制？
  - 这里假设了 GKC 的什么行为？
  - 这里是否误解了 GKC 的边界？
- 把“之前依赖 agent 转述的理解”和“现在亲自阅读后的理解”做一次对账。

产出：

- 一份“本项目对 GKC 的依赖面”清单。
- 一份“已经确认的理解”和“仍需继续验证的问题”清单。

## 7 天执行计划

为了避免阅读节奏失控，建议按 7 天执行，每天只攻一个主轴。

### 第 1 天：建立全局地图

阅读：

- `doxygen/index.dox`
- `doxygen/intro.dox`
- `doxygen/development/base.dox`
- `doxygen/development/sys.dox`

回答：

- GKC 怎么分层？
- 哪层和当前项目最相关？

产出：

- GKC 分层图
- 项目依赖层次表

### 第 2 天：建立基础词汇表

阅读：

- `public/include/base/GkcDef.h`
- `public/include/base/GkcFrame.h`
- `public/include/base/GkcMath.h`
- `public/include/base/GkcCga.h`（只看入口与模块结构，不深读 `_cga_/*`）

回答：

- 基础类型体系和所有权体系是什么？
- 常见宏、字符串、容器和结果类型的风格是什么？
- `IUiHost` / `UiMessage*` 为什么放在 `GkcDef.h`？
- `GkcCga` 在上游中的定位是什么？

产出：

- GKC 概念卡：`CallResult`、`UniquePtr`、`RefPtr`、字符串体系、`IUiHost`

### 第 3 天：建立系统接口理解

阅读：

- `public/include/sys/GkcSys.h`
- `RT/GkcSys/public/_GkcSys.h`

回答：

- helper 和 runtime 接口之间是什么关系？
- 用户真正依赖的是哪个边界？
- 哪些接口应该在业务代码里直接面对，哪些更适合被包装后使用？

产出：

- helper / runtime 对照表

### 第 4 天：专读 IoPool 接口

阅读：

- `_GkcSys.h` 中的 IO 相关接口
- `RT/GkcSys/include/base/system/sys_io.h`
- `RT/GkcSys/include/base/system/sys_work.h`

回答：

- `IoPool` 的 API 面是什么？
- 使用时有哪些必须遵守的顺序和约束？

产出：

- `IoPool` 接口卡
- 生命周期草图

### 第 5 天：专读 IoPool Linux 实现

阅读：

- `RT/GkcSys/src/pool/IoPool.cpp`
- Linux `_x_epoll.h`
- Linux `_x_socket.h`

回答：

- Linux 下事件循环和 socket 处理是怎么接进来的？
- 哪些行为是实现细节，哪些是对上层稳定暴露的约束？

产出：

- `IoPool` 生命周期图
- Linux 实现备注

### 第 6 天：专读 GUI 抽象

阅读：

- `public/include/base/GkcGui.h`
- `public/include/base/GkcGui.cpp`
- `util/private/include/ui/UIDef.h`
- `util/private/include/ui/GkcUIMain.cpp`
- `util/gui/uihost`
- `util/gui/LocalDesk`

回答：

- 绘制与输入事件的分发路径是什么？
- 宿主和业务对象如何对接？
- `uihost` 进程如何加载 `_SA_UIMain`？
- 你的 AppHost 假宿主和 GKC 真宿主，边界对应关系是什么？

产出：

- GUI host 分层图
- 与本项目 apphost / client 的映射说明

### 第 7 天：回到项目做映射和对账

阅读：

- 项目中所有直接依赖 GKC 的关键位置

回答：

- 当前项目在哪些地方真正依赖了 GKC？
- 哪些认知之前是模糊的，现在已经明确？
- 哪些问题仍需后续继续验证？

产出：

- GKC 依赖面清单
- 已确认 / 待验证问题表

## 笔记模板

为了避免重新陷入“大而全文档”的陷阱，建议只保留三类笔记。

### 1. 概念卡

适用于单个核心概念，例如：

- `LiteCom`
- `IoPool`
- `ToplevelImpl`
- `CallResult`
- `IUiHost`
- `_SA_UIMain`

建议模板：

- 它是什么？
- 它解决什么问题？
- 它和相邻抽象的关系是什么？
- 使用它时最容易误解什么？

### 2. 调用链

适用于一个功能路径，例如：

- `IoPool` 的连接建立
- GUI 事件进入业务对象
- 绘制回调进入宿主层
- `uihost` → `_SA_UIMain` → `ToplevelImpl` 的调用路径

建议模板：

- 入口在哪里？
- 中间经过哪些对象？
- 生命周期边界在哪里？
- 哪一步最关键？

### 3. 约束清单

适用于记录不能忘的边界条件，例如：

- 线程模型
- 所有权模型
- 回调时机
- 平台差异
- 必须按顺序调用的 API
- 哪些头是“公共接口”，哪些文件是“平台实现”

## 与 agent 的分工

后续不建议继续把 agent 当成“替你理解 GKC 本质”的主体，而应该把它降级为检索器、对照器和整理器。

### 自己亲自读的部分

- 对象模型
- 生命周期
- 框架边界
- 关键实现
- 与课题设计直接相关的核心抽象

### 可以交给 agent 的部分

- 长文件摘要
- 跨文件跳转和索引
- 样例搜索
- 帮忙做接口对照表
- 帮忙整理“哪里和哪里有关联”

### 明确不再交给 agent 代替完成的部分

- “这个库本质是什么”的最终判断
- “这个抽象的边界在哪里”的最终判断
- “这个实现约束会如何影响我的设计”的最终判断

## 完成标准

这份 roadmap 执行完成后，至少应达到以下状态：

- 可以不用 agent，自己解释 GKC 的基本分层。
- 可以自己说明 `LiteCom`、`IoPool`、GUI host 的角色和边界。
- 可以自己解释 `GkcDef.h`、`GkcGui.h`、`util/private/include/ui`、`util/gui/uihost` 四者的分工。
- 可以指出当前项目依赖 GKC 的真实薄片，而不是笼统地说“依赖 GKC”。
- 可以区分“稳定接口”与“平台实现细节”。
- 可以说明这次上游新增的 `GkcCga` / multi-dimensional array 对当前项目“重要但不紧急”的位置。
- 后续再问 agent 时，问题会从“这是什么”升级为“这里的设计取舍是否合理”。

## 当前版本下的推荐主路径

如果你现在只想要一条最稳、最不容易迷路的阅读路径，直接按下面走：

1. `doxygen/intro.dox`
2. `doxygen/development/base.dox`
3. `doxygen/development/sys.dox`
4. `public/include/base/GkcDef.h`
5. `public/include/base/GkcGui.h`
6. `public/include/sys/GkcSys.h`
7. `RT/GkcSys/public/_GkcSys.h`
8. `RT/GkcSys/src/pool/IoPool.cpp`
9. `util/private/include/ui/UIDef.h`
10. `util/private/include/ui/GkcUIMain.cpp`
11. `util/gui/uihost/include/base/SysDef.h`
12. `util/gui/uihost/src/Main.cpp`
13. `util/gui/LocalDesk/src/view/MainWindow.h`
14. `public/include/base/GkcCga.h`

这条路径的逻辑是：

- 先有分层图
- 再有对象模型
- 再有 runtime
- 再有 UI host
- 最后再看本次更新新增但暂时不阻塞你的内容

## 最后的提醒

这轮源码阅读不是为了变成 GKC 的维护者，而是为了让自己重新掌握主动权。

目标不是读得多，而是读完以后：

- 对底层抽象有自己的语言；
- 对设计边界有自己的判断；
- 对项目依赖关系有自己的地图。

只要做到这一点，后面的实现和设计讨论就会稳很多。
