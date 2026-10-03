# 与 Job 系统的整合（Job system integration）

只要某个实体的**组件类型是非托管的（unmanaged）**，它们就可以在 **Burst 编译**的作业（job）中被访问。为了高效访问实体，ECS 提供了两个专用的作业接口：**`IJobChunk`** 与 **`IJobEntity`**。

下面是一个调度 `IJobEntity` 的**简单系统示例**：

```
// 一个调度 IJobEntity 的简单系统 
public partial struct MonsterMoveSystem : ISystem 
{
	[BurstCompile]
	public void OnUpdate(ref SystemState state)     
	{         
		// 创建并调度作业         
		var job = new MonsterMoveJob 
		{             
			DeltaTime = SystemAPI.Time.DeltaTime         
		};         
		job.ScheduleParallel(); // 并行调度     
	} 
}  

// 一个由 Burst 编译的作业：处理所有同时具有 
// LocalTransform、Velocity 与 Monster 组件的实体 
[WithAll(typeof(Monster))] 
[BurstCompile] 
public partial struct MonsterMoveJob : IJobEntity 
{     
	public float DeltaTime;      
	// 需要修改 LocalTransform，用 ref；     
	// 只读取 Velocity，用 in。     
	public void Execute(ref LocalTransform transform, in Velocity velocity)
	{         
		transform.Position += velocity.Value * DeltaTime;     
	} 
}
```

为方便使用，**系统之间**可以**自动**处理作业的**依赖**与**完成**（也就是跨系统管理依赖图与 `Complete` 时机，不强迫你手写全部依赖传递）。

---

# 子场景与烘焙（Subscenes and baking）

Unity ECS 使用**子场景（Subscenes）**来管理内容（不是直接用传统 Scene），原因是 Unity 核心的场景系统与 ECS **不兼容**。

虽然**不能**把实体**直接**放进传统场景，但通过名为 **Baking（烘焙）** 的流程，可以从场景中**加载实体**：把 **GameObject 与 MonoBehaviour 组件**转换为**实体与 ECS 组件**。

可以把**子场景**理解为：**嵌入在其他场景中的场景**，并且在你编辑子场景时，**烘焙会重新运行**。对一个子场景中的**每个 GameObject**，烘焙都会**创建一个实体**；这些实体随后被**序列化到文件**里；**运行时加载子场景**时，加载的是这些**实体**，而**不是**原本的 GameObject。

**哪些组件会被添加到烘焙后的实体上**，取决于与各个 GameObject 组件**关联的 “Baker”**。例如，与标准渲染组件（如 `MeshRenderer`）关联的 baker 会往实体上添加**图形相关**的组件。对你自己的 `MonoBehaviour`，也可以自定义 **baker** 来精确控制要往实体上加哪些组件。

下面是官方示例里**Authoring 组件 + Baker** 的精简版本（中文注释仅为解释，不改变示例语义）：

```
// 实体组件：能量护盾（血量、上限、回复延迟/速率） 
public struct EnergyShield : IComponentData 
{     
	public int   HitPoints;     
	public int   MaxHitPoints;     
	public float RechargeDelay;     
	public float RechargeRate; 
}  
// 一个简单的 Authoring 组件（普通 MonoBehaviour） 
// 关键在于它定义了一个 Baker 类 
public class EnergyShieldAuthoring : MonoBehaviour {     
	public int   MaxHitPoints;     
	public float RechargeDelay;     
	public float RechargeRate;      
	// 对应的 Baker：会在子场景内，每个挂了     
	// EnergyShieldAuthoring 的 GameObject 上运行一次     
	class Baker : Baker<EnergyShieldAuthoring>     
	{         
		public override void Bake(EnergyShieldAuthoring authoring)         
		{             
			// 通过 TransformUsageFlags 指定实体是否需要变换数据             
			var entity = GetEntity(TransformUsageFlags.None);              
			// 往实体上添加一个组件，并把 Authoring 的数据拷入             
			AddComponent(entity, new EnergyShield             
			{                 
				HitPoints     = authoring.MaxHitPoints,                 
				MaxHitPoints  = authoring.MaxHitPoints,                 
				RechargeDelay = authoring.RechargeDelay,                 
				RechargeRate  = authoring.RechargeRate,             
			});         
		}     
	} 
}
```

一方面，**不能**在场景里直接加实体在**简单用例**下看起来有点不便；但另一方面，在**复杂用例**里，烘焙流程反而很强大：它把**编辑期的数据（Authoring / GameObject）**与**运行期的数据（烘焙后的实体）**彻底**解耦**，让“你在编辑器里直接看到/编辑的东西”和“运行时真正加载的东西”**不必一一对应**。例如，你可以在**烘焙阶段**写代码**程序化地生成**部分数据，把**运行时代价**前移到烘焙时完成。