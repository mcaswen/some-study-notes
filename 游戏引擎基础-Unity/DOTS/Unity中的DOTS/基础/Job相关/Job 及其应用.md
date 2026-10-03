#### 一、什么是 DOTS 里的“Job”？系统为何要用它

Job 可以逐实体/逐块计算丢给 C# Job System 的多线程任务单元；

Entities 会在主线程按系统顺序调度，但**能自动根据组件的读/写关系推导依赖**，把可并行的作业并发执行，串联必须顺序的作业，从而提升一帧吞吐。

官方明确建议：系统逻辑“能上 Job 就上 Job”。

#### 二、两条主流写法：`IJobEntity` vs `IJobChunk`

- **`IJobEntity`**：
	- 写一个 `partial struct … : IJobEntity`，实现 `Execute(...)`，可 `.Schedule()` / `.ScheduleParallel()`，能复用到多个系统；它本质会生成一个 `IJobChunk`，所以只需关心“要改哪些组件”。

	- 支持在 `Execute` 里直接拿 `ref/in IComponentData`、`DynamicBuffer<T>`、`IAspect`，还能用属性标注 `WithAll/WithAny/WithNone/WithChangeFilter/WithOptions`、`[ChunkIndexInQuery]` 等。一般业务首选它。
    
- **`IJobChunk`**：按 **Chunk** 粒度拿句柄、自己处理启用位/掩码等底层细节。适合需要“按块管控/极致自定义遍历”的场景或做更底层优化。
    

#### 三、Job 的依赖与系统的 `Dependency`

每个系统都有一个 `Dependency : JobHandle`。在 `OnUpdate` 开头，它表示“本系统对之前已排作业的**自动依赖**”；

当继续在系统里 `.Schedule(… , state.Dependency)` 并把返回值**写回** `state.Dependency`，后续系统才能正确看到这批作业的**写依赖**。

必要时可 `state.CompleteDependency()` 在主线程人为切分同步点。

#### 四、与 **`SystemAPI`** 的正确搭配（避免“主线程卡刀”）

把 `ComponentLookup<T>` / `BufferLookup<T>` 这类**查表**通过 `SystemAPI.GetComponentLookup / GetBufferLookup` 拿出来传进 Job —— 这些操作**不会触发同步**，源码生成会在 `OnCreate` 缓存、在 `OnUpdate` 自动 `.Update(ref state)`。

相反，直接走 `EntityManager.GetComponentData` 之类会强制同步，热路径要避免。

#### 五、结构变更要借助 **`ECB`**，并行下用 `sortKey` 保留顺序

Job 中要“加/删组件、建/销实体/缓冲追加”，就要记录到 **`EntityCommandBuffer`**（或它的 `ParallelWriter`）。并行写入时传入 `int sortKey`（通常用 `[ChunkIndexInQuery]`），Unity 会在**回放前按 `sortKey` 排序**，让回放顺序可复现（非确定的录制 → 确定的回放）。

#### 六、Burst 编译

在 Job/系统类型上加 `[BurstCompile]` 可获得 `SIMD`/向量化等优化；官方文档也说明可通过属性参数微调编译策略。

#### 七、客户端实践
##### A：`IJobEntity` 并行移动（含依赖回写）

```
using Unity.Burst;
using Unity.Entities;
using Unity.Mathematics;
using Unity.Transforms;

public struct MoveSpeed : IComponentData { public float3 Value; }

[BurstCompile]
partial struct MoveJob : IJobEntity
{
    public float DeltaTime;
    void Execute(ref LocalTransform tf, in MoveSpeed sp)
    {
        tf.Position += sp.Value * DeltaTime;
    }
}

[BurstCompile]
public partial struct MoveSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        var job = new MoveJob { DeltaTime = SystemAPI.Time.DeltaTime };
        state.Dependency = job.ScheduleParallel(state.Dependency); // 关键：回写依赖
    }
}

```

（`IJobEntity` 的写法与 `.ScheduleParallel`；依赖通过 `state.Dependency` 串联。）

##### 代码范式 B：Job 中安全做结构改动（`ECB` + `sortKey`）

```
using Unity.Burst;
using Unity.Entities;

public struct DeadTag : IComponentData {}

[BurstCompile]
partial struct CleanupJob : IJobEntity
{
    public EntityCommandBuffer.ParallelWriter Ecb;
    void Execute(Entity e, [ChunkIndexInQuery] int sortKey, in DeadTag _)
    {
        Ecb.DestroyEntity(sortKey, e);
    }
}

[BurstCompile]
public partial struct CleanupSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        var endSim = 
        SystemAPI.GetSingleton<EndSimulationEntityCommandBufferSystem.Singleton>();
        
        var ecb = endSim.CreateCommandBuffer(state.WorldUnmanaged);
        
        state.Dependency = new CleanupJob { Ecb = ecb.AsParallelWriter() }
                           .ScheduleParallel(state.Dependency);
        // 不必手动回放，ECB System 会在 EndSimulation 自动完成依赖并回放
    }
}

```
（并行录制时传 `[ChunkIndexInQuery]` 做 `sortKey`，回放前会按键排序保证确定性。）

---

#### 八、客户端高频落地

- **热路径**：把“移动/动画采样/插值/投射体更新/`LOD` 计算”等写成 `IJobEntity` 并行跑；用 `state.Dependency` 串联上下游。
    
- **跨实体访问**：给 Job 传 `ComponentLookup<T> / BufferLookup<T>` 做随机访问，不触发同步；需要结构变更时走 `ECB`。
    
- **只处理“变更的”**：在 `IJobEntity` 上用 `[WithChangeFilter(typeof(T))]` 或把 `WithChangeFilter<T>` 加到参数上，减少无效工作。
    
- **确定回放顺序**：并行追加/移除组件时用 `ParallelWriter + [ChunkIndexInQuery]` 当 `sortKey`。
    

#### 九、面试易错点

- **忘记回写依赖**：自己 `new` 的 Job 如果不把返回的 `JobHandle` 赋回 `state.Dependency`，后续系统看不到写入依赖，可能报并发安全错误。
    
- **主线程同步点**：`EntityManager.GetComponentData` 会强制完成相关写作业；优先用 `SystemAPI.GetComponentLookup/BufferLookup` 把数据喂进 Job。
    
- **并行结构改动无序**：并行录制 `ECB` 不传 `sortKey`，回放顺序就不确定；按文档用 `[ChunkIndexInQuery]`。
    

---

#### 总结

> **Job = 多线程执行 `ECS` 计算**；常用 **`IJobEntity`**（高层、好复用）和 **`IJobChunk`**（底层、可精细控制）。
> 
> **依赖**通过系统的 `Dependency` 串起来，必要时 `CompleteDependency` 切分同步点。
> 把 **`Lookup`** 从 `SystemAPI` 拿出来喂 `Job`，避免主线程同步；
> 
> 要做**结构变更**用 **`ECB`**，并行时用 `[ChunkIndexInQuery]` 当 `sortKey` 让回放确定。