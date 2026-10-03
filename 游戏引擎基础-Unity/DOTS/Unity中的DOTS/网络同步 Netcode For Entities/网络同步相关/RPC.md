#### 一、概念与定位

**RPC（Remote Procedure Call）** 在 NetCode for Entities 中承担“低频、一次性、必须到达的事件传递”。

其本质是：在**可靠通道**上发送一条带数据的指令，远端在接收线程/系统内将其映射为一个临时实体（携带 `ReceiveRpcCommandRequest` 与具体 RPC 组件），随后由已生成的执行器将其落入游戏逻辑队列。

与 Ghost 快照这种“持续状态流”不同，RPC 面向“事件瞬时性”的语义：要么到达并被处理，要么就不算发生，因此走可靠管道且保序（每条连接内发送序的先后在接收端保持一致）。

#### 二、通道与顺序语义（为何“可靠”且如何“保序”）

RPC 使用专门的**可靠管道**发送，具备“必达”与“单连接内按发送顺序到达”的保证；但与快照属于**不同数据通道**，两者的相对到达顺序**不作保证**，因此不能假设“先发 RPC、后发快照”就一定先收到 RPC 再收到快照。

可靠管道存在**在途窗口**与重传机制，窗口满载会延迟后续 RPC 的发送，属于可靠带来的代价范畴。RPC 采用 dedicated reliable channel；可靠窗口有限；与快照无全局先后保证，但 RPC 在网络层“接收顺序 == 发送顺序”成立（按连接计）。

#### 三、发送与接收流程（从“构造消息”到“落到逻辑”）

**发送端**：定义一个实现 `IRpcCommand` 的 `struct`（可带字段），随后创建实体并添加该 RPC 组件与 `SendRpcCommandRequest`；`TargetConnection` 指定单播目标，不指定则在服务器端广播，客户端侧默认发往服务器。代码生成会把序列化、注册与排队（`OutgoingRpcDataStreamBuffer`）接好，`RpcSystem` 在仿真帧末尝试发送队列中的 RPC（若可靠缓冲未满）。

**接收端**：网络层解包后创建带 `ReceiveRpcCommandRequest` 的临时实体并附上 RPC 数据；随后由已生成的执行器或自定义 `IRpcCommandSerializer<T>.CompileExecute` 将其变成“可由游戏系统消费的请求”，典型做法是使用 `EntityCommandBuffer` 在合适的系统中处理并销毁该临时实体。

**排队替代**：也可直接使用 `RpcQueue` 从连接的 `OutgoingRpcDataStreamBuffer` 排队，避免反复做结构变化；两条路径底层等价，都是把数据写入可靠发送缓冲，由 `RpcSystem` 统一出包。

#### 四、序列化与执行（为何能跨 Job/线程安全地落地）

使用 `IRpcCommand` 的**代码生成**会产出对应的 `IRpcCommandSerializer<T>`，负责将值类型字段经 `DataStreamWriter/Reader` 写入/读取，并生成一个 `Burst` 可调用的 `FunctionPointer`，用于在接收侧以“参数包（reader, connection, ECB, jobIndex）”的形式执行。

执行函数通常不直接改游戏大状态，而是通过 ECB 写请求或标记，再在主逻辑系统里落地，从而保持 Job 化与线程安全的边界清晰。

#### 五、与 Ghost/命令的边界（为何“事件用 RPC，状态/输入不用 RPC”）

- **Ghost 快照**：面向“连续状态”；走不可靠通道 + 基线差分；频率高、带宽敏感、允许丢帧但通过插值/回滚收敛。
    
- **命令（`ICommandData` / `IInputComponentData`）**：面向“逐 Tick 输入”；走专用命令流；按 Tick 编号做预测/回滚。
    
- **RPC**：面向“低频一次性事件”（如加载关卡、请求进入大厅、确认开局、授予所有权、开局时的一次性配置等）；走可靠通道，**不能**替代高频状态同步或输入流，原因在于可靠窗口有限、队尾等待会引入额外时延，且消息必须**单包可容纳**。 

以上分工直接来自三种“时间语义”的差异：持续状态（`Ghost`）、逐 Tick 决策（命令）、一次性事件（RPC）。
    

#### 六、可靠性的代价与工程约束（原理层解释）

可靠管道需要**确认、重传与缓冲排序**，因此：

- **在途上限**：可靠消息存在队列窗口（in-flight）；窗口满载时，后续 RPC 无法及时注入，造成**头部阻塞**与**整体延迟增长**，这也是“RPC 不宜承载高频”的根因之一。
    
- **单包限制与 MTU**：默认必须**单包装载**，包含协议头后可用负载受 MTU 限制（通常 ~1400B）；如需承载更大数据，需自定义 Transport 管线（例如添加**分片阶段**或调整 MTU），但这属于高风险变更，需充分测试。
    
- **与快照相对顺序不保证**：可靠只是“同管道内保序”，而快照走不可靠通道，因此二者的**相对时序**无法假定；这解释了为何“用 RPC 驱动状态”和“用快照驱动状态”不能交叉依赖相对顺序，应通过**状态机/标志位**或**等到收到双方的条件**来拆偶。
    

#### 七、使用模式与可验证性（从客户端角度）

- **会话与关卡流**：从客户端向服务器发送“加入会话/准备就绪/加载完成”等一次性 RPC；服务器按连接实体记录进度并广播“开始游戏”RPC，客户端再切换世界或加载场景资源。该流程因可靠与保序，能确保“先 Ready 后 Start”的因果顺序在同一连接内成立。
    
- **所有权/稀疏控制事件**：例如切换 `CommandTarget`、授予/撤销某 `Ghost` 的 `Owner`，或发起一次性交互确认。事件不易用快照表达，且必须达成一致，适合 `RPC`。
    
- **带宽与延迟度量**：在弱网/高 RTT 时监控可靠队列深度与发送失败计数，避免 RPC 洪泛；对于需要“看起来即时”的高频操作，应回到 `Ghost`/命令通道。
    

#### 八、客户端实践

**示例：客户端发送一次性“Ready”RPC，服务器接收并回写状态**

```
using Unity.Burst;
using Unity.Entities;
using Unity.NetCode;

// 1) 定义 RPC（代码生成将产出序列化与请求系统）
public struct ClientReadyRpc : IRpcCommand
{
    public int LobbyId;      // 简化示例：入参
}

// 2) 客户端：发起请求（单播到服务器）
[WorldSystemFilter(WorldSystemFilterFlags.ClientSimulation)]
public partial struct SendClientReadySystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        // 条件满足时创建 RPC 实体 + 发送请求标记
        if (/*本地加载完成*/)
        {
            var e = state.EntityManager.CreateEntity();
            state.EntityManager.AddComponentData(e, new ClientReadyRpc { LobbyId = 42 });
            state.EntityManager.AddComponent<SendRpcCommandRequest>(e);
            // 不设 TargetConnection：在客户端默认发往服务器
        }
    }
}

// 3) 服务器：接收并落地逻辑（ReceiveRpcCommandRequest 出现即处理）
[WorldSystemFilter(WorldSystemFilterFlags.ServerSimulation)]
public partial struct ReceiveClientReadySystem : ISystem
{
    public void OnUpdate(ref SystemState state)
    {
        foreach (var (rpc, req, e) in 
                 SystemAPI.Query<RefRO<ClientReadyRpc>, RefRO<ReceiveRpcCommandRequest>>()
                          .WithEntityAccess())
        {
            // ……记录该连接 ready；条件达成后可广播“StartGame”RPC（略）
            state.EntityManager.DestroyEntity(e); // 用完即销毁临时实体
        }
    }
}

```
发送路径与接收路径均依赖代码生成的 `SendRpcCommandRequest/ReceiveRpcCommandRequest` 工作流与可靠通道，保证“必达与按连接保序”。

#### 九、总结

RPC 在 NetCode for Entities 中是“**一次性可靠事件**”的通道：**可靠且按连接保序**、**与快照/命令分域**、**必须单包可容纳**。其实现以 `IRpcCommand` 的代码生成为核心，统一处理序列化、队列与分发；可靠性来自 Transport 的可靠管线，代价是窗口上限、重传延时与 MTU 约束。

将“持续状态”交给 `Ghost`，“逐 `Tick` 决策”交给命令，把“偶发且必须达成一致的事件”交给 `RPC`，能在客户端侧获得更清晰的时间与因果分层，降低跨通道时序耦合与性能风险。[官方文档](https://docs.unity3d.com/Packages/com.unity.netcode%401.0/api/Unity.NetCode.IRpcCommand.html)