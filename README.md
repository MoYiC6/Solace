# Solace

Solace 的 C++20 核心重建、局部更新工具与配置工具。当前公开的是可维护的恢复源码；**完整 Solace 尚未恢复**。

## 项目内容

| 目录 | 内容 |
| --- | --- |
| [cpp-reconstruction](cpp-reconstruction/README.md) | 配置读写、资源表、HLSL 编译、实体登记状态机、Lua 5.4.7 运行时与 C ABI |
| [source-update-kit](source-update-kit/README.md) | 版本绑定的实体初次登记修补源码及机器码模拟测试 |
| [config-tools](config-tools/README.md) | 从完整基线生成独立配置的 Python 工具与参数计划 |

`cpp-reconstruction` 构建生成 `SolaceCore.dll`、静态库和 `solace-tool.exe`。`SolaceCore.dll` 使用本工程定义的 C ABI；原 DLL 的完整菜单、游戏宿主接线及全部功能仍未恢复。各模块的实现范围见对应 README 和 `docs/`。

## 构建 C++ 核心

需要 Windows x64、Visual Studio 2022 或 2026 的 Desktop C++ 工具与 Windows SDK、CMake 3.25+、Python 3.10+。Visual Studio 2026 需要支持其生成器的 CMake。

```powershell
git clone https://github.com/MoYiC6/Solace.git
Set-Location Solace

# 首次构建：从本地样本准备资源，不加载或执行 DLL。
python .\cpp-reconstruction\tools\prepare_resources.py --dll 'C:\path\to\Solace.dll'

# 配置、编译并运行 CTest。
.\cpp-reconstruction\build.ps1
```

资源清单绑定的原始样本 SHA-256：

```text
8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b
```

公开仓库保留资源的偏移、长度和 SHA-256 清单。原 DLL 及提取出的 146 份文本、5 个字体、26 张图片未随仓库分发。准备工具检查输入样本与每项资源；版本或内容不匹配会直接报错。资源准备成功后，常规 C++ 构建使用随附的 Lua 和 nlohmann/json 源码。

## 验证

```powershell
# 资源准备工具的边界与错误处理测试，无需原 DLL。
python -m unittest discover -s .\cpp-reconstruction\tests -p test_prepare_resources.py

# C++ 的 14 组 CTest 随 build.ps1 执行，需要先准备资源。
.\cpp-reconstruction\build.ps1
```

局部更新工具还需要匹配版本的 `client.dll`，构建命令和独立机器码模拟的范围见 [source-update-kit](source-update-kit/README.md)。配置工具需要用户自行保存的完整基线。构建和离线测试结果不代表游戏运行验收。

## 发布范围与许可

本仓库发布原创实现、测试及其数值样本、构建脚本、资源清单和技术文档。工作目录中的运行日志、个人配置、原始反编译证据、IDA 数据库、二进制产物、工具缓存以及上游仓库副本保留在本地。文档中提及的这些历史调查材料不属于公开发布内容。

原创代码采用 [MIT License](LICENSE)。随附的 Lua、nlohmann/json 保留各自许可，其他依赖及资源范围见 [第三方说明](THIRD_PARTY_NOTICES.md)。资源准备步骤不会为原始样本或提取内容授予新的许可。
