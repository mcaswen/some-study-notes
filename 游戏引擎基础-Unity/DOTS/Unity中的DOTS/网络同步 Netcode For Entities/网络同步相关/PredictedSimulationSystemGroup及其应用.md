#### 一、概念与定位

**`PredictedSimulationSystemGroup`（PSG）** 是面向“可预测玩法逻辑”的系统组，承载对 **`Predicted Ghost`** 的确定性更新，**客户端与服务器都在固定步长下运行**；步长由 `SimulationTickRate` 决定。

服务器侧该组**每 `Tick` 恰好运行一次**；客户端侧该组承担**预测**，必要时在同一帧内多次运行，用以追上或重放到目标 `Tick`。该组是预测环路的中枢。

#### 二、时间轴与调度

PSG 按**网络 `Tick`** 而非渲染帧推进。客户端每当接收新快照，会先把快照应用到各预测实体，并找出此次应用的**最老 `Tick`** ；随后 PSG 从该最老 `Tick` **回滚并重放**直至当前目标 `Tick`。

在该循环中，PSG 会将正在重放的 `Tick` 写入 `NetworkTime.ServerTick`，并把 ECS 的 `TimeData` 设为对应步长，确保系统以“当前被预测的 `Tick`”为时间基准运行。上述流程定义了“回滚—重放”的基本原理与调度次序。

#### 三、与帧（Frame）的关系与负载控制

帧是渲染节奏，`Tick` 是仿真步进；两者频率不必一致。帧率低于仿真频率时，PSG 可能在一帧内**补跑多个 `Tick`**；补跑上限由 `ClientServerTickRate.MaxSimulationStepsPerFrame` 约束。

一旦触顶，仿真时间将**慢于真实时间**而非继续无限补算，避免尖峰卡顿。快照发送频率由 `NetworkTickRate` 控制，可低于 `SimulationTickRate` 以降低带宽与 CPU，但会增大插值滞后。

#### 三、预测与回滚的关键信号

预测循环区分**完整 `Tick`** 与**部分 `Tick`（partial）**。

`NetworkTime.IsFirstTimeFullyPredictingTick` 在“该 `Tick` 第一次完整预测”时为真，可作为**一次性副作用**（生成、销毁、音效触发等）的门闩，防止在 `partial` 与重放阶段重复执行。该属性**仅在预测循环中有效**，用于显式约束副作用的触发时机。

#### 四、物理与固定步的衔接

初始化阶段，NetCode 会把 `PhysicsSystemGroup` 及所有 `FixedStepSimulationSystemGroup` 下的系统迁入 **`PredictedFixedStepSimulationSystemGroup`**。

该组是“预测版固定步”，**不执行 partial tick**，并可配置为**高于 PSG 的固定频率**；在需要回滚/重放时，该组也会随预测循环多次运行，从而保证物理与玩法逻辑在同一套回滚框架下对齐。

#### 五、资源与成本权衡

客户端预测具备显著 CPU 成本：每次到快照都要从快照 `Tick` 回放到目标 `Tick`，即使没有状态差异也要重放。通过降低 `NetworkTickRate` ，可以减少快照到达次数从而降低重放频率，但会增加视觉延迟与插值压力。在工程实践中，取舍通常在**仿真频率、快照频率、同帧补算上限**三者之间进行。

#### 六、配置与使用边界

`ClientServerTickRate` 单例用于配置**仿真步长**（`SimulationTickRate`）、**快照发送频率**（`NetworkTickRate`）及相关上限参数；客户端可在握手阶段同步关键 `Tick` 配置，服务器侧通常需显式创建并设置。上述参数直接决定 PSG 的运行次数、是否出现同帧多 `Tick`，以及插值缓存需求。

#### 七、客户端实践

PSG 内仅放**需要端服一致**且会随回滚多次运行的确定性玩法系统；表现层与插值系统放在渲染/展示相关的组中，避免被回滚反复驱动。PSG 中读取时间应基于 `NetworkTime` 与当前预测 `Tick`，而非 `Time.deltaTime`。

输入与状态读取应与 `Tick` 对齐（例如基于当前预测 `Tick` 的命令数据），并结合 `Simulate` 条件仅处理**应在该 `Tick` 更新**的实体，减少不必要的重放开销。

**示例/代码**
```
using Unity.Burst;
using Unity.Entities;
using Unity.NetCode;
using Unity.Transforms;

// 预测侧移动：只在需要预测的实体上执行，副作用用“首次完整预测”门闩
[UpdateInGroup(typeof(PredictedSimulationSystemGroup))]
public partial struct PredictedMoveSystem : ISystem
{
    [BurstCompile]
    public void OnUpdate(ref SystemState state)
    {
        var nt   = SystemAPI.GetSingleton<NetworkTime>();
        var tick = nt.ServerTick;

        foreach (var (lt, e) in
                 SystemAPI.Query<RefRW<LocalTransform>>().WithAll<Simulate>())
        {
            // …按 tick 取命令/输入，推进确定性运动（略）

            // 一次性副作用仅在首次完整预测触发
            if (nt.IsFirstTimeFullyPredictingTick)
            {
                // 例如：写入一次性事件、音效触发、统计计数等
            }
        }
    }
}

```
该组织方式以 PSG 的预测 `Tick` 为唯一时间基准，副作用借助“首次完整预测”进行门控；渲染与插值应在表现层读取 `InterpolationTick(+Fraction)` 完成，不应混入 PSG 的确定性循环。

#### 八、结论

PSG 的职责是以固定步长在端服两端推进同一套确定性玩法逻辑，并在客户端执行“**快照→回滚→重放**”的预测环路；该环路以 `NetworkTime.ServerTick` 为锚，配合“首次完整预测”信号约束一次性副作用，同时通过 `ClientServerTickRate` 在仿真频率、快照频率与补算上限之间平衡成本与观感。

物理通过预测版固定步组接入同一回滚框架，避免相位错位；表现层与插值独立于 PSG，减少回滚对画面与音效的扰动。