#### 一、CommandTarget 是什么

**CommandTarget 是挂在“连接实体（connection entity）”上的组件**，里面只有一个指针 `targetEntity`。它表示：

- **在客户端**，输入系统应当把 `ICommandData` 写到哪个实体的命令缓冲；
    
- **在服务器**，接收系统应当把来自该连接的命令分发到哪个实体的命令缓冲。  
    未使用自动机制时，**必须**把 `targetEntity` 指向一个**带有至少一种 `ICommandData` 的实体**；薄客户端场景同样需要显式设置。
    

> 连接实体即带 `NetworkStreamConnection` 的那条 `ECS` 实体；连接被销毁时，这个实体随之销毁。

#### 二、CommandTarget 与“命令流”的关系

从输入到落地的大致路径可以这样拆解：

1. **客户端“写命令”**
    
    - 在 `GhostInputSystemGroup` 里，把一帧的输入值打包成 `ICommandData`，**写入 CommandTarget 所指实体**的 `DynamicBuffer<TCommand>`。若选择 `IInputComponentData` 路线，代码生成会自动把该组件在本帧的值拷入命令缓冲。
        
    - 命令随后由 `CommandSendPacketSystem` 按固定仿真 Tick 序列化、携带冗余历史并发给服务器。
        
2. **服务器“收命令并分发”**
    
    - 数据进入 `IncomingCommandDataStreamBuffer` 后，由接收系统将其**分发给连接实体的 `CommandTarget` 所指向的目标实体**的命令缓冲。服务器不应重写来自客户端的输入。
        
3. **多种命令类型的选择**
    
    - 即便同时存在多种 `ICommandData` 类型，**只会发送/分发与 `targetEntity` 上存在的那种命令**；因此 `CommandTarget` 既是“目标实体”，也是“本连接要发哪类命令”的**开关**。


#### 三、与 AutoCommandTarget 的取舍（何时自动，何时手动）

**自动模式**（更省样板）：在`Ghost`上开启 **Has Owner** 与 **Support Auto Command Target** 后，若该幽灵**归本机连接所有**且是 **Predicted/OwnerPredicted**，并且 `AutoCommandTarget.Enabled = true`，则会自动把命令发送到该`Ghost`；无需手工维护 `CommandTarget`。该模式**不适用于插值`Ghost`**。

**手动模式**（更可控）：以下场景建议或必须手动设置 `CommandTarget.targetEntity`：

- 项目要支持**薄客户端**（Auto 方案在薄客户端不生效）；
    
- 需要**动态切换**操控对象（例如“下车/上车”“切换炮塔”）；
    
- 多命令类型或多实体控制的**显式路由**。  
    两种方案可并存：开启 Auto 的同时，手动改 `targetEntity` 仍然有效。

#### 四、客户端实践

##### 范式 A：服务器在“进游戏”时一次性建链（最小必需）

> 目标：连接完成→分配所有权→指定命令目标→允许收发（`InGame`）。
```
// ServerSimulation 侧：为新连接生成玩家并设置命令目标
[WorldSystemFilter(WorldSystemFilterFlags.ServerSimulation)]
public partial struct ServerEnterGameSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        var ecb = new EntityCommandBuffer(Unity.Collections.Allocator.Temp);
        foreach (var (conn, netId, connEntity) in
                 SystemAPI.Query<RefRO<NetworkStreamConnection>, 
		                  RefRO<NetworkId>>()
                          .WithNone<NetworkStreamInGame>()
                          .WithEntityAccess())
        {
            var player = ecb.Instantiate(/* Player Ghost Prefab */);
            // 指定所有权（便于预测/权限）
            ecb.AddComponent(player, new GhostOwner { NetworkId = netId.ValueRO.Value });
            // 指定该连接的命令目标：把命令落到这个玩家实体
            ecb.SetComponent(connEntity, new CommandTarget { targetEntity = player });
            // 准许这条连接开始收发快照与命令
            ecb.AddComponent<NetworkStreamInGame>(connEntity);
        }
        ecb.Playback(state.EntityManager);
    }
}
```
- “设置命令目标→InGame”应在同一条逻辑里完成，避免出现“已 InGame 但命令无处可去”的空窗。上面几步分别对接了连接、命令分发与 InGame 的硬性要求。
    

##### 范式 B：客户端动态切换控制对象（上车/下车）

> 目标：本地拥有多个可控实体时，切换 `targetEntity` 指向。

- 切换后，客户端会把下一帧的 `ICommandData` 写入新的实体缓冲；服务器也会把该连接后续命令分发给新的实体。**薄客户端**同样依赖这种显式路由。
```
// ClientSimulation 侧：根据本地“切换控制”事件，改写 CommandTarget
[WorldSystemFilter(WorldSystemFilterFlags.ClientSimulation)]
public partial struct ClientSwitchControlSystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        if (!SystemAPI.TryGetSingletonEntity<NetworkId>(out var conn)) return;
        // 例如：侦测某个 UI/交互事件后，选择新的 controlled 实体
        var newControlled = /* 查询或缓存的本地拥有实体（GhostOwnerIsLocal 标记） */;
        if (newControlled != Entity.Null)
            state.EntityManager.SetComponentData(conn, new CommandTarget { targetEntity = newControlled });
    }
}
```
---

#### 五、与 `IInputComponentData` 的协作（更“免样板”的输入链）

如改用 `IInputComponentData`，代码生成会自动完成“**采集输入 → 拷贝到命令缓冲 → 在预测/服务器侧按当前 Tick 应用**”。此时 **CommandTarget 仍然定义“这条连接把命令写到谁身上”**；只是“把组件数据拷到命令缓冲”的环节被自动化了。

#### 六、常见误区与纠偏

- **只生成了玩家，却没设置 CommandTarget**  
    结果：服务器收到命令包，却找不到目标缓冲，输入“丢在地上”。修正：在**同一条接入链**里设置 `CommandTarget` 并 `InGame`。
    
- **以为 Auto 模式能覆盖所有情况**  
    Auto 依赖“拥有者 + 预测/自有预测 + 启用标记”，且不覆盖薄客户端；需要动态路由或压测时，保留手动改 `targetEntity` 的能力。
    
- **把命令缓冲只加在一端**  
    命令缓冲既要存在于**客户端**，也要存在于**服务器**；运行时动态添加时，需确保两端都有该缓冲。
    
- **仍在插值Ghost上尝试自动命令**  
    插值Ghost不会被自动设置为命令目标；需要控制的对象应走 Predicted/OwnerPredicted。
    
- **搞混旧名**  
    早期的 `CommandTargetComponent` 已弃用，统一使用 `CommandTarget`。
    

#### 七、总结

> **CommandTarget 是“每条连接的命令路由表”**：连接实体有且只有一个 `targetEntity` 指针，决定本连接的命令写到谁（客户端）/分发给谁（服务器）。
> 
> 不使用自动方案时，必须把它指向一个**拥有至少一种 `ICommandData`** 的实体；该实体通常也是本连接“拥有”的`Ghost`。
> 
> 自动方案在“拥有者 + 预测态 + 启用”时可省去手工设置，但面对薄客户端与动态切换控制对象，显式改写 `targetEntity` 更稳。
> 
> 把 **“设置 CommandTarget → 允许 InGame”** 放在同一条接入链中，可以避免输入未被处理的空窗。