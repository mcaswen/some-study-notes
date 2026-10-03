# `Netcode for Entities`

**`Netcode for Entities`** 是 Unity 提供的两套联网方案之一。与另一个面向 `GameObject` 的方案不同，`Netcode for Entities` 采用**权威服务器**（authoritative server）并支持**客户端预测**（client-side prediction），因此更适合**节奏快速、对抗性强**的游戏。

## 权威服务器（Authoritative server）

与其把对“游戏里到底发生了什么”的**控制权**分散在玩家机器上，不如让一台**权威服务器**运行**完整的游戏模拟**，并由它来“裁决”游戏状态。客户端把玩家输入发给服务器，服务器更新游戏模拟，然后把**新的状态快照**回传给客户端。实现起来**更简单**，也**更不容易被外挂利用**。

## 客户端预测（Client-side prediction）

网络有延迟，客户端总是**落后于服务器**一点点。很多元素可以容忍这种滞后，但例如**玩家角色**就不行：延迟会直接**破坏手感**。客户端预测用于解决这个问题：对指定元素（如玩家角色），客户端会尝试把它的状态**预测到未来一小段时间**；只要这些预测**足够准确且稳定**地与服务器状态一致，玩家体验就会**近似零延迟**。

除了这两个核心特性之外，`Netcode for Entities` **相较** `Netcode for GameObjects` 还有更好的**可扩展性**以及更丰富的**带宽优化**手段。

### 入门与样例

一个不错的起点是 **Netcode for Entities Samples** 仓库。它涵盖从**基础到进阶**的功能，包括**状态同步**、**连接流程**、与 **Unity Physics** 的集成等。建议从 **Networked Cube** 教程开始，内容包括：建立与服务器的连接；与服务器通信；在服务器上生成**同步实体**；打包**服务器/客户端**独立构建；以及在编辑器里**同时运行**服务器与一个客户端。

此外还有 **ECS Network Racing** 样例：一个**大厅制**的多人赛车 Demo，集成了 **Unity Physics** 与 **Vivox** 语音。

---

# Character Controller

**CharacterController** 包提供了**基于 ECS** 的**第一/第三人称**角色控制器实现，可与 **Unity Physics** 和 **Netcode for Entities** 协同工作；内置支持**冲刺**、**二段跳**等常见角色行为。你也可以通过它配套的**教程与样例**继续学习；该包可在 **Unity Asset Store** 获取。