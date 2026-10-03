#### 一、`Job`、`JobHandle`、`Dependency` 分别是什么

- **Job**：丢给 C# Job System 的可并行任务单元。系统在主线程“排班”，Job 在工作线程“干活”。
    
- **`JobHandle`**：排班后返回的一张“票据/栅栏”，代表这份 Job 的完成状态；它既是**依赖的输入**，也是**结果的输出**。把一个 Job 的 `JobHandle` 传给另一个 Job 的 `Schedule`，就建立了**先后依赖**（后者要等前者）。
    
- **`SystemState.Dependency`（简称 `Dependency`）**：每个系统自带的一条“总依赖线”。**在进入 `OnUpdate` 之前**，它已经合并了“所有会与本系统读/写组件发生冲突的前序作业”的句柄；在 `OnUpdate` 里继续排作业时，系统会根据**读/写的组件集合**自动**更新**这条依赖线。

> **要点**：Dependency 不是“随便的一个句柄”，而是**该系统的 `ECS` 相关依赖的总和**，由引擎根据**读/写了哪些组件**推导而成。

#### 二、系统如何“自动”管理 Job 依赖？

- **自动部分**：Entities 会按“**哪个系统读/写了哪些组件**”在系统之间建立**作业依赖**。比如甲系统读 `Velocity`，乙系统写 `Velocity`，那乙系统的作业会**依赖**甲系统的相关作业。
    
- **最佳实践**：在系统里每次 `.Schedule()` / `.ScheduleParallel()` 之后，把返回值**赋回** `state.Dependency`（或 `this.Dependency` in `SystemBase`），这样后续系统才能感知到你新排的作业。必要时用 `CompleteDependency()` 把当前系统的未完成作业**切到同步点**（例如你马上要在主线程访问这些数据）。

#### 三、手工“捆绑”与合并策略

- 同一帧里可能得到**多张票据**（多个 `JobHandle`）。把它们用 **`JobHandle.CombineDependencies`** 合成一张，然后把这张**合并结果**作为后续 Job 的输入依赖，或赋给系统的 `Dependency`。适合“扇出→扇入”的图状流水。
    
-  **Combine = 合并等待条件**，并不是“让 A 依赖 B 再依赖 C 的链条”。链式先后仍靠逐个 `Schedule(前一个句柄)` 来表达。

#### 四、为什么不能直接用 `EntityManager` 访问组件？

在 **主线程**用 `EntityManager.GetComponentData` 等 API 读取，常常会**强制完成相关写作业**，形成意外同步点；

更推荐把“跨实体随机访问”的需求改用 **`ComponentLookup<T>` / `BufferLookup<T>`**，它们是为 Job 设计的查表容器，可作为字段传入 Job，不会制造额外同步。

这些查表入口放在 **`SystemAPI.GetComponentLookup/ GetBufferLookup`**。

#### 五、结构变更的依赖：并行录制 + 回放点

结构变更（建/销实体、加/删组件、写 Buffer）**不能**直接在 Job 里做，正确方式是把意图记录进 **`EntityCommandBuffer`**，并在**组的固定回放点**（Begin/End _X_）由主线程**顺序回放**。

并行录制用 `ParallelWriter`，**第一个参数传 `sortKey`（常用 `[ChunkIndexInQuery]`）**，回放前会按 `sortKey` 排序，从而得到**确定的回放顺序**。

#### 六、`IJobEntity` 与“依赖注入”的常见写法

`IJobEntity` 是最常用的 `ECS` Job 壳子：只用写 `Execute(...)`，编译器会把它生成为 `IJobChunk`。它可以直接用 `in/ref IComponentData`、`DynamicBuffer<T>`、`Aspect` 等参数，支持 `WithChangeFilter` 等过滤特性；排完后把 `JobHandle` 回写系统的 `Dependency` 即可。

#### 七、高频用法

1. **数据→行为→回放**：  
    数据热路径（移动/插值/AI 评估）→ 写成 `IJobEntity`；需要生成/回收/切换状态 → 在 Job 里用 `ECB` 记录 → 选对回放点（多半 `EndSimulation`），**并把并行录制的 `sortKey` 固定**。
    
2. **依赖即“数据冲突关系”**：  
    把依赖想成“哪些组件被谁读/写”；分解系统让**读多写少**、**只读可并行**，把“写”集中在少数 Job，减少跨系统的写写/读写冲突。引擎会据此自动把必要的依赖并起来，只需**回写 `Dependency`**。
    
3. **同步点要有意识地打**：  
    只有当真的要在主线程操作（例如 `UI` 桥接、物理世界只读快照）时，才用 `CompleteDependency()`；否则让作业链条自然延伸到下游系统。

#### 八、客户端实践
##### 代码范式 A：两段并行作业 + 明确依赖链（移动 → 阻尼）
```
using Unity.Burst;
using Unity.Entities;
using Unity.Mathematics;
using Unity.Transforms;

public struct Velocity : IComponentData { public float3 Value; }
public struct MoveSpeed : IComponentData { public float3 Value; }

[BurstCompile]
partial struct IntegrateJob : IJobEntity
{
    public float Dt;
    void Execute(ref LocalTransform tf, in Velocity v)
        => tf.Position += v.Value * Dt;
}

[BurstCompile]
partial struct DampenJob : IJobEntity
{
    public float Dt;
    void Execute(ref Velocity v, in MoveSpeed s)
        => v.Value = math.lerp(v.Value, s.Value, math.saturate(Dt * 8f));
}

[BurstCompile]
public partial struct MovementSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        var dt = SystemAPI.Time.DeltaTime;

        // 1) 阻尼：写 Velocity
        var hDampen = new DampenJob { Dt = dt }.ScheduleParallel(state.Dependency);

        // 2) 积分：读 Velocity、写 LocalTransform —— 依赖上一步
        var hIntegrate = new IntegrateJob { Dt = dt }.ScheduleParallel(hDampen);

        // 3) 回写系统依赖，交给下游系统感知
        state.Dependency = hIntegrate;
    }
}
```

**要点**：`ScheduleParallel` 接收“上一棒”的 `JobHandle` 并返回“这一棒”的 `JobHandle`；最后把**最新句柄**回写 `state.Dependency`。引擎会基于“谁读写了 `Velocity` / `LocalTransform`”在系统之间继续传递依赖。

##### 代码范式 B：并行记录结构变更（`ECB` + `sortKey`）+ 合并依赖
```
using Unity.Burst;
using Unity.Entities;

public struct DeadTag : IComponentData {}
public struct SpawnRequest : IComponentData { public Entity Prefab; }

[BurstCompile]
partial struct CleanupJob : IJobEntity
{
    public EntityCommandBuffer.ParallelWriter Ecb;
    void Execute(Entity e, [ChunkIndexInQuery] int sortKey, in DeadTag _)
        => Ecb.DestroyEntity(sortKey, e); // 并行录制，确定性回放靠 sortKey
}

[BurstCompile]
partial struct SpawnJob : IJobEntity
{
    public EntityCommandBuffer.ParallelWriter Ecb;
    void Execute([ChunkIndexInQuery] int sortKey, in SpawnRequest req)
        => Ecb.Instantiate(sortKey, req.Prefab);
}

[BurstCompile]
public partial struct CleanupAndSpawnSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        var endSim = SystemAPI.GetSingleton<EndSimulationEntityCommandBufferSystem.Singleton>();
        var ecb = endSim.CreateCommandBuffer(state.WorldUnmanaged);

        var h1 = new CleanupJob { Ecb = ecb.AsParallelWriter() }.ScheduleParallel(state.Dependency);
        var h2 = new SpawnJob   { Ecb = ecb.AsParallelWriter() }.ScheduleParallel(state.Dependency);

        // 扇出之后合并句柄，再回写系统依赖
        state.Dependency = JobHandle.CombineDependencies(h1, h2);
        // EndSimulation ECB System 会在本组末尾：完成依赖→排序→回放→释放
    }
}
```

这里展示了**并行录制 + `sortKey`** 与 **`JobHandle.CombineDependencies`** 的常见组合。并行录制的命令在回放前会按 `sortKey` 排序，得到**确定性**回放。


#### 九、面试易错点

- **忘记回写 `Dependency`**：作业排了但没把 `JobHandle` 写回系统，后续系统“不知道”你的写入，轻则串发警告，重则数据竞争。修正：每次 `Schedule*` 后都更新 `state.Dependency`（或把最后的合并句柄写回）。
    
- **滥用主线程直读**：`EntityManager` 直读直写容易拉出同步点；跨实体随机访问优先 `ComponentLookup/BufferLookup` 并传进 Job。
    
- **误解 `CombineDependencies`**：它是“合并等待”，不是“建立先后”。链条依赖依然靠把**前一项句柄**作为**后一项调度的输入**。
    
- **结构变更无序**：并行录制 `ECB` 不给 `sortKey` 的话，回放顺序不确定；用 `[ChunkIndexInQuery]` 或你自己的稳定键。
    
- **同步点缺乏节制**：`CompleteDependency()` 用在“必须主线程读取”的临界点，不要每帧乱打同步点。（API 说明确认为“完成本系统已注册的作业”。）
    

#### 总结

> **`JobHandle`** 是 Job 的“完成票据”；系统的 **`Dependency`** 是“该系统的总依赖”。
> 
> 在 `OnUpdate` 里，**每个排出的作业**都要把返回的 `JobHandle` **赋回** `Dependency`；需要链条就把前者句柄传给后者调度；“扇出→扇入”用 **`CombineDependencies`** 合并。
> 
> 要做结构变更，用 **`ECB`** 并行录制；为**确定性回放**传 **`sortKey`**；
> 
> 只有要在主线程读写时才 `CompleteDependency()`。这些规则让客户端的移动/AI/插值/回收等热路径既**并行**又**无卡顿同步点**。