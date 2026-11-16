# 事件驱动简易 CPU 模拟器

此版本在 WSL/Linux 下使用 C++17 构建，使用全局离散事件队列驱动 CPU 与内存交互。

## 模型约定

- 地址为 32 位；数据内存按字节编址，每格 8 位，未初始化的数据读为 0。
- 寄存器文件有 32 个 32 位寄存器，两个读端口和一个写端口。
- 指令使用 `enum class Opcode` 与结构化字段表示，不编码成机器码字节。
- 指令和数据使用同一组 32 位地址编号，但分别保存在指令表和数据字节表；地址段不重叠。程序入口 `0x100`，每条指令占一个逻辑 8 字节槽位。
- Load 读取一个字节并零扩展到 32 位；Store 写寄存器的低 8 位；Add 是 32 位无符号加法。
- CPU 是顺序、阻塞、非流水的，最多有一个未完成的内存请求。
- CPU 向内存传请求的延迟为 2 ns；内存向 CPU 返回响应的延迟为 2 ns。内存内部服务时间为 0 ns，不模拟排队、Busy 状态或端口争用。
- Add 运算耗时 1 ns；取指、译码和控制逻辑没有额外模拟延迟。
- Memory 在写请求到达时更新数据；CPU 收到写确认后才认为 Store 完成。
- HALT 被 CPU 取指并处理后，在同一时间请求结束模拟。
- Tick 使用整数纳秒；事件按 `(时间, 插入顺序)` 排序。只有事件发生时才推进模拟时间。

这些参数是教学模型假设，不代表真实硬件延迟或 gem5 默认值。

## CPU 与 Memory 的事件连接

请求方向由 CPU 负责安排：`Cpu::issueRequest()` 通过 `Simulation::scheduleAfter(cpuToMemoryLatency, ...)` 把“调用 `Memory::receiveRequest()`”的动作加入全局事件队列。请求到达事件发生后，Memory 读取指令、读取数据或写入数据。

响应方向由 Memory 负责安排：Memory 在处理请求后，通过 `Simulation::scheduleAfter(memoryToCpuLatency, ...)` 把调用 CPU 响应回调的动作加入同一个全局队列。响应事件发生时，该回调进入 `Cpu::onMemoryResponse()`。

CPU 不直接推进全局时间；Memory 也不直接调用 CPU 的状态更新函数。请求和响应各自经过事件队列，因此两段通信延迟都能单独调整。

## 测试程序与时间

```text
Load r1, #0
Load r2, #1
Add r3, r1, r2
Store r3, #3
Halt
```

默认 `memory[0]=10`、`memory[1]=20`，期望 r3=30、memory[3]=30。

| 时间 | 事件 |
|---:|---|
| 0 ns | CPU 安排第一条取指请求到达 Memory |
| 2 ns | 请求到达 Memory，读取第一条指令 |
| 4 ns | CPU 收到第一条 Load，安排数据读取请求 |
| 8 ns | CPU 收到数据 10，写 r1，开始下一次取指 |
| 16 ns | CPU 收到数据 20，写 r2，开始取 Add |
| 20 ns | CPU 收到 Add，开始计算 |
| 21 ns | Add 完成，写 r3，开始取 Store |
| 25 ns | CPU 收到 Store，安排写请求到达 Memory |
| 27 ns | 写请求到达 Memory，更新 memory[3] |
| 29 ns | CPU 收到 Store 确认；业务指令完成 |
| 33 ns | CPU 取得并执行 Halt，模拟器退出 |

所以最后一条业务指令在 29 ns 完成；包含 Halt 取指在内的总模拟时间是 33 ns。

## 构建与运行

```sh
make
./simulator
./simulator 5 7
./simulator 255 1
```

两个可选参数分别设置 memory[0]、memory[1]，范围为 0 至 255。程序检查寄存器、内存、退出原因和关键时间；全部通过后输出 `Testbench checks: PASS`。

## 结构化指令的范围

本版本优先展示事件推进与请求／响应，因此指令以记录形式保存在独立的程序映射中，数据以字节映射保存。PC、地址编号和取指延迟已经独立于宿主指针。后续可以定义 opcode、寄存器编号、立即数的字节布局和字节序，再用字节取指和译码替换结构化指令表；无需改变事件队列的核心机制。
