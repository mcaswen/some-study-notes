#### 一、概述

**PredictedSimulationSystemGroup（PSG）** 采用“**快照为锚 + 局部回滚 + 按 Tick 重放**”的机制实现预测与校正：

在客户端，每当接收新的权威快照，系统先把快照数据应用到涉及的 **`Predicted Ghost`** 并记录这些实体被同步到的 **`AppliedTick`**；同时定位本轮中应当开始重放的**最老起点 `Tick`（`PredictionStartTick`）**。随后 PSG 以 **`Tick` 为最小步长**，将 `NetworkTime.ServerTick` 与 `ECS.TimeData` 依次设置为“正在被重放的 `Tick`”，并只重算**需要在该 Tick 参与预测**的实体；

重放结束时，客户端得到与“此刻若在服务器上同样从该 `Tick` 往前模拟”一致的本地状态，从而把误差压缩到“快照→当前”这段时间窗之内。

该流程的**关键支点**在于：可恢复的本地状态备份、以 `Tick` 为键的确定性输入/步长、以及**选择性回滚**而非全世界回滚。


#### 二、底层抽象：三块“硬约束”

**1）权威锚点＝快照状态**  

客户端接收的服务器快照被缓存并在下一帧由 `GhostUpdateSystem` 应用到相应的`Predicted Ghost`；同一批快照只回滚**收到更新的实体**，而不是整世界。这是一种**选择性回滚**策略，缩小了回放工作集与内存带宽压力。

**2）可恢复的本地基线＝预测历史备份**  

在每轮预测循环的“最后一个完整 `Tick`”之后，`GhostPredictionHistorySystem`对需要回滚的组件做**逐 `Chunk` 的内存拷贝**，把预测后的状态备份到与 `Chunk` 关联的独立内存区；

当下一次有新快照到达并要回滚时，由 `GhostUpdateSystem`从该**历史备份（`GhostPredictionHistoryState`）** 恢复，保证重放从一个**未经本轮新输入污染**的干净基线开始。

**3）统一时间轴＝按 `Tick` 固定步 + 确定性输入**  

PSG 在客户端重放时，会把**正在模拟的 `Tick`** 写入 `NetworkTime.ServerTick`，并把 `ECS.TimeData` 的步长（`delta`）设为该 Tick 的仿真步长。

依赖 `Tick` 的输入（如 `ICommandData` / `IInputComponentData`）在每个 `Tick` 被对应地取出并应用，使“状态转移函数 `f(状态t, 输入t)→状态t+1`”对齐到同一时间轴上，具备可复演性与平台间一致性。

#### 三、执行顺序：为何这套循环“必然”实现预测与回滚

1）**应用快照 → 标记“锚点”**：将最新快照写入对应的Predicted Ghost，更新每个实体的 `PredictedGhost.AppliedTick`（已对齐到权威）；同时确定**本轮最老起点 Tick（`PredictionStartTick`）**，用于定义需要回放的时间窗口。

2）**恢复基线 → 保证起跑一致**：若存在上轮完整 `Tick` 的备份，则从该备份恢复；若无备份，则以快照 `Tick` 作为起点。此举消除了“上轮本地预测产生的漂移”对新一轮重放的影响。

3）**逐 Tick 重放 → 把误差收敛到“快照之后”**：PSG 从 `PredictionStartTick` 向**目标预测 `Tick`** 推进。每步推进前，将 `ServerTick` 与 `TimeData` 设成当前 `Tick`，使所有系统在相同的 `Tick` 语义下运行；仅对 `ShouldPredict(tick)` 为真的实体启用 `Simulate` 并参与该 `Tick` 的计算，避免重放那些已经“领先到更晚快照”的实体，降低无效工作。

4）**结束态＝“如果服务器从同一 `Tick` 往前算”的近似**：由于状态起点来自权威锚点或其后首次完整 `Tick` 的干净备份，输入是逐 `Tick` 绑定且时间步长一致，重放结束时的本地结果与服务器权威轨迹在“快照→当前”区间内一致或**误差严格受限**，从而实现“预测 + 回滚”的可靠校正。

#### 四、为何选择“快照锚 + 选择性回滚 + 逐 Tick 重放”

**1）可证明的收敛性与可复演性**：以 `Tick` 为键的确定性更新 + `Tick` 对应的输入流 = 给定同一“起点状态”和“输入序列”，重放结果唯一。只要起点来自权威（快照）或其后首次完整 `Tick` 的历史备份，误差不会跨越“快照应用点”；下一轮收到更新时再重复该过程，误差持续被“裁切”在最新快照之后。


**2）成本受控的最小化原则**：只回滚收到更新的实体，而非整个世界，显著减少状态恢复和缓存抖动；这是“选择性回滚”的直接收益。

同帧多 Tick 的补算有上限（`MaxSimulationStepsPerFrame`），在算力吃紧时让仿真时间**相对变慢**而不是持续堆积，从而避免帧时尖峰。


**3）与渲染解耦，保证交互实时性**：PSG 以 `Tick` 驱动、可在单帧多次运行，不绑定屏幕刷新；在高延迟或低帧率下，仍能通过多次重放追上目标 `Tick`，维持输入手感与权威轨迹的一致。该机制被[文档与样例](https://docs.unity3d.com/Packages/com.unity.netcode%401.4/manual/prediction-n4e.html?utm_source=chatgpt.com)明确用作延迟管理的核心手段。


**4）工程上的可分治**： 预测物理迁入**预测版固定步组**，随回滚一起复演，避免“物理走帧、逻辑走 `Tick`”的相位错位；状态备份按 `Chunk` 粒度做内存拷贝，数据布局与 `ECS` 存储保持一致，减少额外结构化开销。
    

#### 五、对照备选方案的取舍理由

**“只插值、不预测”**：以远端插值替代本地预测可以规避回滚成本，但本地交互将直接暴露 RTT 级输入延迟，无法满足动作/射击类玩法的反馈要求。PSG 的预测-回滚将交互延迟压到本地一帧，并把绝对一致性交给“快照校正 + 逐 Tick 重放”。

**“整世界回滚”**：对所有实体做全局回滚与重放虽简单，但在动态场景（大量 `Ghost`）下成本与缓存失配过高。NFE 采用“**选择性回滚** + `ShouldPredict`/`Simulate` 精确筛选”以缩小重放集合，在一致性与吞吐之间取得更优平衡。

**“只做位置修正（瞬移/漂移修正），不复演”**：直接把客户端状态硬贴到权威快照会引入明显的“橡皮筋”效应，且难以保证依赖历史的子系统（物理、命中回溯、连击计时）的一致。逐 `Tick` 复演在时间轴上“把过程补齐”，副作用与累计量（例如速度积分、冷却计数）保持连贯。

**“锁步/完全确定性 P2P”**：需要强确定性与严格同步的输入分发，RTS 可行，但在服务器权威、掉包与动态带宽环境下成本过高；NFE 的“服务器权威 + 客户端预测”在现代网络条件下更具通用性，且与 Ghost 快照压缩/确认机制天然适配（如 Ack mask 与基线选择）。

#### 六、实现细部与边界

**状态基线选择**：`PredictedGhost.PredictionStartTick` 优先取“上轮完整 `Tick` 备份”的 `Tick`；若找不到延续备份，则退化为“最近一次应用的快照 `Tick`”，保证总能从一个可度量的一致点起跑。

**时间语义稳定**：PSG 在每个重放 `Tick` 前设置 `NetworkTime.ServerTick` 与 `ECS.TimeData`，确保各系统的 `deltaTime` 与 `Tick` 对齐；若不这样做，即便状态起点正确，过程也会因步长不一致而不可复演。

**成本上界与观感权衡**：在高 RTT 下，单帧重放次数可能达到两位数；[官方文档](https://docs.unity3d.com/Packages/com.unity.netcode%401.4/manual/prediction-n4e.html?utm_source=chatgpt.com)明确给出“300ms RTT ≈ 22 次重放”的量级，提示应结合批处理与上限配置进行节流。

##### 示例/代码
```
// 1) 应用快照（选择性回滚集合 + 起点 Tick）
foreach (var ghost in ReceivedSnapshot.Ghosts)
{
    ApplySnapshotTo(ghost);                    // 写入最新权威状态
    ghost.Predicted.AppliedTick = snapshotTick;
    ghost.Predicted.PredictionStartTick = ResolveStartTick(ghost); // 历史备份优先
}

// 2) 回滚恢复（从历史备份或快照 Tick）
RestorePredictionHistoryIfAvailable();         // GhostPredictionHistoryState → 实体/Chunk

// 3) 逐 Tick 重放（PSG）
for (tick = Min(PredictionStartTick[]) ; tick <= TargetTick ; ++tick)
{
    NetworkTime.ServerTick = tick;             // 统一时间轴
    TimeData.Delta = FixedStepFor(tick);       // 统一步长
    EnableSimulateForEntitiesThatShouldPredict(tick); // 选择性重放

    RunPredictedSystemsOnce();                 // 同一帧可多次
}

// 4) 循环末尾：做一次“完整 Tick”历史备份，供下轮恢复
GhostPredictionHistorySystem.BackupAfterLastFullTick();

```
该伪代码对应的职责可在文档中找到直接描述：PSG 在预测循环中设置 `ServerTick` 与 `TimeData`；快照只回滚更新到的Predicted Ghost；历史备份用于在下一轮回滚前恢复。

#### 七、结论

PSG 通过“**快照锚定 → 历史恢复 → 按 Tick 逐步重放**”把客户端状态不断收敛到服务器权威轨迹。关键点在于：快照只触达受影响实体（降低回滚集合）、历史备份保证起跑一致（可复演）、Tick 级步长与输入绑定（决定性），以及同帧多次执行但受上限约束（成本可控）。

相较于“只插值”“整世界回滚”或“硬贴权威状态”等替代方案，该机制在保证交互实时性的同时，以可证明的时间轴一致性消解误差，提供更稳健的多人同步基础。