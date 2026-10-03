#### 一、概念与定位

`Ghost` 是 Netcode for Entities 中对“可复制的网络实体”的抽象：服务器端以 `Ghost` 预制体为单位生成快照，按网络 `Tick` 序列化并发送到各客户端；客户端基于快照还原或更新对应的实体，进而以“预测或插值”的方式参与本地仿真与呈现。

该抽象将“状态如何被复制”“以何种频率传输”“在本地以何种方式使用”分层解耦，并以固定的 `Tick` 语义连接至预测与插值两条时间轴。这一过程总称为“Ghost 同步（`Ghost Synchronization`）与快照（`Snapshots`）”。

#### 二、数据流与工作原理（服务端→客户端）

服务器端在 `NetworkTickRate` 的节奏下遍历活跃 Ghost，以字段级序列化生成**快照帧**；每个快照根据客户端的确认掩码（`ack mask`）选择 0–3 个基线（`baseline`），对本帧数据做**预测式增量压缩**，并通过不可靠通道流式发送。

该压缩策略将组件数据与若干基线对齐并做差分，再经熵编码模型打包，显著降低带宽成本。客户端按序接收并在本地基于基线重建出“当前帧”的 `Ghost` 组件值，然后更新已有实体或触发生成/回收。该机制的关键是“面向 `Tick` 的增量 + 基线选择”，从而在包丢失与乱序条件下保持高还原率与稳定带宽。

#### 三、预测、插值与`Owner`语义

`Ghost` 的“本地使用方式”由模式决定：**`Interpolated`** 表示只做插值呈现；**`Predicted`** 表示在本地按 Tick 参与预测仿真；**`OwnerPredicted`** 表示在拥有者客户端“预测”，其他客户端“插值”。

该模式可在 `Authoring` 阶段声明，也可在满足约束前提下于运行时在 `Predicted` 与 `Interpolated` 之间切换。拥有者语义通过 `GhostOwner` 的 `NetworkId` 与连接 `ID` 匹配确定，是“`OwnerPredicted`”正确生效的前置。此设计允许同一网络对象在不同客户端以差异化代价参与仿真与表现，达到手感与成本的平衡。

#### 四、序列化语义：字段选择、量化与变体

组件参与复制需以 `[GhostField]` 标注字段；字段可配置量化步长与平滑动作（如插值），非标注字段在网络层面不被传输。对于外部组件或想要针对不同 `Ghost` 采用不同复制策略的场景，可通过“变体（`Variants`）”定义多套序列化方案，并在烘焙时为具体 `Ghost` 选择合适的变体。

类型模板（`Type Templates`）与变体系统共同决定代码生成与序列化路径，形成“按字段可控、按类型可扩”的复制管线。

#### 五、生命周期与拓扑：生成、相关性与优化

`Ghost` 的生成与销毁是通过“快照流中的生成/回收标记”在客户端重放的。然而，并非所有 `Ghost` 必须对所有客户端可见，服务器通往往通过**重要性（`Importance`）与关联性（`Relevancy`）** 筛选要发送的 `Ghost` 子集，从而进一步降低带宽与反序列化开销。

大量 `Ghost` 的场景可启用预序列化、重要性缩放与优化模式，以在“吞吐—延迟—画质”的三角中做工程级取舍。

#### 六、与 `Tick` / 预测回滚的结合

客户端应用快照后，**`Interpolated Ghost`** 仅参与插值链路；**`Predicted Ghost`** 在 `PredictedSimulationSystemGroup` 内按 `ServerTick` 复演过去到当前的 Tick 序列，实现“回滚 + 重放”的误差收敛。

两类 `Ghost` 共享同一套快照输入，但在本地时间轴上的职责不同：前者优化观感稳定，后者保障交互手感与一致性。`Ghost` 抽象以“快照作为权威锚点”的方式与 PSG 的预测循环耦合，形成“可重复的状态转移（快照→预测→校正）”。

#### 七、方案选择的动因与取舍

选择“`Ghost` + 快照流”的方案而非纯 `RPC` 状态推送或锁步输入转发，基于以下约束与目标：

其一，快照–基线–差分模型具有**带宽可控性**，可通过字段级量化与重要性裁剪将成本稳定在可预期区间；

其二，服务器权威 + 客户端预测的结构允许在弱网环境维持“近实时手感”，并通过回滚消解误差；

其三，变体与模板使“复制粒度与格式”具备工程可塑性，可按对象类型与距离分层调优；

其四，`Ghost` 模式（预测/插值/`Owner`）在同一抽象内统一表达，利于大规模项目的系统化管理。相关机制在 Netcode 包中以通用 API 与代码生成落地，避免重复造轮。

#### 八、客户端实践

**组件字段的复制控制与量化：**
```
using Unity.NetCode;
using Unity.Mathematics;
using Unity.Entities;

public struct GhostMovement : IComponentData
{
    // 位置：毫米级量化 + 插值
    [GhostField(Quantization = 1000, Smoothing = SmoothingAction.Interpolate)]
    public float3 Position;

    // 朝向：角度量化 + 大跳变禁用插值
    [GhostField(Quantization = 512, Smoothing = SmoothingAction.Interpolate, MaxSmoothingDistance = 0.2f)]
    public quaternion Rotation;

    // 速度：合成位对齐压缩（作为小结构整体变化）
    [GhostField(Composite = true, Quantization = 1000)]
    public float3 Velocity;
}

```
该示例展示了“字段显式标注 + 量化 + 插值平滑”的复制语义，未标注的本地字段不随快照传输。

**Ghost 预制体与模式：**

`// Authoring 面板设置：DefaultGhostMode = OwnerPredicted `
`// 运行时可在满足前提下进行 Predicted/Interpolated 切换（非 OwnerPredicted）`

该配置指示拥有者侧采用预测，其余客户端采用插值；若需运行时切换，需满足“支持所有模式、当前非 `OwnerPredicted`、从 `Interpolated` 切到 `Predicted` 或反向”等约束。

#### 九、结论

`Ghost` 将“服务器权威状态”组织为按 `Tick` 的快照流，以字段级可配置的序列化与差分压缩完成跨端复制，并在客户端通过“插值链路”与“预测链路”分别承担稳定呈现与交互一致性。

模式、变体与优化策略构成可扩展配置面，将网络带宽、CPU 计算与表现质量纳入可度量的权衡框架，适用于对实时性与规模化复制同时有要求的客户端项目。