#### 一、`SystemState` 是什么？

**`SystemState` 是 `ISystem` 的“系统上下文”，也就是系统运行期的核心句柄与缓存入口。**

所有 `ISystem.OnCreate/OnUpdate/OnDestroy` 都以 `ref SystemState` 作为参数，通过它拿到 `EntityManager/World/WorldUnmanaged`、作业依赖 `Dependency`、启停开关 `Enabled`、查询/句柄/Lookup 获取与缓存、以及“应不应该执行”的判定等。

简单说：**在 `SystemBase` 里用到的一切系统级功能，在 `ISystem` 里都从 `SystemState` 进入**。

#### 二、它暴露了哪些关键属性？

**答：**

- **`EntityManager / World / WorldUnmanaged`**：这是访问当前世界与实体管理器的入口。
    
- **`Dependency` & `CompleteDependency()`**：这是系统级 Job 依赖聚合/完成点，串起在 `OnUpdate` 调度的作业。
    
- **`Enabled`**：将其置 `false` 可直接让该系统跳过更新（也会触发 `OnStopRunning` 生命周期）。
    
- **`LastSystemVersion` / `GlobalSystemVersion`**：可能变更的版本号，供“`Changed` 过滤”等机制计算是否有数据自上次系统运行后发生变化。

- **`WorldUpdateAllocator`**：用于获取与本帧更新期相同生命周期的分配器，用来构建临时查询等。
    

#### 三、它能做什么“系统级控制”？（`ShouldRun` 与 `RequireForUpdate`）

- **`RequireForUpdate<T>() / RequireForUpdate(EntityQuery)`**：声明“满足这些条件系统才执行”。可以叠加多个条件；同时也有 `RequireAnyForUpdate(...)` 支持“多选一”。引擎内部据此决定是否调用 `OnUpdate`（这里可以配合 `OnStartRunning/OnStopRunning`）。
    
- **`ShouldRunSystem()`**：内部用于判定是否应更新（由上面的 require 系列与启停状态共同决定）。

- **注意**：`RequireForUpdate` 只检查“组件是否存在”，**不会**考虑 `IEnableableComponent` 的启用位（想按启用位控制，就要在查询里用 Enabled 过滤或在逻辑里检查）。
    

#### 四、它如何拿“查询、句柄与 Lookup”？（性能关键）

在 `OnCreate` 里用 `SystemState` 获取并缓存；在 `OnUpdate` 前**要 Update 一次**，就能在 Job 中零 `GC` 地高效使用。  

**常用方法**：`GetEntityQuery(...)`、`GetComponentTypeHandle<T>(isReadOnly)`、`GetBufferTypeHandle<T>(...)`、`GetComponentLookup<T>(isReadOnly)`、`GetBufferLookup<T>(...)`。这些都是写 `IJobEntity/IJobChunk` 时传参的基础。

**对比 `SystemAPI`**：它是源码生成的语法糖，会在 `OnCreate` 自动缓存、在 `OnUpdate` 自动 `.Update(ref state)`，并在需要时做同步；也可以手动用 `SystemState` 走“显式缓存+显式 Update”的老式高掌控度写法。


**示例（显式缓存 `Lookup` 与 `Query` 的 `ISystem` 模板）：**
```
using Unity.Burst;
using Unity.Entities;
using Unity.Jobs;

// 读 PlayerTag，写 Health。Job 里用 Lookup 访问“其他实体”的数据。
[BurstCompile]
public partial struct DamageSystem : ISystem
{
    private ComponentLookup<Health> m_HealthLookup;
    private EntityQuery m_Players;

    [BurstCompile]
    public void OnCreate(ref SystemState state)
    {
        // 只在场景里真的有玩家时才跑
        state.RequireForUpdate<PlayerTag>();

        // 缓存 Lookup / Query
        m_HealthLookup = state.GetComponentLookup<Health>
				        (isReadOnly:false);
        m_Players = 
	    state.GetEntityQuery(ComponentType.ReadOnly<PlayerTag>());
    }

    [BurstCompile]
    public void OnUpdate(ref SystemState state)
    {
        // 每帧先把缓存跟世界同步
        m_HealthLookup.Update(ref state);

        var job = new ApplyDamageJob
        {
            HealthLookup = m_HealthLookup
        };

        // 这里可以对玩家集合开 Job；也可把 Query 交给 IJobChunk。
        state.Dependency = job.Schedule(state.Dependency);
    }

    [BurstCompile] public void OnDestroy(ref SystemState state) { }

    private struct ApplyDamageJob : IJob
    {
        public ComponentLookup<Health> HealthLookup;

        public void Execute()
        {
            // 省略：根据某处事件对特定 Entity 直接写 
            HealthLookup[entity]...
        }
    }
}
```

上面这套“`**OnCreate` 缓存 → `OnUpdate` 更新缓存 → `Job` 使用**”与 `SystemAPI` 展开的代码是一致的（`SystemAPI` 文档有[完整对照示例](https://docs.unity3d.com/Packages/com.unity.entities%401.0/manual/systems-systemapi.html)）。

#### 五、三类典型用法

1. **系统启停与场景/状态驱动**
    
    - 要在登录/主菜单不跑战斗逻辑：`state.Enabled = false`；进入关卡或 `RequireForUpdate<InGameTag>()` 成立后再跑，配合 `ISystemStartStop` 的 `OnStartRunning/OnStopRunning` 做资源（如特效池/声音混音组）启停。

2. **查询与跨实体访问（插值、摄像机跟随、血条同步）**
    
    - 用 `GetComponentLookup<T>` 在遍历 A 实体时读取 B 实体的数据（例如父子层级 `LocalToWorld`，或玩家实体 → `UI` 实体）。`ComponentLookup<T>` 正是为“跨集合查另一个实体的数据”而设计。

3. **作业依赖与卡顿治理**
    
    - 在 `OnUpdate` 用 `state.Dependency = job.ScheduleParallel(..., state.Dependency)` 串起依赖；必要时 `state.CompleteDependency()` 做分段同步，避免大量同时的 `EntityManager` 同步导致主线程抖动。

    - [`SystemAPI` 文档](https://docs.unity3d.com/Packages/com.unity.entities%401.0/api/Unity.Entities.SystemState.html)也强调直接 `EntityManager.GetComponentData` 会强制同步，而 `SystemAPI`/Lookup 路径可避免不必要的同步。

#### 六、和 `SystemAPI` 的职责分工

`SystemState` 是**底座**（底层句柄与手动缓存/更新），`SystemAPI` 是**语法糖 + 自动缓存/更新 + 智能同步策略**。

要极致掌控与最小同步开销，走 `SystemState` 显式缓存；要更快上手与更少样板，就用 `SystemAPI`。

两者可混用，但**在 Job 参数与热路径**，`SystemState` 的显式缓存、手动 `.Update(ref state)` 更可控。

#### 七、生命周期与顺序相关的小细节

- `OnCreate` → `OnStartRunning` → `OnUpdate`（满足 `RequireForUpdate`/`Enabled` 才会跑）→ `OnStopRunning` → `OnDestroy`，上层由系统组的 `OnUpdate` 触发。
    
- 定位“为什么本帧没跑”：看 `RequireForUpdate` 是否满足、系统是否被 `Enabled=false`、以及 `ShouldRunSystem()` 的判定；这三者共同决定是否调用 `OnUpdate`。
    

---

#### 八、总结：

> `SystemState` = `ISystem` 的世界句柄 + 依赖汇总器 + 查询/`Lookup/TypeHandle` 缓存器 + 运行开关。
> 
> `OnCreate` 缓存（`Query/Handle/Lookup`），`OnUpdate` 先 `Update` 缓存再调度 `Job`，用 `Dependency` 串依赖；
> 
> 用 `RequireForUpdate`/`RequireAnyForUpdate`/`Enabled` 决定系统是否跑；需要时 `CompleteDependency` 做分段同步。
> 
> `SystemAPI` 是糖衣，能自动缓存并智能同步，但热路径上显式用 `SystemState` 更可控。