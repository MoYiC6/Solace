# 离线机器码验证

`validate_bootstrap.py` 使用 Unicorn 执行候选 DLL 的实际 wrapper、实体按索引查询和缓存成员查询指令。原 FrameStage、SRW API 和实体登记函数使用可控桩：桩主动改写易失寄存器与 32 字节 shadow space，从而检查调用约定和寄存器保存。测试不会打开游戏进程，也不会由 Windows 加载 DLL。

在项目根目录运行 `build.ps1` 时会自动执行验证。已构建后也可单独运行：

```powershell
.\.venv\Scripts\python.exe tests\validate_bootstrap.py --dll dist\Solace.esp-bootstrap.dll --manifest dist\Solace.esp-bootstrap.manifest.json --output dist\Solace.esp-bootstrap.validation.json
```

若自行管理 Python 环境，先按项目 `requirements.txt` 安装依赖，再使用该环境的 `python` 执行同一命令。脚本从普通 Python 包导入 `pefile` 和 `unicorn`；项目的构建器另需 `capstone` 和 `keystone-engine`。脚本不含工作区依赖目录，不需要其他分析文件。

输入清单必须包含 `output_sha256`、`wrapper_rva`、`symbols`、`state_rva`、`state_layout` 和 `statuses`。地址及状态值由清单解析，DLL 的 SHA-256 必须匹配清单。测试固定的是此次修补的语义：stage 12、最多遍历 0x4000 个索引、现有实体列表与缓存布局。未来更换布局时，需要同时更新夹具及预期结果。

17 种场景分别在 PE 首选基址和不同重定位基址执行，共 34 项：

1. 非 stage 12 等待。
2. 加载期间等待，准备完成后执行。
3. 上下文无效时等待，恢复后执行。
4. `frame_ready` 门禁。
5. 空实体系统等待。
6. 稀疏实体表、chunk 边界和一次性执行。
7. 已有缓存仍补录其余实体。
8. 原 FrameStage 先执行，再检查准备状态。
9. observer pawn 满足上下文要求。
10. controller 未进入缓存时记录失败状态。
11. 成功状态、实体数量、遍历上限和 controller 诊断值。
12. controller 必须存在。
13. pawn 或 observer pawn 必须存在。
14. 两层实体系统指针的 0 / -1 哨兵。
15. 索引 0 和 0x4000 排除边界。
16. 登记被拒绝时记录失败并停止重复尝试。
17. 已开始状态阻止重复启动。

所有调用同时验证 RAX 返回值、栈平衡、Win64 栈对齐、非易失通用寄存器及 XMM6–15、锁配对和参数传递。保留真实查询函数，因此实际索引运算与成员判断也会执行。

每次命令重新执行，生成带输入摘要和完成时间的 JSON。全部通过返回 0；输入问题或失败返回非零。开始执行前，旧输出先被标为 `validation-incomplete`，避免误把旧报告当新结果。使用 `python -O/-OO` 会被明确拒绝，因为它会禁用断言。输出路径须与两个输入路径不同。

这些结果验证修补控制流及 ABI；原 FrameStage、实体登记的完整实现、游戏对象生命期、并发时序、加载器行为和实际绘制效果仍需现场验收。
