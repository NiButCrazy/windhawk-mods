# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

本目录是 Windhawk 仓库（git 根为 `E:\MyCode\Scripts\windhawk mods`）中的一个 mod 工程，注入 `explorer.exe`，把新打开的 File Explorer 窗口重定向为已有窗口中的标签页。

| 文件 | 状态 | 说明 |
| --- | --- | --- |
| `mod.wh.cpp` | **当前实现** | id `nbc-explorer-single-window-tabs`，version 0.10 |
| `mod-v2.wh.cpp` | 实验性，暂不使用 | id `nbc-explorer-single-window-tabs-v2`，包含同路径复用等未采用的逻辑 |
| `MAINTENANCE.md` | 参考资料 | v2 的实测内部行为、内部命令表、测试清单；内部命令和标签页识别部分对 v1 同样适用 |

**不要同时启用 v1 和 v2**：两者都会 hook `CreateWindowExW`，会把对方创建的窗口当成普通新窗口。

父仓库其余文件（`StartMenuStyler*.json`、`TaskbarStyler.*`、`NotificationCenterStyler*`、`TranslucentWindows.cpp` 等）是其他 mod，与本工程没有构建依赖。

没有测试、CI、Makefile 或构建脚本；编译由 Windhawk 完成。

## 编译检查

Windhawk 1.7.3 安装在 `E:\Windhawk`。修改 v1 后用其自带 clang++ 做语法/类型检查：

```bash
"E:/Windhawk/Compiler/bin/clang++.exe" @E:/Windhawk/Compiler/compile_flags.txt \
  -DWH_MOD_ID='"nbc-explorer-single-window-tabs"' \
  -DWH_MOD_VERSION='"0.10"' \
  -I E:/Windhawk/Compiler/include \
  -c mod.wh.cpp -o "$TEMP/est_v1_check.o"
```

- `-DWH_MOD_ID` 必须提供，否则 `windhawk_utils.h` 会因宏未定义而报错。
- 头部 `@compilerOptions` 里的 `-l...` 在 `-c` 下只会产生 "linker input unused" 警告，可忽略。
- 输出 `.o` 放仓库外，不要提交。
- 本机 Windhawk 没有安装/应用 mod 的命令行；实际编译、安装、加载需在 Windhawk UI 中完成。
- 运行时日志由 `Wh_Log` 输出，在 Windhawk UI 的 mod 日志中查看。v1 会输出 `[v1-timing]` 阶段耗时（hook precheck/create、path、primary、selection、snapshot、post、find、navigate、select、total），排查延迟时先看这一行。
- IDE/clangd 可能因为找不到 `windhawk_utils.h` 报错；以上面的命令结果为准。

## v1 架构（当前实现）

1. `CreateWindowExW_Hook` 只拦截 `CabinetWClass`（Shift 放行）。先用纯 Win32 `EnumWindows` 快速判断是否有其它可见 Explorer 窗口；没有就直接原样创建（首个窗口不隐藏）。有则创建后立刻设为 `WS_EX_LAYERED` + 全透明，保留 `WS_VISIBLE`（shell 只会把可见窗口注册进 `IShellWindows`，读路径/选中项需要它），登记为 phantom，启动后台 `RedirectThread`。`ShowWindow_Hook` 阻止透明窗口闪现。UI 线程不再做 `IShellWindows` 扫描。
2. `RedirectThread` 在 STA 中：读取透明窗口的目标路径（`LocationURL`，特殊位置用 PIDL），然后在后台调用 `FindExistingExplorer` 确认目标 Explorer 窗口（失败就 `AbortRedirect`），快照目标窗口的标签页句柄。
3. 向 `ShellTabWindowClass` 发送 `WM_COMMAND 0xA21B` 新建标签页，用 `FindNewTab` 按 per-tab `IShellBrowser::GetWindow()` 句柄精确识别**刚创建**的标签页，然后只导航这一个标签页。
4. **不再回退到已有标签页**：如果新标签页识别失败或 `Navigate2` 失败，不碰任何已有标签页，直接 `AbortRedirect` 恢复原新窗口显示。原重复窗口只在导航成功后关闭。
5. `SHOpenFolderAndSelectItems` 的选中项改为**导航成功后再读取**，不再阻塞建标签页；读取后在新标签页里用 `IFolderView::SelectAndPositionItems` 恢复，最后才关闭透明重复窗口并把目标窗口带到前台。

### 已实测的 Windows 内部行为（build 26300）

- 同一窗口所有标签页的 `IWebBrowser2::get_HWND()` 返回同一个 `CabinetWClass` frame 句柄；每个标签页唯一身份是 `IServiceProvider::QueryService(SID_STopLevelBrowser)` → `IShellBrowser::GetWindow()` 返回的 `ShellTabWindowClass`。
- `IShellWindows` 中同一 frame 的条目顺序 = 标签栏顺序；frame 的直接子窗口顺序**不是**标签栏顺序（不要当索引用）。
- 每个 `ShellTabWindowClass` 的窗口标题 = 标签页显示名；frame 标题以活动标签页标题开头。
- 内部命令：
  - `0xA21B`：发给任意 tab 窗口，`wParam=0xA21B, lParam=0`，新建标签页。
  - `0xA221`：发给 frame，`wParam=0xA221, lParam=1-based index`，切换标签页；只接受 1..9，超出范围被忽略。
  - `0xA021`：发给活动 tab 窗口，`wParam=0xA021, lParam=1`，关闭标签页。
- 没有公开的「按路径直接开标签页」API；`opennewtab` shell verb 程序化调用也只会开新窗口。`0xA21B` + `Navigate2` 是已知可行方案。

### 修改时的不变式

- `WS_VISIBLE` 必须保留：去掉它会导致 shell 不注册窗口，读不到路径/选中项。
- 只操作 `FindNewTab` 识别出的新标签页；永远不要用「最新索引/空标签页」导航用户已有标签页。
- 先导航成功，再关闭重复窗口；导航失败要 `AbortRedirect` 恢复原窗口。
- `0xA021` 只用于关闭已确认的活动标签页（如有需要）。
- 修改内部命令或标签页识别逻辑前，先阅读 `MAINTENANCE.md` 的实测记录并在目标 build 上复测。

## 提交约定

提交信息使用中文并带 Conventional Commits 前缀（如 `feat: 小修26h2菜单`、`fix: 修复了标签页一点BUG`）。版本号在文件头 `// @version` 中维护。
