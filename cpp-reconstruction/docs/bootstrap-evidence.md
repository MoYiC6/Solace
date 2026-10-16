# 实体初次登记：C++20 恢复记录

## 恢复范围

`include/solace/bootstrap.hpp` 和 `src/bootstrap.cpp` 把现有 `source-update-kit/src/entity_bootstrap.asm` 的**一次性登记状态机**翻译为标准 C++20。该模块可以由源码独立编译、测试，不读取原版 DLL，不包含任何地址、机器码、进程访问或补丁操作。

恢复对象是此前新增的登记逻辑。实体查找、实体分类与存储、成员检查、帧处理和上下文同步仍是显式 `Host` 接口，必须由实际宿主提供。测试使用内存容器作为宿主验证状态流，未交付游戏宿主适配器。此模块不构成完整 Solace、完整 ESP 或其余菜单、移动、Lua 功能的恢复。

原 ASM 文件保持原样。本文记录的来源 SHA-256：

| 来源 | SHA-256 |
|---|---|
| `source-update-kit/src/entity_bootstrap.asm` | `6d76a34ec92e886d98b2376e3b50476b7713aed868831382ad258ab441d35e2c` |
| `source-update-kit/tests/validate_bootstrap.py` | `4a65c892195ed36fdff183f6b1fd38b357e5452a66cbf9de79bf20606eaeff84` |
| `source-update-kit/profiles/solace-build14191.json` | `5a46121525d1d51e7dc64ef11f5a9ccc7d2245d8631ada1e45be2b938bb54a41` |

上述路径相对工作区根目录。

## 逐项语义对应

| ASM 行为 | C++ 实现与边界 |
|---|---|
| 首先调用 `original_frame`，保留其整数返回值 | `Host::original_frame(stage)` 始终先执行一次；正常返回路径原样返回 `uint64_t`，包括所有早退及已终止状态 |
| 帧阶段等于 12、状态为 0 | 默认 `Config.ready_stage=12`，仅 `Status::waiting` 可以启动 |
| `loading_gate & 1` 为零、`frame_ready & 1` 非零 | `ready_mask=1`，按位测试而非要求字段严格等于 0 或 1 |
| 外层和内层实体系统指针分别排除 0 / -1 | 两级 `Host` 查询返回不透明整数，分别排除 0 和 `UINT64_MAX`；外层无效时不查询内层，不解引用任何值 |
| 共享锁下读取 controller / pawn / observer | 显式 acquire / read / release；退出锁作用域后再验证 controller 非零及 `pawn | observer_pawn` 非零 |
| `lock cmpxchg`：0 → 1 | `compare_exchange_strong` 选择唯一登记调用；失败调用直接保留原函数返回值 |
| 保存此次 controller | 保存上下文快照的 controller，后续成员检查不重新获取 controller |
| 枚举索引 0 到 `0x3fff` | 默认循环上限 `0x4000` 不含上界；查找结果非零才登记，参数是实体和枚举索引 |
| 每次非零查找结果在 `entity_add` 前增加计数 | 诊断 `nonzero_entity_count` 表示登记调用尝试次数，不把它冒称成功插入数 |
| 扫描结束记录枚举上限 | `enumerated_index_limit=0x4000`，若扫描因 C++ 异常中断则尚未写入该完成标记 |
| 检查保存的本地 controller 是否为缓存成员 | `Host::contains` 的真实结果决定状态 2 / 3；没有根据缓存非空或调用成功推断登记成功 |
| 成功/失败后不自动重试 | 状态 2 和 3 均为终止态，没有重试、缓存为空重放或自动重置入口 |

状态值固定为：`waiting=0`、`started=1`、`complete=2`、`missing_controller=3`。C++ 诊断数据是类型化 API，**没有声称与原 ASM 的内存偏移布局兼容**。

索引 0 的细节：ASM 自身确实调用索引 0 的查找；原 DLL 的查找函数拒绝该索引。C++ 状态机保留该调用，测试宿主实现原查找的零索引排除。`lookup_entity`、`add_entity` 和 `contains` 的完整内部算法并未由此恢复。

## C++ 边界中的新增约定

这些规则用于定义 C++ API 的行为，不把它们归为已证明的 ASM 行为：

- 配置构造验证索引上限位于 `[1, 0x4000]`，就绪掩码非零。默认值严格对应现有版本配置。拒绝越界配置，不静默改值。
- 宿主异常向调用方传播，不转换为成功或自动重试。取得状态 1 之后若宿主抛异常，状态保留 1 及已写入的诊断，表示此次登记未完成；调用方仍能收到异常。
- 上下文读取异常通过 RAII 释放已取得的共享锁。`release_context_shared` 必须遵守 `noexcept` 契约。
- 终止状态通过 release 发布，诊断通过 acquire 读取；在 `started` 状态下读取的字段是逐字段进度，并非一次事务快照。宿主必须自行保证对象寿命、线程安全及接口约定，状态机不把这些责任隐藏起来。
- 并发与重入调用仍各自调用原帧函数；只有一个调用获得登记所有权。类没有重置或地图生命周期接口，因为 ASM 本身没有该接口。

## 验证

2026-10-16 使用本机 MSVC `19.51.36260.0`、Windows SDK `10.0.26100.0`，以 `/std:c++20 /EHsc /W4 /MD` 直接编译 `src/bootstrap.cpp` 与 `tests/bootstrap_tests.cpp`，编译无警告，运行结果 **27 / 27 通过**。独立检查产物位于 `build/bootstrap-only/bootstrap_tests.exe`。该程序只执行标准 C++ 状态机与内存宿主测试。

测试文件是独立 `main`，使用始终启用的检查函数；即使 Release 定义 `NDEBUG`，检查也不会消失。主工程可将其单独加入 CTest。

现有 `source-update-kit/tests/validation-current.json` 记录的 **34 项旧模拟**，实际上是以下 17 个场景在 `0x180000000` 和 `0x7f0000000000` 两个基址各执行一次。该旧结果本次只读核对，未重新运行机器码模拟；它与新 C++ 测试是两套证据。

| 对照旧模拟的 17 个场景 | C++ 测试 |
|---|---|
| 非目标帧继续等待 | `non_stage12_waits` |
| 加载中等待，就绪后登记 | `loading_waits_then_ready` |
| 无效上下文等待 | `invalid_context_waits_then_ready` |
| 帧就绪门禁 | `frame_ready_gate` |
| 实体系统为空等待 | `null_entity_system_waits` |
| 稀疏实体与块边界、只登记一次 | `sparse_table_chunk_boundaries_once` |
| 已有部分缓存仍执行初次登记 | `nonempty_cache_still_bootstraps` |
| 观察原帧函数更新后的状态 | `original_frame_stage_runs_before_ready_check` |
| observer 可提供有效上下文 | `observer_can_supply_valid_context` |
| 缓存非空但 controller 缺失为状态 3 | `membership_false_nonempty_cache_is_status3` |
| 成功状态和诊断标记 | `success_diagnostic_markers` |
| pawn 存在仍要求 controller | `controller_required_even_with_pawn` |
| controller 存在仍要求 pawn 或 observer | `pawn_or_observer_required_even_with_controller` |
| 外层/内层 0 和 -1 哨兵值 | `null_and_minus_one_system_pointer_guards` |
| 零索引查找规则与 `0x4000` 排除 | `index0_and_0x4000_excluded` |
| 登记拒绝后为状态 3 且不重试 | `on_add_rejection_reports_status3_once` |
| 状态 1 排除重复启动 | `in_progress_status_prevents_duplicate_start`，由真实重入触发 |

另有 10 项验证：位掩码、空扫描终止、捕获 controller 后变化、终止帧仅调用原函数、原函数异常、共享锁异常释放、登记异常保留进度、成员检查异常、配置验证、8 个线程同时越过就绪门禁后只有一次完整登记。

新测试保留了原函数参数/返回值、早退状态、登记顺序、索引边界、锁范围和诊断行为的检查。它不验证原机器码的寄存器、栈对齐、重定位、Win64 unwind 或 ABI，也没有实际运行原 DLL 内部的实体查找/成员检查实现。这些内容仍属于旧模拟及后续宿主集成的独立验证范围。
