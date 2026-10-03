
Unity 的 **实体组件系统（ECS）** 是支撑 DOTS 技术栈的数据导向架构：它让你在**内存中的数据布局**与**运行时调度**上拥有更强的控制与确定性。Unity 2022 LTS 版本为 ECS 提供了两套可兼容的物理引擎、一个高层的 Netcode 包，以及能把大量 ECS 数据送入 SRP（URP/HDRP）的渲染框架；并且它能与 **GameObject** 数据协同，让你继续复用暂未原生支持 ECS 的系统（动画、导航、输入、地形等）。

本章聚焦 DOTS 的关键特性，说明它们如何帮助你**规避前一章提到的 CPU 性能陷阱**。开始动手前，建议先看看官方 **EntityComponentSystemSamples** 仓库（带讲解与示例）。

# C# Job System（多线程基础）

**C# Job System** 提供一种更容易、更高效的方式来写多线程代码，让你的程序吃满目标平台的 CPU 核心。它不是单独的包，而是 **Unity 核心模块**的一部分。

传统 `MonoBehaviour` 的更新只在**主线程**执行，很多项目因此把绝大多数逻辑都塞在一个核心上。你当然可以自己管理线程，但既安全又高效地做这件事很难。Job System 则提供了**更省心**的路径：

- 维护一个**工作线程池**（通常为“总核心数 − 1”）；例如 8 核设备会有 1 个主线程 + 7 个工作线程。
    
- 工作线程从**作业队列**里拉取可执行的 **Job**；一旦开始执行，**直至完成**（不被抢占）。
    

一个最小 Job 示例（示意）：

```
// 把两个数组的元素逐一相乘 
struct MyJob : IJob 
{     
	public NativeArray<float> Input;
    public NativeArray<float> Output;     
    public void Execute() 
    {         
	    for (int i = 0; i < Input.Length; i++) {             
		    Output[i] *= Input[i];         
		}     
	} 
}

```
（要点：`NativeArray<T>` 属于**非托管**数据，可在 Job 中安全使用。）

## 调度与 Complete

- **只能在主线程**调用 `Schedule*()` 把 Job 加入队列；**不能**在 Job 内调度新 Job。
    
    Data-Oriented_Technology_Stack_…
    
- 主线程调用 `Complete()` 会**等待**该 Job 执行结束（若尚未完成）。**只有主线程**能调用 `Complete()`。
    
    Data-Oriented_Technology_Stack_…
    
- `Complete()` 返回后，Job 使用过的数据**重新安全**，可在主线程访问，也可安全地传给**后续**要调度的 Job。
    
    Data-Oriented_Technology_Stack_…
    

## 安全检查与依赖

- 多线程的核心是**避免数据竞争**与**内存破坏**。Job System 通过“**安全检查**”与“**依赖**”来兜底。要点如下：
    
- Job 默认有各自的**私有数据**，主线程与其他 Job 不可同时访问。若多个 Job **需要共享**同一份数据，不能并发执行，否则会有竞争；因此当你调度可能冲突的 Job 时，**安全检查会报错**。
    
- 调度 Job 时可**显式声明依赖**：只有当其**所有依赖 Job 完成**后，工作线程才会开始执行该 Job。例如 A、B 都访问同一数组，可令 **B 依赖 A**，保证 B 一定在 A 之后运行。调用 `Complete()` 也会**级联**完成它所有直接或间接依赖的 Job。
    
- 许多 Unity 内部系统也在用 Job System，所以 Profiler 里你会看到不止你自己调度的 Job。在 Job 里**不要做 I/O**（读写文件/网络等可能阻塞线程），若要并发 I/O，请在主线程调用异步 API 或用常规 C# 线程。