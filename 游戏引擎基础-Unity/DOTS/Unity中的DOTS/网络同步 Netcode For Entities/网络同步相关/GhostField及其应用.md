#### 一、概念与定位

**`GhostField`** 是对组件字段进行“是否参与网络复制、以何种精度与平滑方式复制”的标注机制；

当某个 `IComponentData`/`IBufferElementData` 至少有一个字段标注了 `GhostField`，NetCode 会为该组件自动生成快照序列化代码，并将其纳入 Ghost 快照流。

该机制将“复制粒度”下沉到字段级，使状态同步可在带宽、精度与视觉稳定之间做可控取舍。

#### 二、序列化与增量压缩的衔接

`Ghost` 快照以 `Tick` 为单位发送，序列化器按字段写入数据，并与 0–3 个基线快照做差分（delta），再进行打包传输；`GhostField` 决定该字段是否进入快照，以及进入后的量化与平滑策略，最终影响差分效果与带宽。

该流程的关键在“基线选择 + 字段级差分”的组合，能在丢包与乱序条件下保持较高还原率与稳定带宽。

#### 三、字段级控制项（核心属性与语义）

**量化（`Quantization`）**：对浮点与部分支持类型启用定点化，以精度换带宽；值被乘以量化系数并压为整数再传输，整数类型不适用。高频与噪声数据（如位置、速度）通常通过量化显著降低比特数。

**平滑（`Smoothing`）**：为插值链路定义重建策略，常用取值包括 `Clamp`、`Interpolate`、`InterpolateAndExtrapolate`。插值用于在两帧快照之间平滑过渡，外推在缺少下一帧数据时按上一段速度线性外推（外推时长受限）。

**最大平滑距离（`MaxSmoothingDistance`）**：当相邻快照差值超过阈值时禁用插值，直接使用最新值，用于吸收“瞬移/传送”这类大跳变而避免拖尾。

**合成位（`Composite`）**：对“结构体字段”启用“合并变化位”策略，即用一位标记“该结构体是否整体变化”，而非逐字段位图，适合把强耦合的小结构视作一个原子单元来压缩。

**子类型（`SubType`）**：为特定字段选择特殊序列化规则或自定义类型模板的变种，便于在同一组件内对同类字段应用不同的复制策略。

#### 四、与“类型模板 / 组件变体”的关系

`Ghost` 类型模板与组件变体允许替换“默认序列化器”，并可在“每个 `Ghost`、每个组件”的粒度上生效。需要注意，某些自定义类型的注册与匹配会对 `GhostField` 的属性组合“逐字匹配”；注册何种组合，运行时就只能使用对应组合。

该设计确保生成代码与运行时序列化路径一一对应，避免隐式退化。

#### 五、与预测/插值链路的协同

`GhostField` 的平滑策略直接作用于插值链路：插值或外推发生在“上一帧快照→下一帧快照”的可见过渡上；当出现位置大跳变时，以 `MaxSmoothingDistance` 切断插值避免拖影。

对预测链路，字段量化与差分影响回滚后的重建误差与重放成本；量化越强，带宽越省，但回放时的“细节还原”越粗，通常需结合物理或控制层设计容忍度进行权衡。

#### 六、带宽与性能的取舍逻辑

字段级量化、合成位与“仅需字段才复制”的选择性，是快照压缩的主要杠杆；在大量 Ghost 的场景中，通过提高量化、对小结构使用 `Composite`、以及只标注“对远端可见且必要”的字段，可显著降低序列化比特数和解包成本。新版本也提供了诸如“单基线压缩”等进一步的 CPU/带宽优化开关，但根本仍取决于 `GhostField` 的字段选型与参数。

#### 七、边界与易错点

**整数不支持量化**；浮点量化需关注取值**范围与步长**，否则可能在回放端放大离散误差。

**`InterpolateAndExtrapolate`** 仅对**插值链路**有效，不参与预测逻辑的“真正模拟”，过度外推会在包延迟较长时引入视觉相位误差，应配合外推时长限制。

对结构体字段启用 **`Composite`** 会牺牲“按子字段差分”的精细度，适合“强耦合、要么全变要么不变”的场景，不适合频繁单子段变化的结构。

针对自定义类型模板的属性组合匹配必须严格一致，否则不会命中自定义序列化器。

#### 八、客户端实践

**示例：位置与旋转的带宽—观感平衡**

```csharp
using Unity.NetCode;
using Unity.Mathematics;
using Unity.Entities;

public struct GhostMovement : IComponentData
{
    // 位置：毫米级量化 + 插值
    [GhostField(Quantization = 1000, Smoothing = SmoothingAction.Interpolate)]
    public float3 Position;

    // 朝向：角度量化 + 大跳变禁用插值
    [GhostField(Quantization = 512, Smoothing = SmoothingAction.Interpolate, MaxSmoothingDistance = 0.2f)]
    public quaternion Rotation;

    // 速度：合成位对齐压缩（作为小结构整体变化）
    [GhostField(Composite = true, Quantization = 1000)]
    public float3 Velocity;
}
```

该示例展示了三类常见权衡：位置用较高量化配合插值以压缩带宽；旋转在小角度内插值，在大跳变时禁用平滑以避免拖尾；速度视作原子结构体，使用合成位减少变化位图开销。

#### 九、结论

GhostField 将网络复制的控制下沉到字段层，决定“哪些字段被复制、以何种精度传输、在客户端如何重建与平滑”。该机制通过量化、平滑、合成位与子类型策略，与快照的基线差分压缩形成互补关系，在多人实时场景中以可度量的方式平衡带宽、CPU 开销与视觉稳定性，并为预测与插值两条链路提供一致的字段级时间序列输入。([Unity 手册](https://docs.unity3d.com/Packages/com.unity.netcode%401.0/api/Unity.NetCode.GhostFieldAttribute.html?utm_source=chatgpt.com "Class GhostFieldAttribute | Netcode for Entities | 1.0.17"))