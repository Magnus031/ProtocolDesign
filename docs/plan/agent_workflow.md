# Agent 协作工作流

每个 milestone 内，三个 Agent 按三种角色横向分工，互相审查、补位。Milestone 之间轮换角色，避免单一盲区。

---

## 三角色模型

```
Stage 1: 方案评审          Stage 2: 实现            Stage 3: 验证
─────────────────    ─────────────────    ─────────────────
Planner   提出方案    Implementor 写代码   Reviewer   对照 plan 验收
Reviewer A 审查架构   TestWriter 写测试   TestWriter 跑全量测试
Reviewer B 审查鲁棒性 Reviewer   代码审查   Planner    修复漏洞
```

---

## Stage 1 — 方案评审（不写代码）

| 角色 | 职责 | 审什么 |
|------|------|--------|
| **Planner** | 读需求 + 读现有代码 → 出 mini plan | 产出：类设计草图、数据流图、文件清单、测试策略、接口定义 |
| **Reviewer A** | 架构合规性审查 | 是否符合全局约束（ConnContext 模式、无 new/delete、复用 MessageHandler等）？接口是否和 `process.md` 一致？文件拆分是否合理？ |
| **Reviewer B** | 鲁棒性审查 | 竞态条件？资源泄漏？错误路径是否处理？POISONED 恢复？边界条件？测试用例是否覆盖异常路径？ |

**两个 Reviewer 的关注点不重叠**：A 看"做对了没有"，B 看"做漏了没有"。

**你（决策者）在 Stage 1 结束时介入**：阅读 Planner 的方案 + 两份审查意见，判断方案是否通过。如果两个 Reviewer 意见矛盾，你来裁决。

---

## Stage 2 — 实现（写代码）

Plan 经你批准后，三个 Agent 并行启动：

| 角色 | 产出 | 工作方式 |
|------|------|----------|
| **Implementor** | `.h` + `.cpp` + BUILD 修改 | 按 Stage 1 确定的接口和文件清单实现 |
| **TestWriter** | `_test.cpp` | **不等实现完成，基于同一份接口同步开工**。两者在测试跑通时汇合 |
| **Reviewer** | 增量代码审查 | 每完成一个文件就检查：是否偏离了 plan？接口是否一致？命名和风格是否符合 CLAUDE.md？ |

**关键原则**：TestWriter 和 Implementor 基于 Stage 1 输出的同一份接口并行工作——如果测试写起来别扭，说明接口设计有问题，应该在 Stage 2 早期反馈给 Reviewer，而不是硬写。

**你（决策者）在 Stage 2 的介入点**：当 Implementor 发现需要偏离 plan 时，你来判断偏离是否合理，是否需要 Planner 更新方案。

---

## Stage 3 — 验证（把关）

| 角色 | 做什么 |
|------|--------|
| **Reviewer** | 对照 Stage 1 的交付物清单逐项检查：文件存在？→ 打开读内容？→ 内容和 plan 一致？ |
| **TestWriter** | 跑 `bazel test` 全量通过。检查是否有 flaky test（重复跑 3 次观察） |
| **Planner** | 修复 Reviewer 和 TestWriter 发现的问题 |

**你（决策者）在 Stage 3 结束时介入**：亲自跑 `bazel test //...`，通过则合并，失败则指派 Planner 修复。

---

## Milestone 间轮转

同一套三角色每个 milestone 固定，但角色分配在不同 milestone 之间轮换：

```
         M3a          M3b          M4           M5           M6
Planner   Agent A  →  Agent C  →  Agent B  →  Agent A  →  Agent C
ReviewerA Agent B  →  Agent A  →  Agent C  →  Agent B  →  Agent A
ReviewerB Agent C  →  Agent B  →  Agent A  →  Agent C  →  Agent B
```

轮转的目的：
- 每个 Agent 都体验"出方案 → 被审查"和"审查别人方案"两种视角
- 你可以对比不同 Agent 在同一角色上的表现差异，定位自己可以学习的方向
- 避免某个 Agent 的设计偏好成为隐性标准

---

## Agent 启动包模板

每次启动 Agent 时，按以下模板交付任务说明：

```
任务：M<X> — <标题>
你的角色：Planner / Reviewer A / Reviewer B / Implementor / TestWriter

需要读的文档（先读完再动手）：
  1. docs/plan/process.md — M<X> 节
  2. docs/design/architecture.md
  3. 相关现有代码文件路径

你的文件范围：src/<component>/*

你需要交付的（来自 process.md 交付物清单）：
  - [ ] 文件1
  - [ ] 文件2
  - [ ] ...

通过标准：bazel test //src/<component>:* 全部 PASSED
```

---

## 你的三个介入点

```
Stage 1 结束：
  你自己对着 plan 先列出你发现的 gap
  → 然后读两份审查意见，对比自己漏了什么
  → 裁决矛盾意见，批准或驳回方案

Stage 2 过程中：
  当 Implementor 提出偏离 plan
  → 判断偏离是否合理
  → 是否需要 Planner 更新方案

Stage 3 结束：
  → 亲自 bazel test //...
  → 通过 → 合并
  → 失败 → 指派 Planner 修复
```

---

## Stage 1 验收清单（供决策者使用）

这是你在 Stage 1 结束时检查 Planner 方案的框架：

| 检查项 | 问什么 |
|--------|--------|
| 文件清单 | Plan 是否列出了所有需要新增/修改/删除的文件？ |
| 接口契约 | 每个新类的公开方法和参数是否在 plan 中写死？调用方视角是否自然？ |
| 架构一致性 | 是否复用 MessageHandler？是否遵循 ConnContext 模式？是否使用 free_list？ |
| 数据流 | 数据的完整路径是否在 plan 中画清楚了？（哪个线程读、哪个线程写） |
| 测试策略 | 每个测试用例是否对应一个可验证的断言？是否覆盖了错误路径？ |
| 下游影响 | 这个 plan 的接口是否会让下一个 milestone 的 Agent 无法直接复用？ |

**技巧**：Stage 1 结束时，先不看两个 Reviewer 的意见，自己对着交付物清单列一份你发现的 gap。然后对比两份审查意见——你漏掉的就是你需要加强的方向。
