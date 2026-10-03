#### 一、什么是 `EntityQuery`？

`EntityQuery` 用来在**原型-Chunk**层面选择数据：它先锁定“包含某些组件集合”的**原型（archetypes）**，再把这些原型下的**Chunk**收集起来，供系统按块/按实体迭代与调度作业，是 `ECS` 数据访问的核心入口。

#### 二、如何创建一个 Query？

- **`EntityQueryBuilder`（推荐）**：`WithAll/WithAny/WithNone` 组合组件约束，Burst 友好；适用于 `ISystem` 与 `SystemBase`。
    
- **`SystemAPI.QueryBuilder`**：语法与 `EntityQueryBuilder` 相同，但会**自动缓存**，系统 `OnCreate` 时静态创建，运行期开销极小。
    
- **`GetEntityQuery`/描述结构体**（次选）：老写法，仍可用，但不如前两者易优化。
    

> 还可以把 **Aspect 的组件约束**并入查询（`WithAspect<T>`；放在最后以避免别名问题）。

**示例（`ISystem` + `OnCreate` 缓存 `Query`）：**

```
public partial struct FindMovablesSystem : ISystem
{
    private EntityQuery _q;

    public void OnCreate(ref SystemState state)
    {
        _q = SystemAPI.QueryBuilder()
             .WithAll<LocalTransform, MoveSpeed>()
             .WithNone<Prefab>()                  // 默认就会排除 Prefab，这里显式说明
             .Build();
        state.RequireForUpdate(_q);               // 没匹配就不跑系统
    }
}
```

（`SystemAPI.QueryBuilder` 可构建并缓存 `Query`；`RequireForUpdate(EntityQuery)` 让系统“有匹配才跑”。）

#### 三、查询条件 & 过滤：All/Any/None + Filters

- **集合约束**：  
    `WithAll<T>()`（必须都有），`WithAny<T>()`（至少一个），`WithNone<T>()`（必须没有）。
    
- **变更过滤（Changed）**：  
    - 只处理“自上次系统运行后**发生变化**的 Chunk”。
    - 高级接口：`SetChangedVersionFilter` / `AddChangedVersionFilter`；等价的“语法糖”在 `SystemAPI.Query` 场景也有（`WithChangeFilter<T>`）。
    
- **结构变更过滤（`OrderVersion`）**：  
    只处理“发生结构改动”的 Chunk（如加删组件/生成销毁）。`AddOrderVersionFilter()`。
    

#### 四、Query 选项（`WithOptions`）——Prefab/禁用/启用位等

通过 `WithOptions(EntityQueryOptions)` 改变匹配策略：

- `IncludePrefab`：**包含**带 `Prefab` 标签的原型（默认大多数查询会排除 Prefab）。

- `IncludeDisabledEntities`：**包含**带 `Disabled` 标签的实体。
    
- `IgnoreComponentEnabledState`：**忽略可启用组件**的启用位，强制把它们当“存在”对待。默认情况下，**可启用组件禁用时，会被当作查询里“仿佛没有该组件”**（这点很关键）。

- `FilterWriteGroup`：按 `WriteGroup` 约束过滤，只选明确包含的写组成员。
    

#### 五、如何在主线程和作业里“消费”一个 Query？

- **主线程枚举**（推荐易用）：`foreach (var (...) in SystemAPI.Query<...>())`；支持 `RefRO/RefRW<T>`、`DynamicBuffer<T>`、`Aspect`、`EnabledRef*` 等参数类型，源码生成自动缓存句柄与依赖。
    
- **作业化**：把 Query 传给 `IJobEntity` 的 `Schedule(_q, state.Dependency)`，或用 `IJobChunk`/`ArchetypeChunk` 级处理。`EntityQueryBuilder`/`SystemAPI.QueryBuilder` 创建的查询都能用于并行调度。
    
#### 六、高频用法

1. **系统启停/降噪**  
    - 用 `state.RequireForUpdate(query)` 只在“真有目标实体”时执行；
    
    - 想快速判断是否空，优先 `IsEmpty` 而非 `CalculateEntityCount()`（后者会执行并应用过滤，开销更大）。
    
2. **只在“发生变化”时工作**  
    - 例如 `UI`/摄像机桥接、网格合批、`LOD` 计算：对 `LocalToWorld` 或自定义标记加 **Changed 过滤**，减少无效遍历与提交。
    
3. **处理启用位与 Prefab**
    
    - 默认禁用的**可启用组件**在查询上等同“缺失”，因此“眩晕/隐形/冷却中”等开关不会误入查询；若要**无视启用位**做诊断或批改，叠加 `IgnoreComponentEnabledState`。
        
    - 需要在烘焙或生成流水线里批量扫描 **Prefab 实体**，再加 `IncludePrefab`。
        

#### 七、客户端实践

**A. 变更过滤 + 主线程枚举（只在位置变更时刷新表现）**
```
[UpdateInGroup(typeof(PresentationSystemGroup))]
public partial struct DrawNamesIfMovedSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        foreach (var (localToWorld, name) in
            SystemAPI.Query<RefRO<LocalToWorld>, RefRO<NameTag>>()
                    .WithChangeFilter<LocalToWorld>()) // 仅LocalToWorld变更的实体
        {
            // 将位置同步到 UI/渲染层
        }
    }
}
```

（`SystemAPI.Query` 支持 `WithChangeFilter<T>`；只处理“自上次系统运行后 `LocalToWorld` 发生变化”的数据。）

**B. 带选项的 `QueryBuilder` + `IJobEntity`（忽略启用位、包含禁用实体）**
```
public partial struct LowFreqAIUpdateSystem : ISystem
{
    private EntityQuery _q;
    public void OnCreate(ref SystemState state)
    {
        _q = SystemAPI.QueryBuilder()
             .WithAll<AIState, LocalTransform>()
             .WithOptions(EntityQueryOptions.IncludeDisabledEntities |
                          EntityQueryOptions.IgnoreComponentEnabledState)
             .Build();
        state.RequireForUpdate(_q);
    }

    public void OnUpdate(ref SystemState state)
    {
        state.Dependency = new AIJob().ScheduleParallel(_q, state.Dependency);
    }

    partial struct AIJob : IJobEntity { public void Execute(ref AIState ai) 
    { /*…*/ } 
    }
}

```
（`IncludeDisabledEntities` 与 `IgnoreComponentEnabledState` 控制“禁用实体/启用位”的匹配；Query 直接传入 `IJobEntity.ScheduleParallel`。）

#### 八、面试“易错点”

- **Prefab 是否默认参与？** 通常 **不参与**；要显式 `IncludePrefab`。
    
- **可启用组件如何影响匹配？** 默认**禁用 = 视作不存在**；如需忽略启用位，用 `IgnoreComponentEnabledState`。
    
- **Changed 过滤有两套入口**：低层 `SetChangedVersionFilter/AddChangedVersionFilter`，高层 `WithChangeFilter<T>`。
    
- **统计数量**：`IsEmpty` 比 `CalculateEntityCount()` 便宜；后者会执行查询并应用过滤。
    

#### 九、总结

> **`EntityQuery` = `ECS` 的数据选集。** 用 `EntityQueryBuilder` / `SystemAPI.QueryBuilder` 组合 **All/Any/None** 创建查询；再按需加 **Filters**（`Changed`/`OrderVersion`）与 **Options**（`IncludePrefab`、`IncludeDisabledEntities`、`IgnoreComponentEnabledState` 等）。
> 
> 主线程用 `SystemAPI.Query` 迭代，作业用 `Schedule(query, …)`；系统启停用 `RequireForUpdate(query)`。
> 
> **默认禁用位=当作缺失**，Prefab 默认不匹配；**Changed 过滤**能显著省帧内无效工作。