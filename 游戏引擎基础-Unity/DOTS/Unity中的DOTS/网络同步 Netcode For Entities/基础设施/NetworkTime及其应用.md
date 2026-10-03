#### 一、核心概念：一套为多人同步服务的“多时间轴”视图

**NetworkTime** 是存在于客户端与服务器世界里的单例组件，提供当前帧网络仿真的关键时间量：预测用的 `ServerTick`、插值用的 `InterpolationTick` 及其小数进度、以及若干预测循环标志位与批处理参数。

它是客户端/服务器共享的“时间真相”，用于驱动**预测、回滚与插值逻辑**，而不是直接依赖 Unity 普通的 `Time.deltaTime`【在服务器与客户端世界中均存在、承载仿真时序特征】。

- **`ServerTick`**：**预测用**的“当前服务器刻度”。服务器端它严格单调且总是整 `Tick`；客户端端它表示“本帧预测服务器应当运行的刻度”，可能是整 `Tick` 或分数 `Tick`，必要时会为回滚而短暂倒退或跳变（随后在预测循环结束时复位）。
    
- **`InterpolationTick` / `InterpolationTickFraction`**：**呈现用**的“插值目标刻度”与到达它的**进度**（0,1]。例如目标是 11、分数 0.5，含义是**正从 10 插向 11 的一半路程**，并非 11.5 这一“半 Tick”时间点。
    
- **`ServerTickFraction`**：客户端可变步长下才有意义；服务器上永远是 1.0（整 Tick）。
    
- **预测循环标志**：如 `IsInPredictionLoop`、`IsFirstTimeFullyPredictingTick`、`IsFinalPredictionTick` 等，用于判断“当前是否在预测”“本 Tick 是否首次完整预测”“是否为循环收尾”等场景。
    
- **`SimulationStepBatchSize`**：一次更新**合并 N 个 Tick**以降低 CPU 成本的批处理系数；在预测循环里的**分数 Tick**阶段该值为 1（逐小步推进）。
    

#### 二、为什么客户端必须围绕 NetworkTime 编程

1. **预测=手感与确定性的桥**：客户端本地用 `PredictedSimulationSystemGroup` 以 **`ServerTick`** 为轴回放/重算仿真，从“最老快照 `Tick`”滚动到“目标预测 `Tick`”，过程中 NetworkTime 会把 `ServerTick` 设成“当前被预测的 Tick”，让同一套逻辑在服端/端侧都能跑通（服务器端即权威仿真，客户端 是预测）。
    
2. **插值=视觉的一致性**：大多数非本地控制物体按 `InterpolationTick` 与 `InterpolationTickFraction` 进行**时间落后**的平滑插值，避免远端对象抖动；这一对字段定义了“目标”与“进度”，是 UI/HUD/动画插值的可靠依据。
    
3. **同步的“唯一时间源”**：`NetCode` 明确要求**用 `NetworkTime` 取时间量**（而非 `Time` 系列），例如 `var t = SystemAPI.GetSingleton<NetworkTime>()`；这样无论在预测循环内外，代码都对齐到同一根网络时间轴上。
    

#### 三、与 `TickRate` 配置的关系：频率、包速率与成本

`ClientServerTickRate` 决定**仿真频率**（`SimulationTickRate`）与**快照发送频率**（`NetworkTickRate`）等参数。

服务器可以**低于仿真频率发包**以省带宽/CPU，但客户端观感会接近“更高延迟”；还能设置每帧最多补跑多少 Tick 以及是否对“补跑 Tick”都发送快照等策略——这些都会直接影响 `NetworkTime` 的取值与客户端的预测/插值压力。

#### 四、高频用法

**A. 预测侧（Owner/Predicted Ghost）**

- 在 `PredictedSimulationSystemGroup` 内，读取 `var tick = SystemAPI.GetSingleton<NetworkTime>().ServerTick;`，以 **`ServerTick`** 为“当前仿真时刻”推进控制器、技能冷却、状态机等，并搭配 `Simulate` 过滤仅处理需在该 `Tick` 预测的实体，避免在回滚-重放期间“多算”。
    
- 若需要“仅在首次完整预测该 `Tick` 时做一次性逻辑”（如缓存构建、一次性事件触发），利用 `IsFirstTimeFullyPredictingTick` 做门闩，避免在分数 `Tick` 阶段重复执行。
    
- 物理/运动积分可结合 `ServerTickFraction`：变步长预测时按分数 delta 推进，确保“半步”“四分之一步”都严格落在网络时序上（服务器永远整 Tick）。
    

**B. 插值侧（Interpolated Ghost/表现层）**

- 对远端物体：以 `InterpolationTick` 为目标、`InterpolationTickFraction` 为进度做 `Transform`/动画插值。“分数是**通向目标 `Tick` 的进度**”，勿将其误当“半 `Tick` 时间点”做物理采样，这会引入时间语义错误。
    
- HUD/轨迹渲染可直接读取 `InterpolationTickFraction` 做 UI 条/进度光标，天然与网络缓冲对齐，无须另造时钟。
    

**C. 帧组织与性能**

- 当客户端渲染帧率与仿真 `Tick` 不一致时，`NetworkTime` 会出现**分数 `Tick`** 与**批量步进**两种现象：
    - 预测循环内：常见分数 `Tick`，`SimulationStepBatchSize = 1`，细粒度推进以保证手感一致；
        
    - 循环外：可能通过 `SimulationStepBatchSize > 1` 合并多 Tick，减少脚本开销（前提是逻辑能容忍合并步长带来的精度损失）。
        
- 服务器/客户端的 `TickRate` 通过 `ClientServerTickRate` 协同配置：`SimulationTickRate` 是**权威仿真与预测**的时间基，`NetworkTickRate` 是**快照出帧率**，差距越大，客户端插值/回滚负担越重。
    

#### 五、与输入/命令、延迟补偿的协同

- **命令时序**：输入命令的采集与消费都要按 `ServerTick` 对齐（例如用 `GetDataAtTick(tick, out cmd)`）。预测循环会将 `ServerTick` 设置为当前正在预测的刻度，从而保证“那一刻”的输入被重复回放与校正。
    
- **延迟观测与补偿**：可结合 `CommandDataInterpolationDelay`（每个命令目标实体都会更新该延迟量），把客户端与服务器在“命令到达点”的时差反馈到命中判定、回溯查询等模块中，避免“看得中打不中”的错觉。
    

#### 六、常见误区与纠偏

- **把普通 `Time` 当网络时间**：在预测系统里用 `Time.deltaTime` 或直接读输入设备，会偏离 `NetworkTime` 的 `Tick` 轴，造成回滚风暴与不可复现的“手感抖动”。统一用 `NetworkTime` 与按 Tick 的命令/状态读取。
    
- **误解分数语义**：`InterpolationTickFraction` 与 `ServerTickFraction` 都是“到目标 Tick 的**进度**”，不是“半 Tick 的绝对时间点”；以此做采样与插值才不会产生视觉/物理上的相位误差。
    
- **忽视 TickRate 对性能/带宽的牵引**：一味拉高 `SimulationTickRate` 或 `NetworkTickRate` 会线性放大 CPU 与带宽；利用 `ClientServerTickRate` 做“高仿真、低快照”的权衡时，要预估客户端的插值缓存与回滚成本。
    
- **中途读取不稳定的 ServerTick**：预测循环中 `ServerTick` 可能为回滚而瞬时回退；在系统开头缓存一次并贯穿使用，或依据 `IsInPredictionLoop`/`IsFirstTimeFullyPredictingTick` 等旗标来组织流程。
    

#### 七、客户端实践
```
// 预测系统：只在需要预测的实体上运行（Simulate）
[UpdateInGroup(typeof(Unity.NetCode.PredictedSimulationSystemGroup))]
public partial struct PlayerPredictSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        var nt = SystemAPI.GetSingleton<NetworkTime>();      // 统一时间
        var tick = nt.ServerTick;                            // 当前预测刻度

        foreach (var (lt, inputBuf) in
                 SystemAPI.Query<RefRW<LocalTransform>, DynamicBuffer<MoveCommand>>()
                          .WithAll<Simulate>())
        {
            if (!inputBuf.GetDataAtTick(tick, out var cmd))  // 与 Tick 对齐的命令
                return;

            var dt = SystemAPI.Time.DeltaTime * nt.ServerTickFraction; // 分数Tick
            // ……移动/跳跃等预测逻辑
        }
    }
}

// 插值系统：面向远端对象的表现层
[UpdateInGroup(typeof(PresentationSystemGroup))]
public partial struct InterpVisualSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        var nt = SystemAPI.GetSingleton<NetworkTime>();
        foreach (var (interp, xform) in
                 SystemAPI.Query<InterpolatedState, RefRW<LocalTransform>>())
        {
            xform.ValueRW.Position = math.lerp(
                interp.PosAtPrevTick, interp.PosAtTargetTick, nt.InterpolationTickFraction);
        }
    }
}

```


#### 八、**总结**

**NetworkTime** 把“权威仿真”“客户端预测”“插值呈现”这几条时间轴压进同一个可查询的单例里。

客户端围绕它写系统，就能让**输入→预测→回滚→插值**在统一 Tick 语义下顺滑运转；再配合 `ClientServerTickRate` 做频率/带宽/CPU 的三角平衡，既保证“手感”，也保证确定性与一致性