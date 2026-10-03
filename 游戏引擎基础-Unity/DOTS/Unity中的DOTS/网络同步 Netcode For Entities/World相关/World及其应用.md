#### 一、`World` 是什么？以及与 NetCode 的关系

在 Entities 中，**`World`** 是一个“独立的 ECS 容器”，内部持有自己的 **`EntityManager`** 和一组 **`Systems`**；实体 ID 只在各自的 `World` 内唯一，系统只能访问同一 `World` 中的实体与组件。进入 `Play` 模式时，默认会创建一个 `World` 并将系统注入其中（也可自定义引导创建与插入到 `PlayerLoop`）

**NetCode 的做法**是在同一进程内**拆分多个 `World`** 来分离网络角色：至少有 **`Client World`** 与 **`Server World`**（还可以有**薄客户端 `World`**），从而把客户端/服务器的逻辑、更新节奏和系统组各自隔离开来。

#### 二、默认与自定义引导（`Bootstrap`）

安装 NetCode 后，会注入一个**默认 `Bootstrap`**：**启动时自动创建** `Client`/`Server`（及可选 `Thin Client`）等 `World`，并按标注把相应系统放进去；

独立构建时通常希望延迟或自定义这些世界的创建，此时可**继承 `ClientServerBootstrap` 并覆写 `Initialize`**，使用其帮助方法按需创建 `local/client/server/thin-client` 世界与连接流程。

[Scripting API](https://docs.unity3d.com/Packages/com.unity.netcode%401.0/api/Unity.NetCode.ClientServerBootstrap.html?utm_source=chatgpt.com)也明确指出 `ClientServerBootstrap` 的职责就是**配置并创建**服务器与客户端 World，支持自动连接等常见流程。

> 代码示意（节选，来源于文档思想）：
```
public class ExampleBootstrap : ClientServerBootstrap
{
    public override bool Initialize(string defaultWorldName)
    {
        // 仅创建“本地仿真”世界；需要时再手动创建客户端/服务器世界
        CreateLocalWorld(defaultWorldName);
        return true;
    }
}

```
> 作用：禁用默认“即刻创建客户端/服务器世界”的行为，转为按菜单/流程时机创建。

#### 三、系统如何被“放进正确的 `World`”

NetCode提供两条主路，**二选一或混用**：

1. **按系统组（`SystemGroup`）隐式过滤**：把系统声明在只存在于某类 `World` 的系统组下，该系统就只会在该类 `World` 被创建并更新。例如声明到 **`GhostInputSystemGroup`**（仅存在于客户端相关的世界），那么系统只出现在客户端/薄客户端 `World` 中；并且 **`PresentationSystemGroup` 仅存在于客户端 `World`**，因此凡属表现层系统只在客户端世界创建与运行。
    
    `[UpdateInGroup(typeof(GhostInputSystemGroup))] public partial class MyInputSystem : SystemBase { /* ... */ }`
    
1. **显式使用 `WorldSystemFilter`**：通过 **`[WorldSystemFilter]`** 指定系统应属于哪些 `World` 类型（枚举值包括本地仿真、服务器仿真、客户端仿真、薄客户端仿真），从而在**编译期**就决定系统落点与实例数量。
    
    `[WorldSystemFilter(WorldSystemFilterFlags.ClientSimulation)] public partial class MyClientOnlySystem : SystemBase { /* ... */ }`
    
    这些标志与 `World` 上的 **`WorldFlags`** 配合使用，用来区分与筛选世界类型。

> 小结：**系统归属 = 组过滤 + 标志过滤**。表现层只在客户端世界；需要显式声明的跨端逻辑，使用 `WorldSystemFilter` 精准约束。
> 
#### 四、时间步进：服务器固定步 & 客户端动态步（含预测）

NetCode 规定**服务器世界**以**固定时间步**更新（默认 60Hz，可通过服务端世界的 `ClientServerTickRate` 单例配置仿真频率与网络发送频率），并提供**每帧最多步数**、**批处理步长**等参数避免“螺旋式卡死”。  

**客户端世界**整体以**动态时间步**运行，但**预测代码**在 `PredictedSimulationSystemGroup` 中按照**与服务器相同的固定 Tick**执行；相同的 `ClientServerTickRate` 在握手阶段从服务器下发到客户端，使预测步长与服务器对齐。 

这一机制直接影响**插值/预测/回滚**等客户端体验，应当把与画面同步的显示系统放在客户端世界的 `PresentationSystemGroup`，把预测系统按要求放进 `PredictedSimulationSystemGroup`，从而保证数值与视觉两条线各自稳定。

#### 五、薄客户端（Thin Client）世界

薄客户端世界用于**本机压力/流程测试**：在编辑器或构建中可以同时运行若干个“极简客户端世界”，它们**不渲染、只做必要逻辑**，通常仅生成随机输入并参与联机；

只有显式标注了 `WorldSystemFilterFlags.ThinClientSimulation` 的系统才会在薄客户端世界运行。官方还提供了“在编辑器并行跑多个薄客户端”的[使用说明与注意事项](https://docs.unity3d.com/Packages/com.unity.netcode%401.5/manual/thin-clients.html?utm_source=chatgpt.com)，强调其对 CPU 负载的影响和测试定位。

#### 六、世界迁移（World migration）

在需要**销毁当前世界并迁移到新世界**而不丢失网络连接时，可使用 **`DriverMigrationSystem`** 保存并恢复传输层状态：先在旧世界存储驱动票据、释放旧世界，再在新世界加载票据并创建新的服务端或客户端世界，使连接继续沿用（注意调用顺序与前置条件）。  

这在“从登录/菜单世界切换到游戏世界、或关卡重载”时尤其有用，避免重新建链导致的闪断。

#### 七、客户端实践

- **世界划分与职责**：将**输入、预测、插值、命令发送/接收**等**客户端专属系统**放入 **Client World**；将**状态权威、快照/命令流、回滚/验证**放入 **Server World**。表现层系统依附 `PresentationSystemGroup`，天然只出现在客户端世界，避免服务器端渲染支出。
    
- **系统标注策略**：对明确只在某端运行的系统使用 `WorldSystemFilter`；对“属于某端特有系统组”的系统使用 `UpdateInGroup` 即可“继承世界过滤”。两者结合可减少多余实例与无效更新。
    
- **Tick 与链路**：预测相关逻辑进入 `PredictedSimulationSystemGroup`，把“渲染桥接/摄像机/UI”留在 `PresentationSystemGroup`，从而**固定步的数值**与**动态步的表现**各行其是，保持客户端帧感受与服务器 Tick 对齐。
    
- **引导与流程**：通过自定义 `ClientServerBootstrap` 控制**何时**创建世界与**如何**连接（例如先进入菜单/登录，再按用户选择创建 `client`/`server`/`thin-client` 世界并连接），避免“进程一启动就建全套世界”的硬耦合。
    
- **测试与压测**：在同一进程启用多份薄客户端世界，配合自动输入与连接，快速得到“真实网络”条件下的客户端行为数据；必要时在构建版中同样创建薄客户端进行长时间 soak 测试。

#### 八、常见误区与纠偏

- **“表现系统在服务器上也会跑吗？”** 不会。`PresentationSystemGroup` 不会创建在 `Server`/`Thin Client` 世界，因此表现层系统只出现在客户端世界。
    
- **“系统默认在哪个世界创建？”** 在 NetCode 下，默认 `Bootstrap` 会**在两端世界各创建一份**（前提是系统属于 `SimulationSystemGroup` 且未做过滤）；需要单端运行时用分组或 `WorldSystemFilter` 约束。
    
- **“服务器卡慢会怎样？”** 服务器以固定 `Tick` 更新；若帧长超标，下一帧可能执行多步来追平（可用 `ClientServerTickRate` 的最大步数与批处理参数做上限控制），防止“螺旋式恶化”。
    
- **“切世界一定要断线吗？”** 不一定。`DriverMigrationSystem` 支持**在世界销毁/重建间迁移传输层**，平滑过渡。
    
- **“独立/专用服务器构建怎么区分？”** 项目设置中的 DOTS 选项与平台目标会设置 `UNITY_SERVER`、`UNITY_CLIENT` 等定义与烘焙过滤，区分 **`Dedicated Server`** 与 **`Client`/`Server`** 构建类型。
    

#### 九、参考要点

- **World 的职责与默认创建**：`World` = 实体集合 + `EntityManager` + `Systems`；默认进入 Play 创建；可自定义 `Bootstrap` 与 `PlayerLoop` 插入。
    
- **NetCode 的多 World 结构与系统分配**：客户端/服务器/薄客户端世界；按系统组或 `WorldSystemFilter` 落位；表现组仅客户端世界存在。
    
- **引导与自动化**：默认 `Bootstrap` 自动建世界；`ClientServerBootstrap` 提供扩展点与工具方法。
    
- **时序与 Tick**：服务器固定步、客户端动态步 + 预测固定步；`ClientServerTickRate` 配置仿真与网络频率。
    
- **薄客户端与迁移**：`Thin Client` 世界的用途与限制；`DriverMigrationSystem` 迁移连接以平滑切换世界。
    
- **默认世界注入系统工具**：`DefaultWorldInitialization.AddSystemsToRootLevelSystemGroups` 可用于手动装配默认三大组（Init/Sim/Presentation）。
    

---

#### 总结

> **`World` 是 NetCode 的“角色隔离边界”与“时间步配置单元”。** NetCode 在同进程内创建 **`Server`/`Client`（及可选 `Thin Client`）** 等多个 `World`，以分离权威与表现、固定与动态时间步、以及系统组的存在性。
> 
> 系统的归属通过**系统组隐式过滤**与 **`WorldSystemFilter` 显式标注**决定；表现层只存在于**客户端世界**。
> 
> 服务器世界以**固定 Tick**更新（`ClientServerTickRate` 可控），客户端世界整体为**动态步**但在 `PredictedSimulationSystemGroup` 里执行**与服务器对齐的预测固定步**。
> 
> 需要切换场景或流程时，`DriverMigrationSystem` 可在**销毁旧世界并创建新世界**的过程中保留网络连接状态。
> 
> 默认 `Bootstrap` 会自动创建并配置这些世界，也可通过继承 `ClientServerBootstrap` 在项目流程中**按需与按时机**创建世界与连接。