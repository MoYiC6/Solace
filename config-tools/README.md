# Solace HvH 参数

适用目标：用户提供的 Solace.dll，SHA-256 `8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b`。配置格式为 version 3。参数计划面向社区服务器 HvH，主打连狙 / AWP；以下按正常武器散布准备。

这是待实战调整的起始档，没有测得胜率或“最强”结论。字段名称、武器组和类型来自这份 DLL 本体；具体数值是本次调参选择。

公开仓库仅提供脚本和参数计划。下文提及的个人配置、运行日志及 `analysis/` 调查材料保留在本地，使用时请自行保存完整基线。

## 主要参数

| 武器 | hit chance | min damage | point scale | autoscope |
|---|---:|---:|---:|---|
| SCAR-20 / G3SG1 | 65% | 35 | 70% | 开 |
| AWP | 75% | 90 | 70% | 开 |
| SSG 08 鸟狙 | 70% | 70 | 75% | 开 |
| Desert Eagle 沙鹰 | 65% | 40 | 70% | 关 |
| General 通用 | 60% | 30 | 65% | 关 |

共同设置：enabled、silent、penetration、autostop 开启；maximum fov 为 180；hitboxes 选择 head / chest / stomach；autostop modes 选 early / between shots。no spread、force shoot、force shot aggressive、limiters 关闭。AA 开启，pitch=down，yaw=targets，jitter 关闭，avoid backstab 开启。

其他未验证的特殊模式和原有数字绑定元数据保留。未启用过的单武器配置按菜单的实际行为，先复制最近启用的父组参数，再写入上表的参数并启用 override general。没有把旧版同源项目的 doubletap、dynamic point scale 或不存在的 auto fire 字段写入。

## 生成完整配置

先在游戏菜单的 **configs** 页面保存当前配置，名称为 **hvh_base**。文件应出现在：

`%APPDATA%\solace_int\configs\hvh_base.json`

保存完整基线是必要步骤：这份 DLL 加载配置时会先清空未列出的开关和绑定。直接生成只含 rage 参数的 JSON，会影响现有 ESP、移动等功能。

在 PowerShell 运行：

```powershell
# 在仓库根目录执行
python .\config-tools\apply_hvh_profile.py
```

生成独立文件 `%APPDATA%\solace_int\configs\hvh_balanced.json`，并写出 `hvh_balanced.changes.txt` 逐项变更记录。原始 hvh_base 保留。缺失字段、类型不符或输出重名会直接报错。

在 **configs** 页面刷新列表，加载 **hvh_balanced**。无需重新注入 DLL。生成脚本本身不会加载配置或操作游戏进程。

`hvh-balanced.plan.json` 是构建用参数计划，**不能直接导入游戏**。实际游戏配置仅由完整基线生成。

## 调整依据

- 长时间不出枪：先核对已选中当前武器独立配置、override general 已启用、确有可穿透目标；再分别降低 min damage 或 hit chance，单次只改一个条件。
- 经常空枪：先确认实际已停稳、开镜和延迟表现，再将 hit chance 每次提高 5。提高阈值会减少允许开枪的情况，不等于修复命中算法。
- 穿墙伤害低于阈值时不射击是伤害门槛的作用。连狙可在 30–40、AWP 在 80–90 之间试调，比较实际伤害和等待时间。
- no spread / hvh mode / selftrack 等特殊功能未在当前服务器验证，不能由选项名称推断提升效果；此档不新增开启这些模式。

字段证据：`analysis/config_trace/rage_config_schema.json`、`rage_menu_1802ab7e0.json`、`rage_0x1802AF4C0.json`、`decompile_180004ed0.json`。脚本完整保留未修改字段和原文件的其他顶层数据。

## 配套操作版 hvh_paired

已基于实际的 `hvh_balanced.json` 另存 `hvh_paired.json`，只修改两个原本常开、没有绑定的选项：

- quick peek：按住 V 启用（key=86、mode=1）。
- duck peek：按住左 Alt 启用（key=164、mode=1）。

两个绑定均来自本体 `decompile_180001000.json` 中的默认初始化，而不是猜测按键。其余 2264 个字段与 balanced 相同，包含武器参数、AA、ESP、移动及已关闭的 `legitbot.enabled`。配置未在游戏中自动加载；需要在 configs 刷新并选择 hvh_paired。

复现命令（输出同名文件存在时会报错）：

```powershell
python .\config-tools\apply_hvh_profile.py --base "$env:APPDATA\solace_int\configs\hvh_balanced.json" --output "$env:APPDATA\solace_int\configs\hvh_paired.json" --plan .\config-tools\hvh-paired.plan.json
```

## AWP 与镜头调整档 hvh_awp_tuned

用户反馈：站稳、开镜、无遮挡目标可以开枪，主要在跳跃和穿墙时不开；同时希望自动开镜后画面不放大。以 hvh_paired 为基线，已生成 hvh_awp_tuned，只改 8 个字段：

- AWP hit chance：75 → 65。
- AWP min damage：90 → 70。穿墙损失后低于阈值的目标仍不满足条件。
- camera.custom fov 开，普通 fov=90。
- scoped fov override 开，scoped fov 与 scoped fov phase 2 均为90。
- removals.remove scope 开，取消原版镜框遮挡。

保留 autoscope 和其他2258个字段。它是门槛与显示调整，不代表修复或保证空中射击；降低门槛会接受较低估算命中率/伤害的射击。

证据：当前 PID12528 的日志在00:24:57记录 hc11%<75%而hold，随后到达75%时射击并记录实际伤害；00:25:02记录100%命中率的射击和实际108伤害。日志副本在 analysis/config_trace/hvh_awp_tuning_log_snapshot.json，仅作为 DLL 自身诊断，未独立验证其概率估算。air_0x180100060.json 证实比较估算命中率与门槛；in air 急停选项不是已确认的空中开火开关。

本体 camera_decompile_18015dfb0.json 使用普通/一级/二级 FOV 值，camera_decompile_18015c0f0.json 将结果写入视图FOV；与关掉武器 autoscope 是不同设置。配置文件已校验，画面效果和调整后射击表现仍需加载验证。

```powershell
python .\config-tools\apply_hvh_profile.py --base "$env:APPDATA\solace_int\configs\hvh_paired.json" --output "$env:APPDATA\solace_int\configs\hvh_awp_tuned.json" --plan .\config-tools\hvh-awp-tuned.plan.json
```

## 公开参数参考档 hvh_public_reference

用户确认应用 [gan15off 的 Neverlose CS2 HvH 指南](https://forum.neverlose.cc/t/guide-to-creating-your-own-cfg-ru-eng/587558) 中的一组参数。原帖发布于 2026-02-10，作者在 2026-06-27 回复已更新。取值为 AWP hit chance=70、min damage=100；SCAR-20 hit chance=60、min damage=35。两个武器均开启 autoscope，max fov=180。

以完整的 hvh_awp_tuned.json 为基线另存 hvh_public_reference.json，实际只改变 AWP 命中率、AWP 最低伤害及 SCAR-20 命中率三个字段；其余字段保留，包括 G3SG1、镜头 FOV、快捷键、ESP 和移动设置。原始文件保留。菜单 configs 刷新列表后加载 hvh_public_reference 即可，无需重新注入。

这是其他程序的公开参数参考，尚未在 Solace 实战验证；AWP 最低伤害提高到100会过滤低伤射击，并不解决跳射或穿墙不开枪。未将原帖 Force Shot、Move between Shots 等选项直接移植到语义未核实的 Solace 字段。

复现命令（输出已存在时会报错）：

```powershell
python .\config-tools\apply_hvh_profile.py --base "$env:APPDATA\solace_int\configs\hvh_awp_tuned.json" --output "$env:APPDATA\solace_int\configs\hvh_public_reference.json" --plan .\config-tools\hvh-public-reference.plan.json
```
