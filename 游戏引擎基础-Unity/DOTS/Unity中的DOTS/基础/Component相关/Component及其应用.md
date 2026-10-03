#### 一、什么是 `Component`？

在 `ECS` 中，`Component` 是**纯数据**（无行为）——系统按帧读取/写入它来推进游戏世界。DOTS中，组件被划成若干类型：**非托管组件**（最常用）、**托管组件**、**动态缓冲组件**、**共享组件**、**清理组件**、**分块（Chunk）组件**、**可启用组件**（开关位）以及**标签组件**、**单例组件**等概念，用于不同的性能/组织诉求。

#### 二、组件主要类型、关键点与客户端典型用途

**1) 非托管组件（`Unmanaged IComponentData`）**

- **定义/限制：** 字段必须是“允许的非托管类型集合”；最常用，能被`Job`调度和`Burst`编译。
    
- **客户端用途：** 移动、数值、状态、插值数据等全在这层，热路径全部用它，保证可并行与零 `GC`。
    
**2) 托管组件（`Managed Component`）**

- **特性：** 可以存任何引用类型，但**不能在 Job/Burst 中用** ，有 GC 成本。仅在主线程访问。
    
- **用途：** 在客户端，偶尔用来挂 `UnityEngine.Object` 引用（如音频源/特效句柄）
    
**3) 动态缓冲组件（`IBufferElementData` → `DynamicBuffer<T>`）

- **特性：** 组件本体是“可变长数组”；可通过 `InternalBufferCapacity` 指定**块内容量**，超出会溢出到堆。
    
- **用途：** 轨迹点、子弹命中列表、网络采样历史、单位编队路径等“变长数据”。
    

**4) 可启用组件（`IEnableableComponent`）**

- **特性：** 可对 **`IComponentData/IBufferElementData`** 加“启用位”；**启用/禁用不会产生结构变更**，可在工作线程切换。查询把禁用视作“仿佛没有该组件”。
    
- **用途：**做轻量状态机：眩晕/隐形/无敌/冷却中等开关，不用 `ECB` 也能并行改。
    

**5) 共享组件（`ISharedComponentData`）**

- **特性：** 按**值**把实体**分簇到同一 Chunk** ，用于去重与批处理；修改共享值会触发**结构变更**并搬迁实体。支持托管/非托管版本。
    
- **用途：** 客户端渲染/区域分组：按材质/网格/场景区块/流式分块 ID 分组，提升批处理/裁剪效率。
    

**6) 清理组件（`ICleanupComponentData`）**

- **特性：** 销毁带清理组件的实体时，Unity只移除**非清理**组件；实体带着清理组件**暂留**，待清理完再移除清理组件才真正消失。
    
- **用途：**“延迟收尸”：如播放死亡特效/计分/掉落在下一帧统一处理。
    

**7) 分块（Chunk）组件**

- **特性：** 把“值”存到**整块（Chunk）** 而不是单个实体上，便于**按块决策**（比如整块都不在屏幕内就跳过）。API 与普通组件不同。
    
- **用途：** 客户端可用它做**视锥/屏幕外剔除的快速门控**、分块 `LOD`。
    

**8) 标签组件（Tag）与单例（Singleton）**

- **标签：** 零大小、零内存，只做查询过滤；概念等同 `GO` 的 `Tag`。
    
- **单例：** 某类型在一个 `World` 里恰好只有一个实例；常存全局设定（例如输入/摄像机配置），配合 `SystemAPI.GetSingleton*` 访问。
    

#### 三、常用 API（以 `SystemAPI` 为主，面向系统/作业热路径）

- **按实体读写组件：**`SystemAPI.GetComponentRO<T>(e)` / `GetComponentRW<T>(e)`；注：遍历自己时优先用 `RefRO/RefRW<T>`，`GetComponent*` 主要用于“跨实体随机访问”。
    
- **缓冲：**`SystemAPI.GetBuffer<T>(e)`、`HasBuffer<T>(e)`；查询里可直接拿 `DynamicBuffer<T>`。
    
- **启用位：**`SystemAPI.IsComponentEnabled<T>(e)` / `SetComponentEnabled<T>(e,bool)`；查询侧用 `EnabledRefRO/RW<T>`。
    
- **单例：**`GetSingleton<T>() / GetSingletonRW<T>() / GetSingletonBuffer<T>()`。
    
- **主线程 `foreach`：**`foreach (var (...) in SystemAPI.Query<...>())`，源生成自动缓存查询与句柄并处理依赖。
    

#### 四、客户端实践

**① 可启用组件 = 轻量状态机**

> 眩晕、无敌、冷却、击退等“布尔态”不要乱增删组件，直接启用位开/关。

```
public struct Stunned : IComponentData, IEnableableComponent {} // 数据可为空 

// 切换：
SystemAPI.SetComponentEnabled<Stunned>(entity, true/false);

```
（启用位不触发结构变更，工作线程可切换。）

**② 动态缓冲 = 变长数据**

> 路径点/采样历史/命中列表。

```
[InternalBufferCapacity(16)] 
public struct Waypoint : IBufferElementData { public float3 Pos; } 

// 读写：
DynamicBuffer<Waypoint> path = SystemAPI.GetBuffer<Waypoint>(e);
```

（容量与溢出行为见[“Capacity”说明](https://docs.unity3d.com/Packages/com.unity.entities%401.0/manual/components-buffer-create.html?utm_source=chatgpt.com)。）

**③ 共享组件 = 批处理/分区**

> 按区块/材质分组，批渲/裁剪友好。

```
public struct Region : ISharedComponentData { public int Id; } // 赋值会触发结构变更并按值重新分块 

EntityManager.AddSharedComponent(e, new Region { Id = cellId });
```
（修改共享值会搬迁实体，是结构变更。）

**④ 清理组件 = 延迟销毁**

> 死亡→播特效→记分→再真正销毁。

```
public struct DeadCleanup : ICleanupComponentData {} // 打标 
// DestroyEntity 时仅移除非清理组件；等系统处理完再移除此组件以彻底删除实体
```

（清理组件生命周期与语义见[Unity 文档](https://docs.unity3d.com/Packages/com.unity.entities%401.0/manual/components-cleanup.html?utm_source=chatgpt.com)。）

**⑤ 分块组件 = 快速剔除门**

> 按 Chunk 存“是否在屏幕内/区域内”，整个块可跳过。

```
public struct ChunkVisible : IComponentData { public bool Value; }  // 定义成 Chunk 组件 
// 使用与添加见“Use chunk components”API 说明
```

（Chunk 组件用于按块优化并有独立 API。）

**⑥ 单例组件 = 全局只此一份**

> 输入、相机、全局配置。

```
public struct CameraSettings : IComponentData { public float Fov; } 

var cam = SystemAPI.GetSingletonRW<CameraSettings>();
```

（单例定义与获取方式见文档[Unity 文档](https://docs.unity3d.com/Packages/com.unity.entities%401.0/manual/components-singleton.html?utm_source=chatgpt.com)。）

#### 五、总结

> **组件是“数据”，系统是“行为”。**
> **常用：** 非托管组件跑 Job/Burst；托管组件仅主线程且有 GC；
> - **动态缓冲**装变长数据（`InternalBufferCapacity`）；
> - **可启用组件**开关不改结构、可在工作线程切；
> - **共享组件**按值分块、改值是结构变更；
> - **清理组件**让实体“带标延迟销毁”；**分块组件**存每块的门控数据；
> - **标签**零大小做过滤；**单例**存全局一份，`SystemAPI.GetSingleton*` 访问。查询/读写统一走 **`SystemAPI`**。