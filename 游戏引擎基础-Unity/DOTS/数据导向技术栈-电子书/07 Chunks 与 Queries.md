# Chunks（分块）

在同一个 **Archetype（原型）** 里，实体及其组件被存放在称为 **Chunk（分块）** 的内存块中。**每个 Chunk 最多容纳 128 个实体**；每种组件各自占用 **一段独立的连续数组**。例如，若某原型包含组件 A 与 B，那么每个 Chunk 内会有三段数组：

- 一段是 **实体 ID**；
    
- 一段是 **A 组件**；
    
- 一段是 **B 组件**。  
    第 1 个实体对应三段数组的索引 0，第 2 个实体对应索引 1，以此类推。这些数组**始终保持紧凑**：新增实体会写入**第一个空槽**；移除实体会把**最后一个实体**搬来**填补空位**（实体被销毁或迁移到其他原型时会从当前 Chunk 移除）。
    

# Queries（查询）

基于 **原型 + 分块** 的数据布局，有一个直接好处：能**高效地查询并遍历**目标实体集合。做一条“具有若干组件类型”的查询时，ECS 会先找出**所有匹配条件的原型**，然后**按这些原型的 Chunk** 逐个遍历：

- 因为组件就在 **紧凑的连续数组**里，按序遍历**大幅减少缓存未命中**；
    
- 多数程序在运行期原型集合**相对稳定**，因此**匹配的原型集合可被缓存**，进一步降低查询成本。
    

示例系统（节选，展示 `SystemAPI.Query` 的基本用法）：

```
public partial struct MonsterMoveSystem : ISystem 
{
    [BurstCompile]
	public void OnUpdate(ref SystemState state)
	{         
		// 查询：遍历拥有 LocalTransform、Velocity 且带 Monster 标签的所有实体
		foreach (var (transform, velocity) in
			SystemAPI.Query<RefRW<LocalTransform>, RefRO<Velocity>>()                        .WithAll<Monster>())         
		{             
			// 按速度推进位置（乘以 delta time）
			transform.ValueRW.Position +=               
			velocity.ValueRO.Value * SystemAPI.Time.deltaTime;         
		}     
	} 
}
```
（要点：`RefRW<T>` 表示可写组件引用，`RefRO<T>` 表示只读组件引用；`WithAll<TTag>()` 用于要求实体具备某标签/组件。）