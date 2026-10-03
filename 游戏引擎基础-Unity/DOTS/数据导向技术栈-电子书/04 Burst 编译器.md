前面提到，Unity 中的 C# 代码默认由 **Mono**（JIT，即时编译器）编译；另外也可选择 **IL2CPP**（AOT，预编译，一般在部分目标平台上有更好的运行时表现）。

**Burst** 包提供了第三种编译器，会进行幅度很大的优化，往往能显著超过 Mono，甚至在不少场景里也优于 IL2CPP；对于**重计算**问题，它能明显提升**性能与可扩展性**。

需要注意：Burst **只能**编译 **C# 的一个子集**。最主要的限制是：**Burst 编译的代码不能访问托管对象**（包括所有 **class** 实例）。因此，Burst 通常**只针对**代码中的某些区域生效，比如各类 **job**。

来自官方示例的一个量化对比：  
在 Jobs 教程中，使用 **Mono** 的 `FindNearest` 更新耗时 **342.9 ms**；改为 **Burst** 编译的 `FindNearestJob`，耗时仅 **1.4 ms**。

要让某段代码被 Burst 编译，可使用 `BurstCompile` 特性标记（示意）：

```
// BurstCompile 特性会让该 Job 由 Burst 编译 
[BurstCompile] 
struct MyJob : IJob {     
	public NativeArray<float> Input;     
	public NativeArray<float> Output;      
	public void Execute()     
	{         
		for (int i = 0; i < Input.Length; i++)         
		{             
			Output[i] *= Input[i];         
		}     
	} 
}

```
**为什么 Burst 快？**  
官方说明中给出的核心原因包括：利用 **SIMD**（单指令多数据，同时对多元素做相同运算）与更好的**别名分析**（aliasing awareness，用于判断不同引用/指针是否指向同一内存），以及其他多种底层优化技术。

进阶方面，Burst 还提供**内建指令（intrinsics）**与 **Burst Inspector**（可查看生成的汇编），便于进一步挖掘性能。