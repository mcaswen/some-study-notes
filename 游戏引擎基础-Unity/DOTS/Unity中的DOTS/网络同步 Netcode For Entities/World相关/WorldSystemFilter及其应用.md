#### 一、WorldSystemFilter 是什么？

`[WorldSystemFilter]` 是一个用于**标注系统应当创建并更新于哪些 World 类型**的特性（`attribute`）。在 NetCode 的多 `World` 架构中（至少有 `Client World` 和 `Server World`；还可启用 `Thin Client`），可以用它在**编译期**声明系统只属于客户端仿真、服务器仿真、或薄客户端仿真等。官方手册给出四类核心标志：`LocalSimulation`、`ServerSimulation`、`ClientSimulation`、`ThinClientSimulation`，并给出直接示例（如下）。

> `WorldSystemFilterFlags.ClientSimulation` 表示“此系统只在可运行**客户端仿真**的 `World` 中创建”，即该 `World` 带有相应的 `WorldFlags`（例如 `GameClient`）。

#### 二、与系统组（`SystemGroup`）的交互：**组内继承**与“表现只在客户端”

`WorldSystemFilter` 并不是唯一的归属手段。**将系统放进只存在于某类 `World` 的系统组**，即可“隐式继承”该 `World` 过滤：

- 示例中把系统声明到 `GhostInputSystemGroup`，由于该组**仅存在于客户端相关的 `World`**，该系统只会在客户端侧被创建（包含普通 `Client` 与 `Thin Client`）。
    
- `PresentationSystemGroup` **只在客户端 `World`** 创建，因此所有表现层系统天然不会出现在服务器/薄客户端世界中。  
    当需要更精确的控制或不依赖组结构时，再使用 `WorldSystemFilter` 做**显式标注。


#### 三、默认行为与“`Default`”的展开规则

在 NetCode 的默认引导下，系统会被创建到 `SimulationSystemGroup`，并且**默认在客户端与服务器两个 `World` 都会创建**；只有当**通过组或 `WorldSystemFilter` 做了过滤**，才会限制到某一端。

进一步地，`WorldSystemFilterFlags.Default` 并非固定含义。**当系统标注为 `Default` 时，最终会根据其所属系统组的 `ChildDefaultFilterFlags` 展开**；若系统不显式归属任何组，则落到 `SimulationSystemGroup` 并继承该组的默认子过滤设置——这是 Entities 在 1.0 中的明确规则说明。  

对于**系统组本身**，`WorldSystemFilterAttribute` 的构造函数允许指定第二个参数 `childDefaultFlags`，即“若子系统未显式声明过滤，就按这里的默认值创建”。该参数仅对**系统组**有效，普通系统指定它不会产生影响。

#### 四、`Thin Client` 的特别之处

`Thin Client` 世界是用于编辑器或本地压测的**极简客户端**：不渲染、尽量少逻辑，仅做联机与输入生成。只有**显式标记**了 `WorldSystemFilterFlags.ThinClientSimulation` 的系统才会在薄客户端世界更新；否则不会创建，避免在压力测试时浪费 CPU。

[官方页面](https://docs.unity3d.com/Packages/com.unity.netcode%401.0/manual/client-server-worlds.html)也提供了 `World.IsThinClient()` 的使用提示，便于在 `MonoBehaviour` 环节提早返回。

#### 五、与 `World` 标志（`WorldFlags`）的关系

World 在创建时带有一组 `WorldFlags`，用于框定其“角色与特性”，而 `WorldSystemFilter` 正是基于这些标志完成“属于/不属于”的筛选与更新。[NetCode 文档](https://docs.unity3d.com/Packages/com.unity.netcode%401.0/manual/client-server-worlds.html)在同一页明确给出上下文：**`World` 被打了特定的 `WorldFlags`，Entities 便可据此做过滤/更新逻辑**。 

补充：`WorldSystemFilterFlags` 的枚举定义中还包含 `Presentation`、`LocalSimulation` 等其它场景标志，[官方](https://docs.unity.cn/Packages/com.unity.entities%401.0/api/Unity.Entities.WorldSystemFilterFlags.html)对各字段目的与“Default 展开”也有详细注释，可作为理解过滤决策的底层参考。

#### 六、客户端实践

##### 1）只在“客户端仿真”创建：输入/预测/UI 桥接

将输入采集、预测相关逻辑限定在客户端仿真世界，避免服务器冗余：

```
[WorldSystemFilter(WorldSystemFilterFlags.ClientSimulation)]
[UpdateInGroup(typeof(GhostInputSystemGroup))] // 组过滤与显式过滤并行使用，更清晰
public partial class CollectInputSystem : SystemBase { /* ... */ }
```

- 组过滤会让系统**只随客户端世界**创建与更新；`WorldSystemFilter` 进一步把归属写死在类型上，避免误入服务器世界。
    

##### 2）只在“服务器仿真”创建：权威校验/生成销毁

将权威决策、刷怪/清理等放在服务器仿真世界：
```
[WorldSystemFilter(WorldSystemFilterFlags.ServerSimulation)]
public partial class ServerAuthoritySystem : SystemBase { /* ... */ }
```
- 默认情况下系统会在两端创建；此标注将其限定到**服务器世界**，避免客户端重复执行。

##### 3）自定义系统组：**统一默认子过滤**

为客户端表现层定义一组“默认只在客户端世界创建”的系统组，子系统不写过滤也能继承：
```
// 指定本组自身只在客户端仿真世界创建；并将“子系统的默认落点”也设为客户端仿真
[WorldSystemFilter(WorldSystemFilterFlags.ClientSimulation,
                   childDefaultFlags: WorldSystemFilterFlags.ClientSimulation)]
public partial class ClientVisualGroup : ComponentSystemGroup { }

// 子系统不写 WorldSystemFilter 也会随组只在客户端世界创建
[UpdateInGroup(typeof(ClientVisualGroup))]
public partial class NameplateSyncSystem : SystemBase { /* ... */ }
```
- 上述第二参数 `childDefaultFlags` 的语义，来自 Entities API 的构造函数文档；仅对**组**有效，用于“未显式声明过滤的子系统默认落点”。

- 这类封装能减少“每个系统都要重复标注”的样板代码，同时保持创建语义一致。
    
##### 4）薄客户端专用系统：**只做必要逻辑**
```
[WorldSystemFilter(WorldSystemFilterFlags.ThinClientSimulation)]
public partial class ThinClientInputStubSystem : SystemBase { /* 生成随机输入/转发命令 */ }
```
- 薄客户端仅运行**显式标注**了 `ThinClientSimulation` 的系统，且不创建表现组，能显著降低压测时的 CPU 负担[Unity 文档](https://docs.unity3d.com/Packages/com.unity.netcode%401.0/manual/client-server-worlds.html)。
    

#### 七、默认引导（Bootstrap）与时机控制（与过滤的配合）

导入 NetCode 后，项目会获得一个**默认引导**：启动即自动创建客户端/服务器 World，并按前述标注把系统放入相应世界。

若项目需要延迟或按流程创建（例如先停在登录/菜单，再决定创建哪些世界），可继承 `ClientServerBootstrap` 并覆写 `Initialize`，使用其辅助方法**只创建所需世界**，再由过滤规则决定系统实际落点。

#### 八、常见误区与纠偏

- **“明明只想在客户端跑，为什么服务器也创建了一个系统实例？”**  
    未做过滤时，系统默认在客户端与服务器世界都创建；应通过**系统组继承**或 `WorldSystemFilter` **显式约束**归属。
    
- **“表现相关系统会不会跑在服务器上？”**  
    不会。`PresentationSystemGroup` **只存在于客户端世界**，因此这类系统不会在服务器/薄客户端创建。
    
- **“给系统标了 Default 就万事大吉？”**  
    `Default` 会根据所属组的 `ChildDefaultFilterFlags` **被展开**，可能与预期不一致。对跨端归属敏感的系统，建议直接给出**明确 Flags**，或通过“自定义系统组 + 子默认过滤”把语义固定下来。
    
- **“Thin Client 压测为何出现多余逻辑？”**  
    仅有 `ThinClientSimulation` 标记的系统会在薄客户端创建；其余系统不会参与。若出现额外开销，多半是忘记显式标注或通过组间接纳入了非必要系统。
    

---

#### 九、总结

> **WorldSystemFilter 是多 World 架构下“系统归属”的编译期合同。** 
> 
> 在 NetCode 中，它与**系统组的继承过滤**共同决定一个系统在哪些 World 中被创建与更新；客户端表现层天然只在客户端世界，服务器权威只在服务器世界。
> 
> `Default` 会按“所属组的子默认过滤”展开，必要时通过**自定义组 + childDefaultFlags**把默认落点固定。
> 
> 薄客户端仅运行显式标注的最小系统集合，用于高效压测。
> 
> 默认引导会在启动时自动建世界，也可通过自定义 `Bootstrap` 按场景/流程创建世界，过滤规则确保系统落点清晰且稳定。