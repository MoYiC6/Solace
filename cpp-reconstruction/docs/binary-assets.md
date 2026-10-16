# 原 DLL 内嵌二进制资源恢复

从原版 `Solace.dll` 提取并严格验证了 **31 个独立资源，合计 2,743,833 字节**：5 个 TTF 字体（2,060,380 字节）和 26 个 PNG 图片（683,453 字节）。31 个 SHA-256 均不同；导出文件保持原字节，没有转换、修补或重新编码。

输入 SHA-256 为 `8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`，大小 9,121,280 字节，ImageBase 为 `0x180000000`。输入 DLL 只读，提取后再次验证哈希未变。没有访问 DLL 中的 URL，没有下载私有 `.rpak`，没有加载 DLL 或操作游戏。

## 文件

- [提取器](../tools/extract_binary_assets.py)：固定输入身份，扫描、验证、导出并回读对比。
- [构建清单](../resources/binary/manifest.json)：每项提供相对文件路径、证据 ID、类型、编码、字节长度、文件偏移、RVA、SHA-256 和完整格式验证信息。
- [完整扫描证据](../evidence/binary-assets.json)：包括原始签名候选数、拒绝原因、依赖版本、提取器哈希及各资源的详细验证结果。
- [资源目录](../resources/binary)：原字节 TTF / PNG 文件。

构建清单使用 `encoding="binary"`、`template=false`，没有伪造 NUL 前后界限。`counts={"png":26,"ttf":5}` 与 `formats={"PNG":26,"TTF":5}` 同时保留。资源表可以直接引用这些已落盘文件，常规 C++ 构建不需要再次读取原 DLL，也不需要执行资源提取器。

## 字体

| 字体内嵌名称 | 文件偏移 | RVA | 字节数 | 字形数 |
|---|---|---|---:|---:|
| Inter 18pt Regular | `0x006992D0` | `0x0069A2D0` | 342,680 | 2,926 |
| Inter 18pt Bold | `0x006ECD70` | `0x006EDD70` | 344,152 | 2,926 |
| Smallest Pixel-7 Regular | `0x00740DD0` | `0x00741DD0` | 25,600 | 316 |
| Inter 18pt SemiBold | `0x00748940` | `0x00749940` | 343,828 | 2,926 |
| Noto Sans Math Regular | `0x0079C860` | `0x0079D860` | 1,004,120 | 5,130 |

五个字体均位于 `.data`。名称来自各字体的 `name` 表，而不是根据上游代码猜测。名称表中的 copyright、license 和 license URL 原文保存在 JSON 验证信息中。

验证步骤包括 sfnt 表目录与搜索字段、标签和四字节对齐、表范围、无重叠、`head` 标记、所有表校验和以及整字体校验和。五个字体的整文件值均为 `0xB1B0AFBA`，`head` 表单独校验时清零 `checksumAdjustment` 字段；规则依据 [Microsoft OpenType 格式规范](https://learn.microsoft.com/en-us/typography/opentype/spec/otff)。

随后使用 fontTools `TTFont(checkChecksums=2, ignoreDecompileErrors=False, lazy=False)`，调用 `ensureDecompiled(recurse=True)` 解析全部表，验证 Unicode cmap，并用 Pillow / FreeType 在 24 像素字号栅格化 `Solace 0123`。记录非空字形掩码的边界和 SHA-256。解析接口依据 [fontTools TTFont 文档](https://fonttools.readthedocs.io/en/latest/ttLib/ttFont.html)。

这证明原 DLL 内已包含可解析、可使用的真实字体，填补此前纯文本资源提取的字体缺口。它不自动证明与上游 `rpak::fonts::inter::medium` 等资源 ID 一一对应；字体的具体使用点、字号与 UI 绑定仍需依据原调用点核对。

## PNG

26 个 PNG 均位于 `.rdata`，图片尺寸从 23 × 23 到 1000 × 100，也包含 400 × 889 图片。25 个为 RGBA，1 个为 RGB。文件名仅标识类型和来源偏移，没有凭外观假定它属于哪个菜单或控件。

提取器逐块读取到长度为零的 `IEND`，验证 `IHDR`、`IDAT`、块边界、块类型及每个块的 CRC32，然后对精确截取的文件调用 Pillow `verify()` 和重新打开后的 `load()`。PNG 尾部取到 `IEND` 的 CRC，不吞入后续 DLL 数据。清单记录宽高、模式、块信息、原文件 SHA-256 和完整解码像素的 SHA-256。

## 搜索与拒绝结果

| 原始签名 | 候选数 | 通过完整验证 |
|---|---:|---:|
| TrueType sfnt `00 01 00 00` | 7,267 | 5 |
| OpenType `OTTO` | 4 | 0 |
| Apple sfnt `true` | 82 | 0 |
| PNG | 26 | 26 |
| JPEG | 12 | 0 |
| BMP | 56 | 0 |
| ICO | 5,052 | 0 |
| `typ1`、TTC、WOFF、WOFF2、GIF87、GIF89、DDS、RIFF、OGG、FLAC | 各 0 | 各 0 |

常见短签名会命中指令或其他资源内部数据，只有签名不能算找到资源。未通过格式验证的候选没有导出为资产。GIF、DDS、TTC、WOFF、WOFF2、OGG 和 FLAC 在此次扫描中没有签名候选；当前提取器对这些格式只记录签名，不把未来出现的候选未经完整解码就导出。

扫描范围是这个已固定哈希的 PE 原始文件。没有执行解压、解密、运行时内存搜索或外部资源包还原，因此 31 不是整个应用所有二进制资源的总数。

## 复现和负例检查

本次使用 Python 3.13.15、fontTools 4.67.0、Pillow 12.3.0、Pillow 内的 FreeType 2.14.3。Pillow 原先已安装；fontTools 从 PyPI 固定版本安装到本工程 `tools/binary_asset_deps`，没有改系统环境。常规 C++ 构建只使用已提取文件，这些 Python 解析依赖只用于重新提取。

在项目根目录 `cpp-reconstruction` 运行：

```powershell
python -m pip install --no-deps --only-binary=:all: --target tools/binary_asset_deps fonttools==4.67.0 Pillow==12.3.0
python -X utf8 tools/extract_binary_assets.py
```

提取器默认读取上一级的原版 `Solace.dll`。输入哈希不匹配会立即失败；已有同名资源内容不同时也会失败，不覆盖该文件。成功清单由本次实际验证结果生成。

已额外在内存副本上执行 4 个负例：PNG CRC 破坏、PNG 截断、TTF 表字节破坏、TTF 截断，均被相应结构或校验和检查拒绝。另逐项核对 31 个导出文件与原 DLL 对应切片完全相同、资源范围互不重叠，并再次确认原 DLL 哈希未变。没有修改原 DLL 来制造负例。
