# Solace 局部更新源码包

这个目录提供**可修改、可重复构建的 ESP 初始实体登记修补源码**。实际新增逻辑在 `src/entity_bootstrap.asm`，构建器在 `src/build.py`。构建时读取原版 `Solace.dll`，生成独立的 `dist/Solace.esp-bootstrap.dll`。

**完整 Solace C++ 源码尚未恢复。** 菜单、移动、ESP 绘制、Lua 等原有功能仍来自输入 DLL；本工程只提供这次新增逻辑和 PE 修补过程的源码，依赖原 DLL。此前使用的 `upstream/velocity` 仅为本地参照材料，存在已确认的实现差异，且未见仓库级许可证，未随公开仓库分发。

公开仓库包含更新器源码、版本配置和模拟测试；原版 `Solace.dll`、游戏 `client.dll`、构建产物、Python wheel、个人配置及本地分析记录均不分发。构建前需自行提供与版本配置匹配的两个 DLL。下文 `analysis/`、`updates/` 路径用于标识历史本地调查记录，不是公开仓库附带文件。第三方来源和许可见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

## 先构建一次

在 PowerShell 中从仓库根目录进入本目录，然后运行；将 `<游戏目录>` 换成实际安装目录，并将原版 DLL 放在仓库根目录或通过 `-OriginalDll` 指定其他位置：

```powershell
Set-Location .\source-update-kit

.\build.ps1 -OriginalDll ../Solace.dll -ClientDll '<游戏目录>\game\csgo\bin\win64\client.dll'
```

默认版本配置为 `profiles/solace-build14191.json`，输出为 `dist/Solace.esp-bootstrap.dll`，默认运行 **34 个离线机器码模拟场景**。构建流程操作文件，不会自动把产物加载进游戏。模拟通过也不等于真实对局中的功能验收。

需要 **Windows x64、64 位 Python 和访问 PyPI 的网络连接**；当前实测 Python 版本为 **3.13.15**。`build.ps1` 会在本目录创建并复用 `.venv`，从官方 PyPI 安装固定版本 wheel；使用 `--only-binary=:all: --require-hashes` 按 `requirements.lock` 校验下载包，不需要系统 Python 预先装好这些库。`pip --isolated` 忽略用户级 pip 配置和环境变量，安装源明确为 `https://pypi.org/simple`。网络、版本、平台或哈希校验失败会直接终止构建。

锁文件绑定 Windows x64 的 wheel 哈希；本仓库不随附 wheel，也不在安装失败时切换镜像、版本或源码包。

固定发行版本为 pefile `2024.8.26`、Capstone `5.0.9`、keystone-engine `0.9.2`、Unicorn `2.1.4`，分别记录在 `requirements.txt` 与带哈希的 `requirements.lock` 中。

默认通过命令 `python` 创建环境；可追加 `-Python '实际的64位python.exe完整路径'` 指定解释器。该参数用于首次创建 `.venv`；已有 `.venv` 时脚本继续使用其中的 Python，不会因改传 `-Python` 自动更换环境。

自定义版本配置和输出位置：

```powershell
.\build.ps1 -OriginalDll ../Solace.dll -ClientDll '<游戏目录>\game\csgo\bin\win64\client.dll' -Profile profiles/solace-build14191.json -Output dist/Solace.esp-bootstrap.dll
```

`-SkipTests` 只跳过这次构建后的模拟验证；它不表示跳过版本兼容性要求，也不应把该次产物记为“测试通过”：

```powershell
.\build.ps1 -OriginalDll ../Solace.dll -ClientDll '<游戏目录>\game\csgo\bin\win64\client.dll' -Profile profiles/solace-build14191.json -Output dist/Solace.esp-bootstrap.dll -SkipTests
```

也可以直接调用 Python 构建器。下面的 `python` 应使用本项目 `.venv` 的解释器；已运行过 `build.ps1` 后，可直接把它替换为 `.\.venv\Scripts\python.exe`：

```powershell
python src/build.py --input ../Solace.dll --client '<游戏目录>\game\csgo\bin\win64\client.dll' --profile profiles/solace-build14191.json --output dist/Solace.esp-bootstrap.dll
```

Python CLI 是文件构建入口，不负责创建环境或自动运行模拟；需要默认的依赖准备和构建后模拟流程时，使用不带 `-SkipTests` 的 `build.ps1`。输入始终使用对应配置绑定的原版 DLL，不把前一次修补后的产物作为下一次输入。

默认生成 `dist/Solace.esp-bootstrap.dll`、同名 `.manifest.json`、`.disasm.txt`；默认模拟另写 `.validation.json`。manifest 记录输入/输出哈希、配置与 ASM 哈希及依赖版本，validation 记录模拟结果。修改逻辑后若同名 DLL 已存在且内容不同，构建器会要求另选 `-Output`，以保留旧候选。

## 每个文件负责什么

| 文件 | 作用 | 修改时需要关注 |
|---|---|---|
| `src/entity_bootstrap.asm` | 本次新增的实际 x64 逻辑 | 门禁、一次性状态、实体枚举、原函数转发、寄存器与栈约定 |
| `profiles/solace-build14191.json` | 绑定目标原 DLL 和当前游戏版本；保存 Solace 内部 RVA、参数和 PE 布局约束 | 哈希、RVA、原始指令约束和版本关系必须一起核验 |
| `src/build.py` | 读取配置、汇编逻辑并处理 PE 文件，生成独立 DLL 及构建证据 | 节布局、地址修正、回调连接、异常展开和文件校验 |
| `build.ps1` | 创建/复用项目 `.venv`、从 PyPI 安装锁定依赖、构建并默认运行测试 | 参数、64 位 Python 环境、网络、测试结果和输出目录 |
| `requirements.txt`、`requirements.lock` | 固定依赖版本与 Windows x64 wheel 哈希 | 升级依赖需同步版本、锁文件与验证结果 |
| `tests/validate_bootstrap.py` | 独立运行机器码模拟场景 | 真实指令、函数桩边界、状态及 ABI 预期 |
| `dist/` | 构建产物 | 用当前构建输出及其证据确定版本，不凭文件名判断兼容 |

**`profile.symbols` 全部是 Solace 模块内部 RVA，不是 cs2-dumper 输出的 `client.dll` 偏移。** 例如 `original_frame=0x2381B0` 是 Solace 的 handler，`entity_system_slot=0x8CA358` 是 Solace 内部保存地址的槽；后者不是游戏实体系统全局在 `client.dll` 中的 RVA。cs2-dumper 输出用于核对游戏侧依赖，不能直接覆盖这些符号。

RVA 相对于指定模块基址，进程绝对地址则会随加载而变化。不要把某次运行中的绝对地址直接抄入配置。

ASM 中的宏由 `src/build.py` 在汇编前展开：

| 宏形式 | 含义 | 源文件中的例子 |
|---|---|---|
| `@rva:name@` | 模块内调用目标 RVA | `call @rva:original_frame@` |
| `@mem:name@` | 按符号生成 RIP 相对内存操作数 | `mov rax, qword ptr @mem:entity_system_slot@` |
| `@value:name@` | 取 `profile.parameters` 中的整数参数 | `cmp ebx, @value:ready_stage@` |

`state_*` 内存符号由 `layout.state_rva + state_layout` 对应偏移生成。当前 `parameters` 的准确键名为 `ready_stage=12`、`entity_index_limit=0x4000`、`ready_mask=1`；枚举上限不包含 `0x4000`。这三个值也属于已验证逻辑的一部分，修改前需核对实际语义及测试边界。

## 这次逻辑修什么

本次原版在对局中加载时，内部人物缓存为空，导致人物 ESP 在成员检查处退出。保持原版加载后重进地图，缓存恢复为 31 条记录（玩家控制器 10、物品 21），本地控制器成员检查变为 true，用户确认骨骼和血条恢复。证据见 `../analysis/entity_bootstrap_comparison/README.md`。

新增逻辑在原 FrameStage handler 执行后，等待对应帧阶段、加载状态、实体系统与本地上下文就绪，进行一次已有实体登记。它复用原 DLL 的实体查找、分类、插入和成员检查；完成后记录结果。登记失败保留失败状态，不使用“缓存为空就永久重试”的循环掩盖主流程问题。

这次修改覆盖初始登记。原有新增、删除和地图生命周期逻辑仍需按实际运行条件验证。`draw_legs` 的两字节签名更新是另一个独立候选，见 `../updates/build_14191/README.md`；不能把人物 ESP 恢复或本工程模拟通过当作其运行验收。

## 要改逻辑，改哪里

1. 修改 `src/entity_bootstrap.asm` 中明确属于新增部分的逻辑，保留原 handler 的调用关系和 Win64 ABI。修改调用、栈空间或寄存器保存时，同步复核异常展开信息和构建器的相关假设。
2. 若依赖的内部函数、状态字段或补丁位置变化，更新对应版本配置的 `symbols`、`layout` 或 `parameters`。新增符号可通过对应宏引用；新增配置类别时同步修改 `src/build.py` 的读取与校验。修改 prologue 时还须同步 `unwind` 数据。
3. 默认构建并检查全部模拟结果。重点看非目标帧、加载未完成、空上下文、稀疏实体、边界索引、重复进入、登记失败、寄存器/栈保护等行为。改变这些行为时同步更新测试的预期与边界，而不是删除失败检查。
4. 最后单独记录实际运行验证：原版输入哈希、游戏版本、候选输出哈希、加载时机和观察结果。至少区分“先进入对局再加载”与“保持加载后切图”，并核对缓存登记和人物显示。

要修改原 DLL 中菜单布局、移动算法或原有 ESP 绘制代码，仍需继续恢复相应函数与资源的行为和接口。本工程的 ASM 只覆盖新增登记逻辑，修改它不会自动重建这些原有功能。

## CS2 更新之后怎么适配

当前配置针对引擎 build **14191**。本次调查的输入身份为：

| 输入 | SHA-256 |
|---|---|
| 原版 `Solace.dll` | `8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b` |
| 当时的 `client.dll` | `659940c1db362ed981d1df10637cec8c2a3d99409b77fc3b73e435e4649e36b0` |

游戏更新后，按以下顺序核对：

1. **保留旧配置和证据，新建版本配置。** 读取实际安装的游戏模块版本和 SHA-256，确认分析的是本机当前文件。
2. **重新采集本机地址。** 使用用户指定的 [a2x/cs2-dumper](https://github.com/a2x/cs2-dumper)，按该版本官方 README/CLI 对当前游戏采集 offsets、interfaces、schema；保存工具 commit、游戏 build、采集时间和原始输出。仓库自带的历史 `output/` 不等于本机当前结果。此次采集与来源见 `../analysis/current_cs2/README.md`。
3. **核对实际依赖链。** 检查原 DLL 的模式是否仍命中正确函数/全局、解引用层数、实体列表布局、回调参数和帧阶段含义。对照 schema 字段及类型名链。若原 DLL 本身换版，还须重新定位配置中的 Solace 内部 RVA、补丁连接点及相关原始指令。
4. **修改配置和必要逻辑，再构建。** `cs2-dumper` 的字段输出不会自动迁移这份 DLL 的全部签名、内部函数或调用约定。模式唯一命中也不证明语义与 ABI 一致。
5. **离线验证通过后做运行对照。** 保存新产物和证据，分别记录初始登记、后续实体事件及切图结果；只把实际验收的项目标为通过。

**只把配置里的游戏哈希换成新哈希，不构成兼容性更新。** 哈希用于绑定已核对版本；必须先证明所依赖的地址、结构和调用约定仍成立，再接受新版本。

## 当前交付边界

已有候选的 34 个离线模拟场景覆盖新增 wrapper 的控制流和 ABI；部分原函数以受控桩代替，不能据此证明完整分类、实际渲染、并发对象寿命或全部游戏功能。候选的最新运行状态以对应构建证据和实际验收记录为准，重新构建也不会自动获得运行验证。

完整功能恢复进度见 `../analysis/RECONSTRUCTION_STATUS.md`，调查依据见 `../analysis/INVESTIGATION.md`。本包交付的是这次局部更新的可维护源码与构建路径，不是全功能 Solace 源码工程。
