# 第三方来源与许可

项目根目录的 `LICENSE` 适用于本项目原创代码。第三方代码继续适用各自许可证；原始样本、提取资源及本地参照材料不由本项目重新授权。

## 随仓库分发的 C++ 依赖

| 组件 | 固定版本与官方来源 | 许可证及本地许可文件 |
|---|---|---|
| nlohmann/json | [3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) | MIT；[LICENSE.MIT](cpp-reconstruction/third_party/nlohmann/LICENSE.MIT)，Copyright (c) 2013–2025 Niels Lohmann |
| Lua | [5.4.7 官方源码](https://www.lua.org/ftp/lua-5.4.7.tar.gz) | MIT；[doc/readme.html 的 License 节](cpp-reconstruction/third_party/lua-5.4.7/doc/readme.html#license) 及 [src/lua.h](cpp-reconstruction/third_party/lua-5.4.7/src/lua.h)，Copyright (C) 1994–2024 Lua.org, PUC-Rio |

依赖下载来源、版本和 SHA-256 另见 [dependencies.json](cpp-reconstruction/third_party/dependencies.json)。原始许可证随代码保留；CMake 安装规则也安装 nlohmann/json 和 Lua 的许可文件。项目的 Windows UTF-8 文件桥位于独立的 `cpp-reconstruction/src/lua_utf8_io.c` 和 `.h`，通过编译设置接入 Lua。

Windows SDK / D3DCompiler 由使用者通过 Microsoft 开发工具安装，本仓库不分发该 SDK。

## 使用者自行安装的 Python 工具依赖

`source-update-kit/build.ps1` 从官方 PyPI 按 [requirements.lock](source-update-kit/requirements.lock) 的固定版本和 wheel 哈希安装以下组件。本仓库不分发 wheel、虚拟环境或这些组件的原生动态库。

| PyPI 包 | 固定版本 | 官方来源及许可说明 |
|---|---|---|
| pefile | 2024.8.26 | [官方源码与 MIT 许可证](https://github.com/erocarrera/pefile/blob/v2024.8.26/LICENSE) |
| capstone | 5.0.9 | [官方 BSD 许可证](https://github.com/capstone-engine/capstone/blob/5.0.9/LICENSE.TXT)；项目包含的其他上游部分仍以其各自声明为准 |
| keystone-engine | 0.9.2 | 原生引擎为 [GPLv2](https://github.com/keystone-engine/keystone/blob/0.9.2/COPYING)，另有 [EXCEPTIONS-CLIENT](https://github.com/keystone-engine/keystone/blob/0.9.2/EXCEPTIONS-CLIENT) 和上游商业许可选项；Python binding 的 BSD 元数据不代表整个原生引擎的许可 |
| unicorn | 2.1.4 | 原生引擎采用 [GPLv2](https://github.com/unicorn-engine/unicorn/blob/2.1.4/COPYING)；Python binding 的 BSD 元数据不代表整个原生引擎的许可，其他上游代码以随源声明为准 |

可选的资源重新扫描工具 `extract_binary_assets.py` 使用 [FontTools](https://github.com/fonttools/fonttools) 解析字体、[Pillow](https://github.com/python-pillow/Pillow) 解析图像。本仓库不分发其安装目录；实际安装版本和许可由使用者取得的软件包记录。常规资源准备工具 `prepare_resources.py` 使用 Python 标准库，不需要这两项依赖。

## 原始样本、提取数据和参照工程

原版 `Solace.dll`、游戏模块、由它们生成的修补 DLL、字体、图像、HLSL、JavaScript、SVG、材质等提取内容不随公开仓库分发，也不纳入项目根目录 `LICENSE` 的授权范围。资源清单中的偏移和哈希仅用于验证使用者自行提供的输入。

本地 `upstream/velocity` 参照工程未随仓库分发，未对其声明本项目许可。本地分析数据库、原始反编译记录、进程记录及个人配置也未分发。
