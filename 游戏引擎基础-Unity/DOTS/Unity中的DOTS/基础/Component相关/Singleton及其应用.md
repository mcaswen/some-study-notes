#### 一、什么是 `Singleton`

**结论**：在 **同一个 `World`** 中，**恰好只有一个实体**持有类型为 `T` 的组件时，`T` 就是一个 **`Singleton` 组件**。如果后来又有第二个实体也加上了 `T`，它立刻**不再是单例**；不同 `World` 彼此独立，各自可以有自己的 `T` 单例。

从设计目的看，`Singleton` 更像“**全局状态的 ECS 化表达**”：把“只有一份”的配置、时钟、模式开关、统计信息，放进“一个带有明确类型的实体+组件”里，而不是散落在静态字段或跨层引用中。这样做的收益是：仍然遵循 ECS 的**数据可查询、可依赖、可并行**的范式。


#### 二、访问方式与读写语义（SystemAPI 为主）

在系统内（`ISystem`/`SystemBase`）有一组 **`SystemAPI`** 方法用于直达单例：

- **读值**：`SystemAPI.GetSingleton<T>()`；
    
- **读写**：`SystemAPI.GetSingletonRW<T>()` 返回 `RefRW<T>`（在当前系统的依赖模型下安全修改）；
    
- **取实体句柄**：`SystemAPI.GetSingletonEntity<T>()`；
    
- **可选式查询**：`SystemAPI.TryGetSingleton<T>(out T)`、`TryGetSingletonEntity<T>(out Entity)`（避免异常）；
    
- **`Buffer` 单例**：`SystemAPI.GetSingletonBuffer<T>()`（`T : IBufferElementData`）。  
    这些 API 的“单例”条件都是：**当前 World 中恰好存在一个匹配实体**；否则会抛错，或在 `Try*` 版本中返回 `false`。
    

**两条使用限制**：

- `SystemAPI` 必须在**系统上下文**内调用（源生成将其替换为缓存查询与类型句柄）；不能在任意工具函数或 `Aspect` 内乱用。
    
- 这些 `GetSingleton*` 的泛型参数 **不支持可启用组件**（`IEnableableComponent`）
    

#### 三、和系统启停的关系：用 `RequireForUpdate` 做“硬前置”

系统常常依赖“某个单例已经存在”才能运行。此时在 `OnCreate`（或早期）调用：

`state.RequireForUpdate<SomeSingleton>();    // 泛型重载：要求某组件必须存在 // 或：state.RequireForUpdate(query);       // 查询重载：要求某查询必须命中`

只要该条件不满足，本系统就**不更新**（即使还有其他查询命中）。需要注意：**对可启用组件，这里的检查只看“存在”，不看“启用位”**。

这条规则在客户端项目里很实用：例如“没有 `GameConfig` 单例就先别跑表现层系统”，或者“必须等到 `ClientServerTickRate` 单例就绪再启动预测链”。


#### 四、客户端实践

##### 1）“配置/模式”单例：集中化可查询的全局参数

把帧率策略、画质开关、难度、关卡 ID、UI 语言等合并到一个 `GameConfig : IComponentData` 单例中。系统通过 `GetSingleton` 读取、`GetSingletonRW` 修改（例如切换语言、热更新数值），而不是到处塞静态字段。

```
public struct GameConfig : IComponentData
{
    public int QualityLevel;
    public float MouseSensitivity;
}

[BurstCompile]
public partial struct QualitySwitchSystem : ISystem
{
    public void OnCreate(ref SystemState state) => state.RequireForUpdate<GameConfig>();

    public void OnUpdate(ref SystemState state)
    {
        var cfg = SystemAPI.GetSingletonRW<GameConfig>();
        // …基于事件调整 cfg.ValueRW.QualityLevel
    }
}

```
这里的读写是**依赖安全**的：源生成会在系统内缓存查询/句柄并参与依赖推导。

##### 2）“事件总线”单例：用 **`SingletonBuffer`** 做跨系统消息队列

将 `struct DamageEvent : IBufferElementData { … }` 作为某个**单例实体的动态缓冲**，不同系统往里“追加事件”，再由消费系统一次性读取并清空。

- 读取：`var buf = SystemAPI.GetSingletonBuffer<DamageEvent>();`；
    
- 并行写入时，优先使用 **ECB 的 `AppendToBuffer`**（并传入 `sortKey` 保确定性），避免锁与竞态。

- 这种做法把“全局消息队列”从静态/脚本事件迁到 ECS 数据层，调试与回放都更可控。
    
##### 3）“系统服务”单例：例如 **ECB 系统单例**

DOTS 自带若干 **CommandBuffer 系统**（如 `EndSimulationEntityCommandBufferSystem`）以**单例组件**形式暴露，用法是先 `GetSingleton<...>.CreateCommandBuffer` 获取 ECB，再在本系统里录制结构变更，交由对应组末尾统一回放。这是一种把“命令回放点”显式化的工程约定。

#### 五、与普通查询/Job 的边界

- **不是“魔法全局变量”**：`Singleton` 仍然只是“**有唯一实例**的普通组件”，受同样的**依赖与生存期**管理；只是访问 API 更直接。
    
- **与 Job 的配合**：在作业里需要这份“全局数据”时，推荐**把值拷入 Job 字段**（或用 `ComponentLookup`/`BufferLookup` 进行只读/只写查表），避免在 Job 内直接走 `EntityManager` 产生同步点。`SystemAPI` 的查询缓存与依赖更新由源生成保障。


#### 六、常见误区与纠偏

- **“取单例时抛异常”**  
    直接 `GetSingleton<T>()` 需要“恰好一个”；**0 或 >1 个**都会报。进入帧更新前用 `RequireForUpdate<T>()` 保证存在，或在运行期用 `TryGetSingleton` 路径处理。
    
- **“把启用位当条件”**  
    单例 API 不接受 `IEnableableComponent`，而 `RequireForUpdate<T>()` 对启用位也**不敏感**。若确实要“可开关”的单例，改用**独立布尔字段**或**可启用的子组件**+普通查询。
    
- **“SystemAPI 用在`System`外”**  
    诸如工具类、`Aspect`、`Entities.ForEach` 中直接调 `SystemAPI.GetSingleton` 都不被支持；必须在`System`方法内使用。
    
- **“当作跨 World 全局”**  
    单例是**每个 World 独立**；在多 World（例如 Client/Server/Thin Client）架构下，不同 World 的同一个单例类型各自维护一份数据。
    

#### 七、性能与代码生成的注意点

`SystemAPI.GetSingleton*` 并非“每次重建查询”；源生成会为所在系统**生成并缓存查询与类型句柄**，在 `OnCreate`/`OnUpdate` 自动 `Update`，因此成本可控、用在热路径是合理的（但仍应避免在每帧调用多次相同 API 冗余取值）。

#### 八、总结

> **Singleton 是“在某个 World 中仅有一份的组件”，用于把全局配置、模式开关、时钟、服务对象等以 ECS 数据的形式表达。** 
> 
> 访问与修改通过 `SystemAPI.GetSingleton / GetSingletonRW / GetSingletonBuffer / GetSingletonEntity`，需要存在性保障时用 `RequireForUpdate<T>()` 阻断系统更新；不确定存在时，用 `TryGet*` 系列走可选路径。
> 
> 与 `Job` 协同时，把单例数据拷入 `Job` 字段或用 `Lookup` 查表，避免同步点。
> 
> 工程上，常见的三类用法分别是“配置/模式单例”“事件总线型单例缓冲”“服务型单例（例如 ECB 系统）”。
> 
> 需要注意的边界包括：**单例是 per-World 的、`IEnableableComponent` 不能用于这些 API、`SystemAPI` 只能在系统上下文调用、`GetSingleton` 对“多或少一份”都会报错**。
> 
> 这些约束落实到实践中，可以让客户端项目的“全局状态”在 ECS 范式里保持**可查询、可依赖、可测试**。