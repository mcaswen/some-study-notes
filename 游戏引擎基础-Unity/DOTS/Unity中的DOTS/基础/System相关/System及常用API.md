#### 一、`DOTS`中的`System`是什么

在`ECS`中，`System`负责接收上一帧的组件数据，并通过一系列处理，输出下一帧的组件数据。它们每帧在主线程按系统组（`SystemGroup`）的顺序运行

默认有三大根组：`Initialization`、`Simulation`、`Presentation`。若不指定，系统会被加入`Simulation`组。

利用特性`[UpdateInGroup]`、`[UpdateBefore]`、`[UpdateAfter]`可以控制所属组和先后顺序。

同时，DOTS提供两种系统写法：

- **`SystemBase`（类，托管）**：适合需要托管对象（如`UnityEngine`引用）的场景。只需实现`OnUpdate`，常配合`Entities.Foreach` / `IJobEntity`调度作业。

- **`ISystem`（结构体，非托管）**：适合全Burst、零托管引用的高性能逻辑。拥有三个回调，分别为`OnCreate`/`OnUpdate`/`OnDestroy`，且都接收`ref SystemState`，可以直接被Burst编译。还可选实现`ISystemStartStop`的 `Start/Stop` 回调。

#### 二、`System`及其组归属的最佳实践

- **`SimulationSystemGroup`**：适用于游戏规则、移动、`AI`、`NetCode` 预测/插值等“模拟”逻辑。所有`System`不额外指定下默认归属此组。

- **`FixedStepSimulationSystemGroup`**：适用于需要固定步长（如物理、体测定时器）的逻辑，内部依赖固定的`DeltaTime`，多次运行来“追上”当前帧。

- **`PresentationSystemGroup`**：与表现层耦合的系统（例如将`LocalToWorld`结果，渲染相关同步到表现），在`PreLateUpdate`末尾执行，避免抢占模拟时序。

- **常见做法**：**模拟放在`Simulation`，渲染/桥接放在`Presentation`，物理/确定步长放在`FixedStep`**。再利用属性把`System`放进正确的组里，并使用`UpdateBefore`/`After`精排顺序。

#### 三、`System/ISystem` 的典型生命周期与”开关“

- `OnCreate`：初始化查询，缓存`Lookup/Query`，或使用`state.RequireForUpdate<T>()`让系统仅再存在某组件时运行。
- `OnUpdate`：读取/写入组件，或**调度作业**（推荐）。可使用`state.Dependency`汇总/串联`Job`依赖。
- `OnDestroy`：清理原生容器等非托管资源。

#### 四、常用API

##### 1） `SystemAPI`
`SystemAPI`在`ISystem`（或`SystemBase`的实例方法）里提供**高频缓存**和**便捷访问**：包括组件读写、单例、时间、查询等功能。
- **时间相关**：`SystemAPI.Time.DeltaTime`（与世界绑定、受组影响；在定步组里是固定步长）。
- **组件直取**：`GetComponentRO<T>(entity)` / `GetComponentRW<T>(entity)`、`HasComponent<T>(entity)`。
- **`Buffer` 直取**：`GetBuffer<T>(entity)`与`GetBufferLookup<T>(ro)`。
- **单例`Singleton`**：`GetSingleton<T>()` / `TryGetSingleton<T>(out ...)` / `SetSingleton<T>`。
- **查询`Foreach`**：`foreach (var (...) in SystemAPI.Query<...>())`，源生成自动缓存`EntityQuery`与`TypeHandler`，并在进入`foreach`前处理并发依赖；支持`RefRO/RefRW`、`DynamicBuffer<T>`、`EnabledRef*`、`IAspect`等。

**示例（`ISystem` + idiomatic `foreach`）**：
```
using Unity.Burst;
using Unity.Entities;
using Unity.Mathematics;
using Unity.Transforms;

[BurstCompile]
[UpdateInGroup(typeof(SimulationSystemGroup))]
public partial struct MoveSystem : ISystem
{
    [BurstCompile]
    public void OnCreate(ref SystemState state)
    {
        state.RequireForUpdate<LocalTransform>(); // 无此组件时不跑
    }

    [BurstCompile]
    public void OnUpdate(ref SystemState state)
    {
        float dt = SystemAPI.Time.DeltaTime; // 世界时间
        foreach (var (transform, speed) in
                 SystemAPI.Query<RefRW<LocalTransform>, 
                 RefRO<MoveSpeed>>())
        {
            transform.ValueRW.Position += speed.ValueRO.Value * dt;
        }
    }
}

public struct MoveSpeed : IComponentData { public float3 Value; }
```
（`SystemAPI.Query`的行为、可用参数与自动依赖处理详见[文档示例与清单](https://docs.unity.cn/Packages/com.unity.entities%401.0/manual/systems-systemapi-query.html) )

##### 2) 启用/禁用组件 （不触发结构更改）

客户端常用来做“轻量状态机”。让组件实现`IEnableableComponent`，用：
- `SystemAPI.SetCOmponentEnabled<T>(entity, bool)`（**工作线程也可**，不触发结构更改）
- 查询测可用`EnabledRefRW<T> / EnabledRefRO<T>`。

##### 3） `EntityCommandBuffer`（`ECB`）--安全地做结构变更

结构变更（创建/销毁实体、增删组件）应积攒到 **`ECB`** 回放点（`Begin/End` X 组）。
在系统里通过**该组的`Singleton`** 获取`ECB`并回放：
```
[BurstCompile]
[UpdateInGroup(typeof(SimulationSystemGroup))]
public partial struct DestroyDeadSystem : ISystem
{
    [BurstCompile]
    public void OnUpdate(ref SystemState state)
    {
        var ecb = SystemAPI
          .GetSingleton<EndSimulationEntityCommandBufferSystem.Singleton>()
          .CreateCommandBuffer(state.WorldUnmanaged);

        foreach (var (hp, entity) in 
                 SystemAPI.Query<RefRO<HP>>().WithEntityAccess())
        {
            if (hp.ValueRO.Value <= 0) ecb.DestroyEntity(entity);
        }
    }
}
```

##### 4）`IJobEntity`/`Job` 调度与依赖

- 推荐用`IJobEntity`描述“遍历一批组件”的作业，`state.Dependency = job.ScheduleParallel(..., state.Dependency);`管理依赖
- `SystemAPI.Query`的主线程遍历会在进入`foreach`前自动完成必要依赖。需要极致管控与性能，需要转向`IJobChunk`。

##### 5) Transform 常用组件

客户端经常改写 `LocalTransform` （包括位置/旋转/缩放），系统会在恰当阶段推导`LocalToWorld`。

#### 五、`SystemBase` vs `ISystem`：该选谁？

- 需要托管引用（纹理、`GO`、`UI`等）或沿用`Entities.Foreach`的遗留代码 -> `SystemBase`。

- 追求极致性能、全`Burst`、零`GC`、易并行（客户端数值/移动/插值等热点） -> `ISystem`。`OnCreate/OnUpdate/OnDestroy` 可直接Burst；1.0之后方法支持默认实现，通常只写`OnUpdate`。

#### 六、面试快速问答

- **如何让系统只在有某单例/组件时运行？**
	在`OnCreate`中用`state.RequireForUpdate<T>()`（或传入`EntityQuery`）。

- **如何管理`DeltaTime`与定步/变布？**
	用`SystemAPI.Time.DeltaTime`取世界时间；在`FixedStepSimulationSystemGroup`中为固定步长，可能一帧内会多跑几次。

- **结构变更为何更推荐`ECB`？**
	直接在作业里变更结构不安全，`ECB`会在定义好的`Begin/End`组中回放，保证时序与安全。

- **如何保证与其他系统的先后？**
	把系统标记到合适的组，再用`UpdateBefore/After`做组内排序；仅同组内排序有效。

#### 七、`SystemBase` 版本的等价写法

```
using Unity.Entities;
using Unity.Transforms;
using Unity.Mathematics;

[UpdateInGroup(typeof(SimulationSystemGroup))]
public partial class MoveSystemBase : SystemBase
{
    protected override void OnUpdate()
    {
        float dt = SystemAPI.Time.DeltaTime;
        Entities.ForEach((ref LocalTransform tf, in MoveSpeed sp) =>
        {
            tf.Position += sp.Value * dt;
        }).ScheduleParallel();
    }
}

```