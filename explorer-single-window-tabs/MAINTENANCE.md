# Explorer 单窗口标签页 v2 维护文档

> **注意：v2 目前暂不使用。当前实现是 `mod.wh.cpp`（v1，version 0.10）。**
> 本文档保留 v2 的实测资料；其中的 Windows 内部行为、内部命令和标签页识别结论对 v1 同样适用。
>
> v1 0.10 的优化：hook 只做 Win32 `EnumWindows` 预检，`FindExistingExplorer` 移到后台线程；
> 读取路径/选中项后立即关闭重复窗口（让 UI 线程先处理 WM_CLOSE，再处理 `0xA21B`）；
> 新标签页导航成功后立即前台显示，选中项恢复放在其后；
> 路径轮询复用同一个 `IShellWindows`；标签页快照改用 Win32 直接子窗口枚举；
> 日志新增 `[v1-timing]` 阶段耗时。

对应实现：`mod-v2.wh.cpp`（mod id `nbc-explorer-single-window-tabs-v2`，version 1.0）

旧实现 `mod.wh.cpp`（v1）仅作参考保留。**不要同时启用 v1 和 v2**：两个 mod 都会 hook
`CreateWindowExW`，会把对方创建的窗口当成普通新窗口，行为会互相破坏。

## 1. 目标与行为

1. 新开的 File Explorer 窗口并入已有窗口的标签页。
2. 如果目标文件夹已经在任意同进程 Explorer 标签页中打开，优先切换到该标签页。
3. 切换降级链：
   - 内部命令 `0xA221` 无刷新切换；
   - `IWebBrowser2::Navigate2` 同路径（保证激活，但会刷新视图）；
   - 在匹配到的窗口里新建标签页并导航。
4. 「在文件夹中显示」（`SHOpenFolderAndSelectItems`）的选中项在目标标签页中恢复。
5. 任何失败都恢复原新窗口正常显示；不导航用户已有标签页；不关闭用户已有标签页。
6. Shift+打开保持原行为（新窗口）。

## 2. 实测环境与 Windows 内部行为

测试环境：Windows 11 build 26300，Windhawk 1.7.3。

### 2.1 IShellWindows 与标签页身份

- 同一窗口的每个标签页都是独立的 `IShellWindows` 条目，但
  `IWebBrowser2::get_HWND()` 对所有标签页都返回同一个 `CabinetWClass` **frame** 句柄。
- 每个标签页唯一的身份是：
  `IServiceProvider::QueryService(SID_STopLevelBrowser)` →
  `IShellBrowser::GetWindow()` → 该标签页的 `ShellTabWindowClass` 窗口。
- `IShellWindows` 集合中同一 frame 的条目顺序 = **标签栏顺序**。`0xA221` 的索引以此为准。
- frame 的直接子窗口顺序 **不是** 标签栏顺序（活动标签页通常排在子窗口 z-order 的最前面，
  新建/关闭标签后还会变化）。因此代码里只用直接子窗口做句柄集合/差集，绝不当索引用。

实测直接子窗口类名包括：

```
InputNonClientPointerSource
TITLE_BAR_SCAFFOLDING_WINDOW_CLASS
ShellTabWindowClass            (每个标签页一个)
Microsoft.UI.Content.DesktopChildSiteBridge
```

### 2.2 活动标签页识别

- 每个 `ShellTabWindowClass` 的 `GetWindowText` = 该标签页的显示名。
- frame 标题 = `<活动标签页标题><本地化后缀>`，例如
  `explorer-single-window-tabs 和 3 个其他选项卡 - 文件资源管理器`，
  单标签页时也可能没有「和 N 个其他选项卡」后缀。
- 因此判断活动标签页：frame 标题是否以某个 tab 窗口标题为前缀，且下一个字符是空格/`-`/`—`。
  这个方法在窗口最小化时同样有效（最小化时 `GetClientRect` 为 0、`ClientToScreen` 返回
  无效坐标，不能靠窗口矩形判断）。
- 多个标签页标题相同时无法从标题区分，代码取第一个匹配项；这种情况极少，切换验证可能
  产生假阳性。

### 2.3 内部命令（build 26300 实测）

| 命令 | 目标窗口 | wParam | lParam | 用途 | 验证情况 |
| --- | --- | --- | --- | --- | --- |
| `0xA21B` | 任意 `ShellTabWindowClass` | `0xA21B` | `0` | 新建标签页 | 实测可用；新标签追加到末尾 |
| `0xA221` | `CabinetWClass` frame | `0xA221` | `index + 1`（1-based） | 切换标签页 | 实测 lParam 1–4 可用；lParam=10 被忽略 |
| `0xA021` | 活动标签页窗口 | `0xA021` | `1` | 关闭标签页 | 实测可用；代码只对活动标签页发送 |

关于 `0xA221` 的索引范围：
- 它接受 1-based 索引，只对 1..9 生效；超出范围的值被忽略。
- 实测 3 个标签页时 `lParam=9` 无效；10 个标签页时 `lParam=10` 无效。没有「9 = 最后一个」的特殊语义。
- 代码只在 `index < 8`，或 `index == 8 && 总标签数 == 9` 时使用该命令。
- 其余情况直接使用 `Navigate2`。`Navigate2` 会激活目标标签页，代价是重新加载视图。

关于 `0xA021`：
- 实测关闭的是接收消息的**活动**标签页。
- 为避免未来系统行为变化时误关用户标签页，`CloseTabWindow` 会先确认目标标签页就是活动
  标签页；不是则放弃关闭（失败回滚时最多留下一个空白标签页，绝不关用户的标签页）。

### 2.4 多进程

Windows 11 的桌面 shell 和文件窗口通常分别在两个 `explorer.exe` 进程中；Windhawk 的
`@include explorer.exe` 会注入每一个。v2 只考虑**同进程**的 Explorer 窗口
（`GetWindowThreadProcessId(frame) == GetCurrentProcessId()`），原因是
`IShellBrowser`/`IFolderView` 等接口在跨进程封送时不可靠。默认配置下所有文件夹窗口在
同一个进程里，不受影响；如果用户启用「在单独的进程中打开文件夹窗口」，不同进程的窗口
不会合并（新窗口正常显示）。

## 3. 代码结构

```
mod-v2.wh.cpp
├─ ComPtr<T>                        最小 COM RAII
├─ phantom 注册表                   透明待重定向窗口的集合
├─ 小工具                           窗口类/标题/Z-order/直接子标签枚举
├─ 路径工具                          LocationURL → 路径、PIDL 回退、规范化比较
├─ EnumerateFrames()                一次 IShellWindows 枚举，按 frame 分组
├─ FindTabBrowser()                 新建标签页句柄 → 对应 IWebBrowser2
├─ NavigateBrowser()                Navigate2（普通路径 BSTR，::{...} 用 PIDL）
├─ CheckActiveTab()                 标题前缀判断活动标签页
├─ 选中项                            QuerySelectedItems / GetSourceSelection / SelectItemsInTab
├─ 目标窗口选择 / 复用               PickPrimaryFrame / TryReuseExistingTab
├─ CreateAndNavigateNewTab()        快照→0xA21B→句柄差集→找 browser→Navigate2
├─ RedirectThread()                 分阶段流程，失败回滚
├─ CreateWindowExW_Hook / ShowWindow_Hook
└─ Wh_ModInit / Wh_ModUninit
```

## 4. RedirectThread 流程

```
-1. Hook 内的纯 Win32 预检：EnumWindows 找不到其它同进程、可见、非 phantom 的
    CabinetWClass → 直接按原样创建并返回，不隐藏、不起线程（首个窗口/全部关闭后）
0. 线程内兜底判空：IShellWindows 里没有其它可见、同进程、非 phantom 的 frame → 恢复原窗口
1. 读路径：只枚举新窗口自己的 IShellWindows 条目
   - LocationURL 路径立即采信
   - 仅 PIDL 的路径要等 READYSTATE_COMPLETE，避免读到临时的 Home/此电脑
2. 一次枚举所有 frame/tab（同进程、可见、非 phantom）
3. 路径匹配已有的 tab
   - 0xA221（受索引限制）→ 标题验证
   - Navigate2 同路径 → 标题验证
   - 都失败 → 在匹配窗口新建标签页
4. 新建标签页：
   - 快照直接子 ShellTabWindowClass 句柄
   - 向任一 tab 窗口 PostMessage(0xA21B)
   - 2ms 轮询直接子窗口差集，拿到本次新建的精确 HWND
   - 轮询 IShellWindows，按 GetWindow()==newTabWnd 找到 browser
   - Navigate2；失败则尝试关闭本次新建的标签页并恢复原窗口
   - 避免 Home/此电脑闪屏：记录创建前活动标签页，新标签页若可被 0xA221 选中
     （总标签数 ≤ 9 且索引 < 9），先切回原标签页；Navigate2 后再切回一次，
     等新标签页的 LocationURL 提交到目标路径（或 READYSTATE_COMPLETE，上限 500ms），
     最后切到新标签页。这样新标签页出现时已经位于目标文件夹。
5. 需要时从原新窗口读取选中项，对目标标签页 SelectAndPositionItems
6. 成功后把目标窗口带到前台，再对透明重复窗口 PostMessage(WM_CLOSE)
```

### 4.1 为什么先导航成功再关闭重复窗口

重复窗口（phantom）在重定向成功前一直保留。这样：

- 任何时候失败都可以恢复它；
- 读取路径和选中项用的是它自己的 per-tab browser，不会被目标标签页干扰；
- 即使目标窗口在过程中关闭，也只是恢复原窗口，而不是丢失用户操作。

失败/回滚矩阵：

| 失败点 | 处理 |
| --- | --- |
| 读不到路径 / 路径不可重定向 | 去掉透明、恢复显示原新窗口 |
| 没有可用的已有窗口 | 同上 |
| 找不到同路径 tab | 走新建标签页 |
| 0xA221 发送失败或活动标签页不对 | 降级 Navigate2 |
| Navigate2 失败 | 尝试关闭本次新建标签页，恢复原窗口 |
| 新建标签页后找不到 browser | 尝试关闭本次新建标签页，恢复原窗口 |
| 新建标签页的 Navigate2 失败 | 同上 |
| 选中项恢复失败 | 仅记录日志，保留已经成功的切换/新建 |
| 卸载 mod 时有 pending phantom | 恢复显示 |

### 4.2 phantom 生命周期

- `Pending`：重定向进行中，卸载或回滚时恢复显示。
- `Redirected`：已成功并发送 WM_CLOSE，只等窗口销毁，**永远不再显示**。
- 窗口真正销毁前 phantom 一直保留，防止正在关闭的窗口被选成目标窗口。
- `PrunePhantoms` 在每次创建 Cabinet 窗口时清理已销毁的 HWND。
- `ShowWindow` 对 phantom 返回 TRUE；`RestoreNewWindow` 先移除 phantom，再直接调用原
  `ShowWindow` 重放之前拦截到的 `nCmdShow`（保留最大化等状态）。

## 5. 日志

关键字 `[est-v2]`，在 Windhawk mod 日志中查看。常用行：

| 日志 | 含义 |
| --- | --- |
| `redirect start new=... fg=...` | 开始处理新窗口；fg 是创建时的前台窗口 |
| `no other Explorer window, restore` | 第一个/唯一窗口，正常显示 |
| `path not available, restore` | 500ms 内读不到路径 |
| `non-redirectable path ...` | 控制面板/shell 特殊位置 |
| `select tab ... ok=1` | 0xA221 发送结果（ok=1 表示没有超时） |
| `reuse tab ... mode=command/navigate` | 复用成功及方式 |
| `reuse match but activation failed` | 匹配到但切不过去，走新建 |
| `new tab frame=... tab=...` | 精确捕获的新标签页 |
| `selected N items` | 选中项恢复成功 |
| `done reuse/new-tab ...` | 成功结束 |
| `not closing background tab` | 回滚时为了安全放弃关闭非活动标签页 |
| `unloaded` | 卸载完成 |

## 6. 构建

```bash
"E:/Windhawk/Compiler/bin/clang++.exe" @E:/Windhawk/Compiler/compile_flags.txt \
  -DWH_MOD_ID='"nbc-explorer-single-window-tabs-v2"' \
  -DWH_MOD_VERSION='"1.0"' \
  -I E:/Windhawk/Compiler/include \
  -c mod-v2.wh.cpp -o "$TEMP/est_v2_check.o"
```

`Wh_ModInit`/`Wh_ModUninit` 通过 Windhawk UI 编译、应用；本机没有 CLI 安装/应用 mod 的方式。

## 7. 手动测试清单

在 Windhawk UI 中编译并应用 v2（先禁用旧 mod）：

1. 已有窗口打开新文件夹 → 新标签页，原窗口前置。
2. 重复打开同一文件夹 → 切到已有标签页，不产生重复。
3. 在 10 个以上标签页的窗口里打开一个已在第 10+ 个标签页中的文件夹 → 走 Navigate2 刷新切换。
4. 浏览器「在文件夹中显示」→ 新建/复用标签页并选中文件。
5. 已有同路径标签页时「在文件夹中显示」→ 切换并选中文件。
6. Shift+双击/Shift+打开 → 正常新窗口。
7. 控制面板、`shell:::{...}` → 不重定向。
8. 连续快速打开 3 个文件夹 → 3 个标签页，无空白、无错导航。
9. 关闭全部窗口后再打开 → 第一个正常显示，后续并入。
10. 最小化主窗口时打开文件夹 → 能并入并恢复前台。
11. 重定向进行中关闭目标窗口 → 原新窗口恢复显示。
12. 重定向进行中禁用 mod → 无隐形窗口残留。
13. 打开「在单独的进程中打开文件夹窗口」→ 新窗口正常显示（已知限制）。

## 8. 已知限制

- 内部命令未公开，Windows 更新后可能变化。失败时代码会回滚到正常窗口。
- `0xA221` 只在小索引范围内使用；第 10 个及以后的标签页切换会刷新视图。
- Home 闪屏抑制只在总标签数 ≤ 9 且新旧标签页索引都能用 `0xA221` 选中时启用；
  超过 9 个标签页时，新建标签页仍可能短暂显示 Home/此电脑。
- 多个标签页同名时活动标签页识别可能不精确。
- 用户在我们「快照→新建→捕获」的几毫秒内同时手动 Ctrl+T，极端情况下可能多一个空白标签页。
- 只合并同进程窗口；单独进程模式不合并。
- 路径匹配是规范化后的字符串比较，不处理短路径/符号链接/硬链接等别名。
- 「在文件夹中显示」的选中项恢复依赖 `IFolderView::SelectAndPositionItems`；搜索视图等
  非普通文件夹视图可能无法选中。

## 9. Windows 版本适配 checklist

如果新系统上出现问题，按顺序检查：

1. `0xA21B` 是否还能在 tab 窗口上新建标签页；不能则无法重定向，所有窗口会正常显示。
2. frame 的直接子窗口是否仍为 `ShellTabWindowClass`；类名变化会导致标签页捕获失败。
3. `IShellBrowser::GetWindow()` 是否仍返回 per-tab 窗口；否则无法区分标签页。
4. `0xA221` 是否仍支持 `wParam=0xA221, lParam=index+1`；不支持会自动降级 Navigate2。
5. 标签页窗口标题是否仍等于 tab 显示名；变化会影响活动标签页识别（未知时信任命令成功）。
6. `IShellWindows` 条目顺序是否仍等于标签栏顺序；变化会导致 0xA221 索引错误，但
   `CheckActiveTab` 会检测到并降级。
