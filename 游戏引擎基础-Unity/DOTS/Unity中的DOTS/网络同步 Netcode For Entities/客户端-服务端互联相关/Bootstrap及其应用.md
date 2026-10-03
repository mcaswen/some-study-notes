#### 一、Bootstrap 在 NetCode 中的职责边界

NetCode 提供了 `Unity.NetCode.ClientServerBootstrap` 作为“开机引导”。该类在**游戏开始（或编辑器进 Play）** 时**配置并创建**客户端/服务器等 World，并提供“创建 `client/server/thin-client/local` 仿真世界”的工具方法；

还内置**自动连接/自动监听**能力（`AutoConnectPort`、`DefaultConnectAddress`、`DefaultListenAddress`）以便无需额外样板即可联上本地或指定地址的服务器。其本体实现了 `ICustomBootstrap` 接口，因此既可复用默认流程，也可完全接管启动逻辑。


#### 二、默认引导 vs. 自定义引导：何时切换、如何接管

将 NetCode 包加入工程后，**默认 `Bootstrap`** 会在启动时**自动创建** `Client/Server`（以及按需要的 `Thin Client`）等世界，并按系统的声明（组/过滤特性）把系统注入对应的 `World`。

这在**编辑器直接点 `Play`** 的迭代场景非常方便；而在**独立客户端/前端菜单**等项目中，更常见做法是**延迟或按流程创建**世界（同一可执行既能当客户端也能当服务器）。此时可**继承 `ClientServerBootstrap` 并覆写 `Initialize`**，用其工具方法按需创建 `client / server / thin-client / local` 世界。

**官方文档给出示例**：覆写 `Initialize` 并仅调用 `CreateLocalWorld(defaultWorldName)`，即可阻止自动建联机世界、只建本地仿真世界。

> **关键点**：`ICustomBootstrap.Initialize` **返回 true** 时，默认世界初始化**不再执行**；这意味着世界创建与系统注入完全由自定义引导负责。必要时可用 `DefaultWorldInitialization.GetAllSystems` + `AddSystemsToRootLevelSystemGroups` 自行拼装系统清单与三大组（`Init/Sim/Presentation`）。

#### 三、ClientServerBootstrap 的核心 API（面向工程落地）

- **世界创建工具**：  
    `CreateClientWorld(name)`、`CreateServerWorld(name)`、`CreateThinClientWorld()`、`CreateLocalWorld(name)`；以及用于“一键按配置创建”的 `CreateDefaultClientServerWorlds()`。这组 API 既可在 `Initialize` 里使用，也可在**运行时**按需加新世界（例如热插 N 个 thin client 做压测）。
    
- **自动连接/监听**：  
    通过 `AutoConnectPort` 开启自动连/听；`DefaultConnectAddress` 指定默认连接地址（编辑器下 `Multiplayer PlayMode Tools` 的地址优先生效），服务器侧可用 `DefaultListenAddress` 绑定监听地址与端口（云环境友好）。
    
    是否应自动监听/连接可用属性 `WillServerAutoListen` 等判断。
    
- **编辑器/构建形态联动**：  
    `RequestedPlayType` 反映“当前 `Play` 模式期望”（例如 `Client&Server`、仅 `Client` 等），`Initialize` 的默认实现会据此创建匹配的世界并在编辑器内批量创建 `Thin Clients`（最大 32 个，`k_MaxNumThinClients`）。
    

#### 四、系统“落位”的决定链：`Bootstrap` × 多 `World` × 系统清单

**答：**`Bootstrap` 负责**创建哪些世界**与**何时创建**；**系统是否在某个世界出现**由两层规则共同决定：

1. **系统组继承过滤**：若系统声明在只存在于某类 `World` 的系统组下（如 `GhostInputSystemGroup` 仅存在于客户端相关世界；`PresentationSystemGroup` 仅在客户端世界创建），该系统只会被注入到对应世界。
    
2. **`[WorldSystemFilter]` 显式过滤**：当需要更细粒度控制时，在系统上声明 `ClientSimulation/ServerSimulation/ThinClientSimulation/LocalSimulation`，编译期写死落位语义；最终匹配靠 `World` 的 `WorldFlags`。**默认**无过滤时，系统会在**客户端与服务器**的 `SimulationSystemGroup` 同时出现。
    

> **工程建议**：自定义 Bootstrap 时，先建世界，再用 `DefaultWorldInitialization.AddSystemsToRootLevelSystemGroups` 注入**经过筛选**的系统列表，确保“默认无过滤的系统”不会误入不该出现的世界。

#### 五、与联机流程的衔接：自启动联接与 UGS 集成

很多入门样例直接在自定义 bootstrap 里**启用自动连接**（设置 `AutoConnectPort`），其余交给默认 `Initialize` 即可；这是在编辑器中做“本进程 Client ↔ Server 联调”的最小配置。官方教程 _Networked Cube_ 就演示了该写法。

与此同时，若工程接入 **Unity Multiplayer Services/Relay/Lobby** 等，更通用的指引是：**先创建 NetCode 的 Client/Server 世界**，默认 `Bootstrap` 会自动完成；高级用法可通过自定义网络处理器绕过默认集成，按会话生命周期驱动世界创建与连接。

#### 六、时间步与引导的关系（只点到即止）

引导阶段完成后，**服务器世界**按**固定步**运行、**客户端预测**在 `PredictedSimulationSystemGroup` 里按与服务器一致的固定步执行；这些策略由 **`ClientServerTickRate`** 控制并在握手时下发到客户端。`Bootstrap` 负责把世界与系统准备好，后续时序就交给系统组与 Tick 配置。

#### 七、客户端实践

- **单一入口引导**：用一个继承自 `ClientServerBootstrap` 的类型作为**唯一入口**。在 `Initialize` 中读取配置/命令行/UI 选择，**决定建哪几类世界**；必要时仅建 `CreateLocalWorld` 进入“单机/菜单态”，待玩家选择后再建联机世界并连接。
    
- **系统清单显式化**：使用 `DefaultWorldInitialization.GetAllSystems` 获取候选清单，配合**组与 `WorldSystemFilter`** 做白/黑名单过滤，再用 `AddSystemsToRootLevelSystemGroups` 注入，保证“不同构建/运行模式”下系统出现的集合**可控且可复现**。
    
- **编辑器联调与压测**：通过 PlayMode 配置或在 `Initialize` 内多次调用 `CreateThinClientWorld()` 批量创建薄客户端，利用 `AutoConnectPort` 直连本地 Server，进行输入/同步压力验证。

#### 八、常见误区与纠偏

- **“覆写了 `Initialize` 却仍然自动建世界”**：`ICustomBootstrap.Initialize` 必须**返回 true**才会**阻止默认 world 初始化**。返回 false 意味着还要执行默认引导，通常会产生重复世界。
    
- **“系统落位与预期不符”**：默认无过滤的系统会被加入 Client/Server 两端；应使用**系统组继承**或 **`WorldSystemFilter`** 明确约束，或在自定义引导中**显式筛掉**不该注入的系统类型。
    
- **“编辑器地址与代码地址冲突”**：编辑器下 `Multiplayer PlayMode Tools`的地址**优先于** `DefaultConnectAddress`；若该地址无效才回退到代码设置。联机异常时先检查该窗口配置。
    
- **“只想单机运行却有 NetCode 系统开销”**：在自定义引导里调用 `CreateLocalWorld`，该世界**不会注入 NetCode 系统**，适合作为菜单/剧情/单机场景的运行容器。

#### 九、总结

> **Bootstrap 是 NetCode 的“进程内世界/系统编排器”。** 
> 
> 默认 `Bootstrap` 会在启动时建立 `Client/Server/Thin-Client` 等世界并注入系统；而自定义引导通过覆写 `Initialize` 与调用 `Create*World` 系列方法，能够精确控制**何时建世界、建哪些世界、注入哪些系统、是否自动连/听**。
> 
> 系统最终落位由**系统组继承**与 **`WorldSystemFilter`** 决定，自定义引导则是把 **“容器与时机”** 这层把住，确保客户端项目在编辑器联调、独立构建、菜单分流、压测等多种形态下都能拥有**清晰、稳定且可复现**的启动与联机流程。