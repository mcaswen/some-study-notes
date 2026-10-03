#### 一、Baker 是什么？在什么时候运行？

`Baker` 是把 **`GameObject/MonoBehaviour` 的 `Authoring` 数据** 转成 **`ECS` 实体与组件** 的桥梁。写 `Baker<TAuthoring>` 并实现 `Bake`的这一步里，就会从 `MonoBehaviour` 读数据、向实体写组件；它只在 **编辑器的“烘焙（baking）流程”** 里运行，而不是在玩家运行时（`Play/Player`）运行。

烘焙过程有以下阶段：先为每个子场景（`Subscene`）里的 `GO` 预建空实体，再跑各类 Baker，最后再跑 **Baking Systems** 做批处理；`Baker` 之间**无执行顺序保证**，因此 **`Baker` 不能互相读/改别人的组件，只能“新增”组件**。

编辑器里既有**全量烘焙**也有**增量/实时烘焙**（`Subscene` 打开时），烘焙结果会被写成 `Entity Scene`，供运行时加载。

#### 二、为什么客户端要关心 Baker？

它决定“运行时看到的 `ECS` 数据长什么样”，直接影响帧内的 **内存布局、变换链、渲染/物理数据** 等。把重数据与格式转换提前到烘焙期做（例如网格/材质、路径、配置表、曲线 转换为 Blob），能显著降低运行时 `CPU/GPU` 压力和 `GC` 风险。

官方也强调：`Baker` 读托管 `Authoring` 数据是串行的，效率较低；而 **Baking System** 用查询批处理，适合重活/并行。
#### 三、核心概念：`TransformUsageFlags`

为什么很多 Baker 代码第一行都是 `GetEntity(TransformUsageFlags.Dynamic)`？  

-因为 **`TransformUsageFlags`** 决定烘焙后给实体加哪些“变换相关组件”（是否可移动、是否渲染、是否需要非均匀缩放、是否保持世界空间等），能避免不必要的 `LocalTransform/Parent` 等冗余。

常用值有 `None / Renderable / Dynamic / WorldSpace / NonUniformScale / ManualOverride`，多个标志会**合并**后再决定最终变换组件。

#### 四、最常用的 Baker API（1.x）

1. **`GetEntity` 系列**
- 取“主实体”：`GetEntity(TransformUsageFlags flags)`；
    
- 把**其他 `GameObject/Prefab`** 注册并转成实体：`GetEntity(GameObject go, TransformUsageFlags flags)`（这一步会自动建立依赖，使其修改能触发重烘焙）。
    
2. **读 Authoring（带依赖跟踪）**
- 用 `GetComponent<T>() / GetComponentInChildren<T>() / GetComponents...` **代替** `GameObject` 上的同名方法，以便烘焙管线追踪依赖。
    
3. **向实体写数据**
- `AddComponent(entity, in T)`、`AddBuffer<T>(entity)` 等把 `IComponentData/IBufferElementData` 加上去；若要加**托管组件**（class 组件/`UnityEngine` 对象），用 `AddComponentObject(entity, object)`。
    
4. **声明额外依赖与自定义数据**
- `DependsOn(obj)`：对脚本化资源（`ScriptableObject`）、纹理、任意 `UnityEngine.Object` 建立依赖，变更可触发重烘焙。
    
- `AddBlobAsset(ref blobRef, out Hash128)` / `AddBlobAssetWithCustomHash(...)`：把构建好的 Blob 资产注册到烘焙输出，避免重复并可用哈希去重；Blob 的构建流程见[官方模板](https://docs.unity3d.com/Packages/com.unity.entities%401.0/api/Unity.Entities.IBaker.AddBlobAsset.html)（`BlobBuilder → CreateBlobAssetReference → Dispose`）。
    
5. **Prefab 与额外实体**
- 预注册可在运行时实例化的 **Entity Prefab**：`var ep = GetEntity(authoring.Prefab, TransformUsageFlags.Dynamic)` 然后写到组件里；或用 **`EntityPrefabReference`** 让 Prefab 独立序列化、被需时加载。
    
- 需要为同一 Authoring 生成多个实体时，用 `CreateAdditionalEntity()`。
    
6. **过滤烘焙输出**
- 仅供烘焙中转、不想出现在运行时：给该实体打上 `BakingOnlyEntity`；或用 `[BakingType] / [TemporaryBakingType]` 标注“只在烘焙期存在/可被系统消费后丢弃”的组件。
    

#### 五、代码范式
```
using Unity.Entities;
using Unity.Mathematics;
using UnityEngine;

// 1) Authoring：策划/美术填数据
public class ProjectileAuthoring : MonoBehaviour
{
    public GameObject ProjectilePrefab;     // 发射体Prefab（GO）
    public ScriptableObject Balance;        // 平衡表
    public float Speed = 20;
    public AnimationCurve DropCurve;        // 下坠曲线，烘焙成 Blob
}

// 2) 运行时组件
public struct Projectile : IComponentData
{
    public float Speed;
    public Entity Prefab;                   // 直接引用 Entity Prefab
    public BlobAssetReference<CurveBlob> Drop; // 自定义 Blob
}

// 3) 自定义 Blob 根结构
public struct CurveBlob
{
    public BlobArray<float> Times;
    public BlobArray<float> Values;
}

// 4) Baker：把 Authoring → ECS 数据
public class ProjectileBaker : Baker<ProjectileAuthoring>
{
    public override void Bake(ProjectileAuthoring a)
    {
        // 主实体 + 声明“会移动”，以获得必要的变换组件
        var entity = GetEntity(TransformUsageFlags.Dynamic);                          // TransformUsageFlags
        // 预注册 Prefab，转为可实例化的 Entity prefab
        var prefabEntity = GetEntity(a.ProjectilePrefab, TransformUsageFlags.Dynamic);// Prefab 注册

        // 声明对外部对象的依赖（改了就触发重烘焙）
        DependsOn(a.Balance);                                                         // 依赖追踪

        // 把曲线烘焙成 Blob
        var builder = new BlobBuilder(Allocator.Temp);
        ref var root = ref builder.ConstructRoot<CurveBlob>();
        var times = builder.Allocate(ref root.Times, a.DropCurve.length);
        var vals  = builder.Allocate(ref root.Values, a.DropCurve.length);
        for (int i = 0; i < a.DropCurve.length; i++) { times[i] = a.DropCurve[i].time; vals[i] = a.DropCurve[i].value; }
        var blobRef = builder.CreateBlobAssetReference<CurveBlob>(Allocator.Persistent);
        builder.Dispose();
        AddBlobAsset(ref blobRef, out _);                                             // 注册 Blob，参与去重

        // 写入运行时组件
        AddComponent(entity, new Projectile { Speed = a.Speed, Prefab = prefabEntity, Drop = blobRef });

        // 如需额外实体（例如碰撞分离/音效触发器）
        // var extra = CreateAdditionalEntity(TransformUsageFlags.Dynamic);
        // AddComponent(extra, new SomeTag());
    }
}

```

上面的每一处 API/思路都有[官方依据](https://docs.unity3d.com/Packages/com.unity.entities%401.0/api/Unity.Entities.IBaker.GetEntity.html)：`GetEntity(...flags)` 与对 Prefab 的注册、`TransformUsageFlags` 的用途、`DependsOn` 依赖、`AddBlobAsset` 的注册与去重、`CreateAdditionalEntity` 的额外实体。

#### 六、Baker 与 Baking System 的职责边界（面试高频追问）

Baker 做“**逐 authoring 组件**”的转换（只增不改他人组件），而 **Baking System** 在 Baker 之后统一批处理 `ECS` 数据（可用 Burst/Jobs），但需要**显式声明依赖**并对增量烘焙自己做“加也要会减”的收尾逻辑。

要把实体留到运行时，**必须在 Baker 里创建**；Baking System 中创建的实体只用于烘焙期间的中转。

#### 七、客户端项目里的落地清单

- **可实例化子弹/特效/单位**：用 **Prefab 注册**（或 `EntityPrefabReference`）+ 运行时 `Instantiate`，既省内存又能任意复用。
    
- **控制“是否产生变换负担”**：静态装饰物只要 `Renderable`，会动的用 `Dynamic`；大量静态体可合并为世界空间 `LocalToWorld`，避免层级与写入。
    
- **大表/曲线/路径**：在 Baker 里做 **Blob**，运行时只读零 `GC`；用 `AddBlobAssetWithCustomHash` 或默认哈希去重。
    
- **跨资源触发重烘焙**：脚本化资源、材质、网格等都用 `DependsOn` 声明；改了立刻重烘焙/热更新场景数据。
    
- **只在烘焙期存在的“临时数据/中间结果”**：`[TemporaryBakingType]`/`[BakingType]`、`BakingOnlyEntity` 让运行时更干净。

#### 八、30 秒速背版

> **`Baker`** = 把 `MonoBehaviour`（`Authoring`）→ 实体组件（`Runtime`）的**一次性转换器**，只在编辑器烘焙期跑；无序、只增不改别人。
>  
>  **常用**：
> - `GetEntity(+TransformUsageFlags)` 决定变换组件；
> - `GetEntity(go, flags)` 注册 Prefab；
> - `GetComponent*` 带依赖追踪；
> - `DependsOn` 建立额外依赖；
> - `AddBlobAsset` 注册 Blob；
> - `CreateAdditionalEntity` 生成额外实体；
> - 需要过滤输出用 `BakingOnlyEntity` / `[TemporaryBakingType]`。
> - 重活/并行交给 **Baking System**（批处理、要自己维护增量一致性）。