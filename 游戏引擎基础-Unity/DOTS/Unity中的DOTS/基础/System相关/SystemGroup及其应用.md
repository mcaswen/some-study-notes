#### 一、`SystemGroup` 是什么？

`SystemGroup` 是“系统的系统”。它把多个 `System`（或子 `Group`）组织成一个有序的树，每帧在主线程按排序规则依次更新，从而控制整帧的执行阶段与先后关系。默认世界会自动创建系统与系统组的层级。

#### 二、默认有哪些组？各自负责哪一段“帧生命周期”？

默认世界有三大根组，分别对应 `Unity PlayerLoop` 的三个阶段末尾：

- **`InitializationSystemGroup`**：在 `Initialization` 阶段末尾更新，放“本帧准备/一次性设置”之类的`System`。

- **`SimulationSystemGroup`**：在 `Update` 阶段末尾更新，是**默认归属**，负责把“上一帧的游戏世界”推进到“下一帧”。
    
- **`PresentationSystemGroup`**：在 `PreLateUpdate` 阶段末尾更新，放“渲染/表现相关”的系统。
    

#### 三、两类常用“速率组”

面向客户端“时间管理”的两大工具，都是 `SimulationSystemGroup` 的子组（常见做法）：

- **`FixedStepSimulationSystemGroup`**：固定步长更新，默认步长 1/60 s；该组运行时会**临时重写** `Time.DeltaTime/ElapsedTime`，并在需要时**一帧多次**补跑以追上真实时间；可在运行时改 `Timestep`。适合物理、定时器、输入采样等。
    
- **`VariableRateSimulationSystemGroup`**：可变低频率组，默认约 15 `fps`（66`ms`）；可把“非关键、容忍延迟”的逻辑放进去（如低频 AI、后台采样）。

#### 四、与结构变更相关的 `ECB` 组（Begin/End）

每个阶段都有与之配套的 **`EntityCommandBufferSystem`** ，用于**汇总并在阶段边界回放**结构变更（建/销实体、加/删组件）：

- 例如 `BeginSimulationEntityCommandBufferSystem` 与 `EndSimulationEntityCommandBufferSystem` 分别在 `Simulation` 的开头和末尾回放；用法是取其 **Singleton** 并创建 `ECB`
    
- **没有** `EndPresentationEntityCommandBufferSystem`，因为渲染数据已经提交，不能再做结构变更；这类改动可放到下一帧的 `BeginInitialization…`。

#### 五、如何把系统放进正确的组并排好顺序？

- 用 `[UpdateInGroup(typeof(某组))]` 指定组；若不指定，系统默认进 `Simulation`。可自定义组：派生 `ComponentSystemGroup` 即可。
    
- 组内排序：`[UpdateBefore] / [UpdateAfter]` 只对**同一父组的直接子项**生效；`OrderFirst/OrderLast` 优先级最高。
    
- 系统创建/销毁顺序与 `CreateBefore/CreateAfter` 相关；复杂工程建议按官方推荐用默认世界创建或自定义引导，避免手动打乱顺序。
    

#### 六、多个 World 的场景（和 `NetCode` 的关系）

可以创建多世界，并让同一系统在不同世界、不同速率下更新；`Netcode for Entities` 就利用多世界把“客户端世界/服务器世界”分离运行。

客户端常见：只有客户端世界包含 Presentation 组，服务器世界则没有。


#### 七、游戏客户端开发最佳实践

- **输入采集与指令打包**：放 **`Initialization`**（非常早的输入采样）或 **`Simulation`** 开头；若走 `NetCode`，可用其输入组（例如 `GhostInputSystemGroup`）以降低采样到发送的延迟。
    
- **本地移动/数值逻辑/客户端预测**：放 **`Simulation`**（默认），必要时用 `UpdateBefore/After` 与物理、网络预测/插值组（若使用 `NetCode` 的专用组）排顺序。
    
- **物理/定时器/固定时间逻辑**：放 **`FixedStepSimulationSystemGroup`**，利用固定步长与一帧多步特性保证稳定。
    
- **低频后台活（远端 AI `LOD`、冷热数据刷新）**：放 **`VariableRateSimulationSystemGroup`**，削峰。
    
- **渲染桥接（将 `ECS` 数据推到渲染/特效/音频）、摄像机与 `UI` 同步**：放 **Presentation**，确保所有模拟已经完成再做表现。
    
- **结构变更（生成/销毁/切换状态）**：在系统内记录到恰当阶段的 **`ECB`**，例如需要“本帧模拟结束后再生效”的变更，就写到 `EndSimulationEntityCommandBufferSystem`。
    
- **可视化排查**：用 **Systems** 窗口查看整棵组树与最终排序（Editor 菜单 Window > Entities > Systems）。
    

#### 八、总结

> **定义**：`SystemGroup` 把系统按阶段/顺序编组，每帧主线程按序更新。默认三根组：Init/Sim/Presentation，分别在 `Initialization`/`Update`/`PreLateUpdate` 末尾跑；不指定组就进 Simulation。排序靠 `UpdateInGroup + UpdateBefore/After`（同组内），`OrderFirst/Last` 优先。
> 
> **速率组**：`FixedStep`（默认 1/60 s，可多步并临时重写 `Time`），`VariableRate`（默认约 15`fps`）。
> 
> **`ECB`**：各阶段 Begin/End 有对应回放点；渲染后不可做结构变更，需移到下一帧 `BeginInitialization`。多世界常用于 `NetCode` 的“客户端/服务器”分离。