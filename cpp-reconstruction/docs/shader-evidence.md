# HLSL 编译链恢复

固定样本 SHA-256：`8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`。

12份文本不是由新代码仿写，均来自已核验的原DLL字节。源码引用及调用点通过本机IDA只读接口重新查询；结果在 `evidence/shaders/`。本机REA适配器返回 unreadable_output，因此本目录使用直接IDA证据，不将其标为REA Evidence。

| 原函数 | 资源 | 入口与目标 | Flags1 / Flags2 |
|---|---|---|---|
| `0x18018BAC0` | 全屏四边形 / motionblur | main / vs_5_0；main / ps_5_0 | 0 / 0 |
| `0x180226590` | 5份天空与风暴文本，数组 `0x1804BFFB0` | main / ps_5_0 | 2048 / 0 |
| `0x1802C4450`（含 `0x1802C4440` thunk） | glove capture、weapon composite、三个 scene | vs_main / vs_5_0；ps_main / ps_5_0 | 2048 / 0 |

调用方分别为 `0x180241E00`、`0x1802C8330`、`0x1802CFDB0`。所有路径没有源宏或include接口。2048为SDK的D3DCOMPILE_ENABLE_STRICTNESS。motionblur两个调用的长度包括终止NUL，重建保留该长度区别。

`src/shaders.cpp` 通过正式D3DCompile编译，任何错误向调用方传播。D3DReflect产生每个程序的资源寄存器、常量缓冲大小、变量偏移/大小/行列等数据。调试名称在本工程使用可追溯的资源相对路径，原版可能为nullptr或不同诊断标签；不宣称编译字节与原产物逐字节相同。

已通过17个shader stage的实际编译与反射，并验证不存在的入口被拒绝。此结果证明这些源码和编译契约可用；不证明D3D设备、纹理数据、材质绑定、渲染状态或最终视觉效果已经恢复。
