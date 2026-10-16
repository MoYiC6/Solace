# Solace C++ 源码重建

**目标是完整、可维护的 C++ 工程；目前交付的是已编译验证的恢复核心，完整 Solace 尚未恢复。**

本目录已有真实 `.cpp/.hpp` 实现、CMake/Visual Studio 工程、固定版本依赖和测试。构建产生 `SolaceCore.dll`、静态库和 `solace-tool.exe`。公开仓库仅包含资源清单，首次构建须从本地原 `Solace.dll` 准备资源；之后编译不访问原 DLL。原文件也用于可选的差分验证。

`SolaceCore.dll` 使用本工程公开的 C ABI，不是原 Solace 的替换 DLL。没有接通游戏宿主，也没有完整菜单、原有功能算法或完整 Lua 宿主。代码中的缺失接口不以空函数、默认成功或静默重试代替。

## 已落地的源码

| 模块 | 实现与验证 | 尚未覆盖 |
|---|---|---|
| 配置 | version 3 JSON 文档、字段键算法、类型 0–8、`k/m/l` 绑定、文件存取；5 份实际配置、每份 2,266 字段无损往返 | 全部设置/default 注册、原加载器重置阶段、类型 9 自定义对象、历史迁移、菜单接线 |
| 资源 | 146 份原文本 + 5 个 TTF + 26 个 PNG，共 177 项、3,081,129 bytes；构建前逐项校验 SHA-256并嵌入 C++ 表 | 控件/字体/图像映射、资源生命周期、外部粒子资源 |
| 着色器 | 12 份 HLSL 的 17 个 VS/PS 程序按已恢复入口、profile、编译标志编译；提供资源绑定及常量缓冲反射 | D3D11 宿主状态、纹理输入、实际绘制和视觉一致性 |
| 实体初次登记 | 原 ASM 更新逻辑转成 C++20 状态机；27 个状态/并发/失败场景 | 实体查找、分类、缓存存储、成员检查的实际 Host 实现；完整 ESP |
| Lua 数学与值类型 | 正式 Lua 5.4.7；12 个 math 名称、Vec2/Vec3 的全部已登记方法/运算、Color/Rect/bits，以及 utils 表 19 个接口 | WorldToScreen、GUI、实际绘制、纹理/字体/图层、实体/命令、ffi |
| Lua 回调与生命周期 | 六类队列的 Add/Remove/派发；print、Delay、执行超时；独立 Session 的加载/重载/卸载及 __shutdown | 游戏事件订阅、native command/event 对象、脚本设置持久化、文件监视、原游戏宿主的时钟与清理接线 |
| C ABI | 资源枚举、字段键、配置验证、独立 Lua 执行；调用方缓冲与错误传播 | 原 DLL ABI、原初始化与卸载链 |

数学表已实现原 13 个名称中的 12 个，保留原生 userdata 和原有方法/运算行为。各模块证据分别列出名称、地址和验证范围，不把接口数量换算为整体完成率。剪贴板接口只核对实现与注册，自动测试没有读取或修改用户剪贴板。

## 构建

环境：Windows x64、Visual Studio 的 Desktop C++ 工具与 Windows SDK、CMake、Python 3.10+。本机已使用 Visual Studio 2026 / MSVC 19.51 构建。Lua 和 JSON 依赖源文件随工程提供。

在本目录执行首次资源准备。工具只读取原 DLL，按两份固定清单校验并提取 177 项资源；输入哈希或任何资源不符都会报错。既有文件内容不同时拒绝覆盖。

```powershell
python .\tools\prepare_resources.py --dll 'C:\path\to\Solace.dll'
```

资源准备完成后，常规构建不下载软件、不访问原 DLL、不读游戏进程：

```powershell
.\build.ps1
```

脚本依次配置、编译并运行 CTest，任一步失败即终止。可用 `-Configuration Debug` 或 `-BuildDirectory <目录>` 指定构建配置。Visual Studio 可直接打开本目录，也可打开构建生成的解决方案。

主要产物在 `build/Release/`：

- `SolaceCore.dll` / `SolaceCore.lib`：本工程恢复核心及其导入库。
- `solace_core.lib` / `solace_lua.lib`：供 C++ 集成的静态库。
- `solace-tool.exe`：资源、配置、脚本和 shader 工具。

## 使用

```powershell
.\build\Release\solace-tool.exe assets
.\build\Release\solace-tool.exe key playerlist priority
.\build\Release\solace-tool.exe inspect-config '<配置文件路径>'
.\build\Release\solace-tool.exe roundtrip-config '<输入配置>' '<新的输出路径>'
.\build\Release\solace-tool.exe run-lua '<脚本路径>'
.\build\Release\solace-tool.exe script-session '<脚本目录>'
.\build\Release\solace-tool.exe compile-shaders '.\build\compiled-shaders'
```

`roundtrip-config` 拒绝已有输出路径。`run-lua` 将所选脚本的父目录显式作为文件/DB根，执行脚本并调用 `__shutdown`，输出捕获的 print 日志；它不驱动帧事件和 Delay 时钟。需要持续脚本会话时使用 `script-session`，通过 JSON 行显式发送 load/tick/dispatch/reload/unload/close，见 [会话协议](docs/cli-session.md)。嵌入式入口为 `solace::script::Session(root)` 或 `Runtime(root)`。未提供根时，文件/DB接口明确报错。

常用入口：

- `include/solace/config.hpp` / `src/config.cpp`
- `include/solace/assets.hpp` / `src/assets.cpp`
- `include/solace/bootstrap.hpp` / `src/bootstrap.cpp`
- `include/solace/script.hpp` / `src/script.cpp` / `src/script_utils.cpp`
- `include/solace/script_session.hpp` / `src/script_session.cpp`
- `src/script_math.cpp` / `src/script_draw_values.cpp` / `src/script_callbacks.cpp` / `src/script_services.cpp`
- `include/solace/shaders.hpp` / `src/shaders.cpp`
- `include/solace/api.h` / `src/api.cpp`

## 验证与证据

源码与已恢复行为的边界分别记录于 `docs/`。历史工作目录的 `BUILD_PROOF.json` 记录当时源码、产物与测试文件的哈希；该文件及原始 `evidence/`、`verification/` 不随公开仓库分发，也不作为后续修改的验证结果。

历史 Release 的 14 组 CTest 全部通过，覆盖数学/向量、draw 值类型、回调队列、脚本服务、Session、UTF-8 文件桥和 CLI 回归。CLI 覆盖 14 个场景、324 项检查，UTF-8 桥通过 46 项检查。配置另用 5 份原文件通过 62 项检查；Lua utils 通过 105 条断言及资源清理检查。当前源码请运行 `build.ps1` 复验，结果写入 `build/test-results.xml`。

新增数学模块通过 75 条 Lua 行为断言和 2,550 项原指令顺序参考等检查；颜色/矩形模块另用原 DLL 局部机器码得到的 4,202 项样本逐字节对照。六类回调队列通过 37 个独立场景。数学参考检查、机器码模拟与实际游戏运行是不同证据，各模块文档明确其边界：

- [数学与向量](docs/script-math-evidence.md)
- [颜色、矩形与位集合](docs/script-draw-values-evidence.md)
- [回调队列](docs/script-callbacks-evidence.md)
- [执行、延迟任务与生命周期](docs/script-runtime-evidence.md)
- [Windows UTF-8 文件与模块导入](docs/lua-utf8-io.md)

数学与 FNV 的独立验证执行原 DLL 的局部指令于 Unicorn，原 DLL没有被Windows加载。5,706个输入中5,673个与模拟器逐位相同；其余33个为NaN传播位差异，已用保持原指令顺序的独立本机SSE程序核实。报告保留差异，并未归一化或删除案例。复验需要已安装的 pefile/Unicorn：

```powershell
..\source-update-kit\.venv\Scripts\python.exe tools\verify_script_machine_code.py --dll ..\Solace.dll --tool build\Release\solace-tool.exe --native-probe build\Release\native_float_probe.exe --output evidence\script-machine-code.json
```

配置的显式绑定器采用先验证后应用；未知字段保留为 JSON。它尚未复刻原加载器对缺失字段的重置与默认值处理。

依照“错误直接暴露”的要求，`DbSave` 在既有 `storage.json` 损坏或非对象时明确失败，保留原文件；原版的静默清空覆盖行为没有照搬。具体差异及证据见 `docs/script-utils-evidence.md`。

## 完整恢复仍需完成

1. 完整设置注册与默认值、菜单布局/交互、热键与主题绑定。
2. 实体/渲染/输入/事件宿主，原有绘制、移动、外观等功能的算法和生命周期。
3. 剩余 Lua 接口、GUI/实际 draw 绑定、原生事件/命令对象、ffi、设置持久化与游戏宿主卸载接线。
4. Solace 独有的 pixelsurf、texturebug、天空/模型扩展、媒体歌词、皮肤界面扩展。
5. 实际宿主运行与切图/卸载对照，逐功能验收。

调查时使用的参照工程保留在本地 `../upstream/velocity`，不随本仓库发布。其配置介质、功能集合与资源链不同，还存在空手套回调，未被改名后当作完整 Solace 编译交付。本工程新增核心没有复制其 VAC 模块。

## 第三方来源

- [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0)，MIT；源文件和许可在 `third_party/nlohmann/`。
- [Lua 5.4.7 官方源码](https://www.lua.org/ftp/)，MIT；压缩包 SHA-256 `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`。
- HLSL 使用 Windows SDK 的 [D3DCompile](https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dcompile) 与 [D3DReflect](https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dreflect)。

原 DLL 文本、字体和图像属于本地样本的恢复数据。仓库仅保留偏移、哈希和格式验证清单，资源由使用者在本地准备；本项目的 MIT 许可不覆盖这些资源。详见 [第三方说明](../THIRD_PARTY_NOTICES.md)。
