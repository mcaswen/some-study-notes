#### 1）`ECB` 是什么？能解决什么问题

`ECB` 是一个**线程安全的“变更录音机”**：先把“创建/销毁实体、增删组件、改 Buffer、启用/禁用”等**结构性改动**先记录下来，等到主线程合适的时机**顺序回放**，从而既能在 Job 里发起改动，又**避免由于依赖关系处理不当导致某些`System`访问或依赖已经被清除的实体/组件**。

它的 API 基本与 `EntityManager` 一一对应（`CreateEntity/DestroyEntity/AddComponent/SetComponent/RemoveComponent…`），但真正生效在回放时；还支持“临时实体”在同一 `ECB` 内互相引用，回放时引擎自动做实体重映射。

#### 2）为什么客户端开发强烈建议用 `ECB`？

客户端一帧里经常既要并行做模拟，又要**批量**生成/回收单位、切换状态、更新命中队列。直接改结构会制造硬同步；

把这些动作**攒到 `ECB`**，由系统在固定点回放，能减少同步点、提升吞吐，还能复播一段“生成序列”（`PlaybackPolicy.MultiPlayback`）做一键刷怪。

回放只能在**主线程**，这正好把“结构变更”收敛在少数位置

#### 3）怎么“自动回放”？Begin vs End 选谁？

不要自己 `Playback/Dispose`（除非确有需要）。常规是在系统里拿到**对应组的 `ECB System` 的 Singleton**，创建 `ECB`；该系统会在**它更新的时机**自动：完成已注册 Job → 回放所有 `ECB` → 释放。默认世界内置这些回放点（无须自己创建）：

- `Begin/EndInitializationEntityCommandBufferSystem`
    
- `Begin/EndFixedStepSimulationEntityCommandBufferSystem`
    
- `Begin/EndVariableRateSimulationEntityCommandBufferSystem`
    
- `Begin/EndSimulationEntityCommandBufferSystem`
    
- `BeginPresentationEntityCommandBufferSystem`（注意：**没有** `EndPresentation`；渲染提交后不能再做结构变更，要放到下一帧的 `BeginInitialization`）  

- 它们分别在对应 **`SystemGroup` 的开头/结尾**执行；因此**需要“本组末尾再生效”就用 `EndXXX`，要“本组一开始就生效/下一帧最早”就用 `BeginXXX`**。
    
> **获取方式（1.0+ 正解）：**`SystemAPI.GetSingleton<EndSimulationEntityCommandBufferSystem.Singleton>().CreateCommandBuffer(state.WorldUnmanaged)`。
#### 4）并行写入与“可复现顺序”

多线程 Job 里用 `ecb.AsParallelWriter()` 并在 API 的**第一个参数传 `sortKey`**，通常用 `[ChunkIndexInQuery]`；

回放前会按 `sortKey` 排序，从而获得**确定顺序**（否则录制顺序随调度而非确定）。

#### 5）和 `DynamicBuffer` 的“专用操作”

**缓冲有单独的 `ECB` API：**

- `SetBuffer<T>`：拿到 **`ECB` 内部**的 `DynamicBuffer<T>` 填数据；回放时覆盖目标缓冲（若目标不存在会抛错）。
    
- `AddBuffer<T>`：若无则添加再得到缓冲；
    
- `AppendToBuffer<T>` / `ParallelWriter.AppendToBuffer<T>(sortKey, …)`：在**并行**下安全地**追加元素**；**要求目标已存在**，否则回放报错。
    
- `AddComponent<T>` / `RemoveComponent<T>` 也可以把缓冲类型 T 当组件来加/删。  
    官方建议：多线程追加前，**先确保目标已有缓冲**（例如先 `AddComponent<T>`）。
    

#### 6）启用位（`Enableable`）与 `ECB`

`ECB` 也支持 `SetComponentEnabled<T>(entity, bool)`；

启用/禁用**不是结构变更**，查询把禁用视作“仿佛没有该组件”。这让“眩晕/无敌/冷却中”等开关可以**不经 `ECB`、在工作线程直接改**；

若想**延后**到某回放点再切，可把“启用位切换”也录到 `ECB`。

#### 7）临时实体与多次回放

在同一 `ECB` 中 `CreateEntity/Instantiate` 得到的实体是**占位符**（负索引），**只对该 `ECB` 的后续命令有效**；

回放时会重映射成真实实体，无法在回放后再通过这个占位符去调用 `EntityManager`。需要**重复播放**同一批命令时，构造 `ECB` 时用 `PlaybackPolicy.MultiPlayback`。

#### 8）实践范式

**A. `ISystem` + `EndSimulation`：并行收集 → 末尾生效**
```
using Unity.Burst;
using Unity.Entities;

public struct DeadTag : IComponentData {}
public struct SpawnRequest : IComponentData { public Entity Prefab; }

[BurstCompile]
public partial struct CleanupAndSpawnSystem : ISystem
{
    [BurstCompile]
    public void OnUpdate(ref SystemState state)
    {
        // 1) 拿 EndSimulation 的 ECB（本组末尾回放）
        var ecbSingleton = SystemAPI.GetSingleton<EndSimulationEntityCommandBufferSystem.Singleton>();
        var ecb = ecbSingleton.CreateCommandBuffer(state.WorldUnmanaged);

        // 2) 并行录制（确定顺序用 [ChunkIndexInQuery]）
        new CollectJob { Ecb = ecb.AsParallelWriter() }.ScheduleParallel();

        // 无需手动 Playback/Dispose；ECB System 会在该组末尾完成 Job、回放并释放
    }

    [BurstCompile]
    partial struct CollectJob : IJobEntity
    {
        public EntityCommandBuffer.ParallelWriter Ecb;

        void Execute(Entity e, in DeadTag dead, [ChunkIndexInQuery] int sortKey)
        {
            Ecb.DestroyEntity(sortKey, e);
            // 示例：也可追加到缓冲（需目标已有该缓冲）
            // Ecb.AppendToBuffer(sortKey, victim, new Hit{ Damage = 10 });
        }
    }
}
```

（获取 `Singleton` 创建 `ECB`、并行写入 + `sortKey`、自动回放/释放的流程均见[Unity 文档](https://docs.unity3d.com/Packages/com.unity.entities%401.0/manual/systems-entity-command-buffer-automatic-playback.html)。）

**B. 向 `DynamicBuffer` 追加（并行安全）**

```
public struct Hit : IBufferElementData { public float Damage; }

[BurstCompile]
partial struct RecordHitsJob : IJobEntity
{
    public EntityCommandBuffer.ParallelWriter Ecb;
    void Execute(Entity victim, [ChunkIndexInQuery] int sortKey)
    {
        // 建议：此前某系统确保 victim 已有 Hit 缓冲（若不确定，可先 AddComponent<Hit>）
        Ecb.AppendToBuffer(sortKey, victim, new Hit { Damage = 12f });
    }
}
```

（`AppendToBuffer<T>(sortKey, entity, element)` 要求目标已存在缓冲；否则回放时报错。`sortKey` 建议用 `[ChunkIndexInQuery]`。）

**C. 需要“下一帧一开场就生效”的变更**

```
var beginInit = SystemAPI.GetSingleton<BeginInitializationEntityCommandBufferSystem.Singleton>();

var ecb = beginInit.CreateCommandBuffer(state.WorldUnmanaged);
// 录制...
```

（Begin/End 的更新时机与“没有 `EndPresentation`”规则。）

#### 9）常见误区与纠偏

- **Playback 只能主线程**；自己 new 出来的 `ECB` 必须自己 `Playback + Dispose`，但**经 `ECB` System 创建**的不要手动回放/释放。
    
- **并行确定性**：并行录制本质非确定；要确定回放顺序，就传 `sortKey`（通常是 `ChunkIndexInQuery`）。
    
- **Buffer 细节**：`SetBuffer` 覆盖内容；`AppendToBuffer` 仅在目标已有缓冲时有效；建议并行追加前先 `AddComponent<T>` 兜底。
    
- **临时实体作用域**：只在**同一 `ECB`** 内有效；回放后不要拿占位符去调 `EntityManager`。
    
- **一活儿一条 `ECB`**：不同并行 Job 最好用**不同 `ECB`**，避免 `sortKey` 交错导致难以读懂的合并顺序。
    

#### 10）面试速答

> `**ECB` = 线程安全的结构变更日志**。Job 里记录，**在指定组的 Begin/End** 回放（默认有八个回放点；没有 `EndPresentation`）；
> 
> 回放与释放可交给 **`ECB System` + `Singleton`**。并行下用 `AsParallelWriter`，用 `[ChunkIndexInQuery]` 当 **`sortKey`** 获得**确定回放顺序**。
> 
> 对 **`DynamicBuffer`**：`Add/Set/AppendToBuffer` 各司其职；启用位可 `SetComponentEnabled` 且**不构成结构变更**。
> 
> 需要“重复播放”用 `PlaybackPolicy.MultiPlayback`。