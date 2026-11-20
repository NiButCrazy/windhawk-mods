// ==WindhawkMod==
// @id              nbc-explorer-single-window-tabs-v2
// @name            Explorer 单窗口标签页 v2
// @description     把新打开的 Explorer 窗口重定向为已有窗口的标签页，优先复用同路径标签页
// @version         1.0
// @author          Ni But Crazy
// @github          https://github.com/almas-cp
// @homepage        https://github.com/almas-cp
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -lshlwapi -lshell32 -luuid -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer 单窗口标签页 v2

把新打开的 File Explorer 窗口重定向为已有窗口中的一个标签页。

## 行为

1. 新窗口的目标文件夹如果已经在**任意** Explorer 标签页中打开，优先切换到该标签页，
   不产生重复标签页。
2. 切换顺序：内部无刷新切换 → `Navigate2` 同路径（会刷新视图）→ 新建标签页。
3. 通过「在文件夹中显示」等方式打开时，会在目标标签页中恢复选中的项目。
4. 按住 **Shift** 打开窗口时保持原行为（新窗口）。
5. 控制面板、`shell:::{...}` 等特殊位置不会被重定向。

## 注意

- 本 mod 是 `mod.wh.cpp`（v1）的全新实现。**不要和旧 mod 同时启用**，两者会互相干扰。
- 只在同一个 explorer.exe 进程内的窗口之间合并。启用「在单独的进程中打开文件夹窗口」
  时，不同进程的窗口不会合并。
- 基于 Windows 11 build 26300 实测的内部行为，系统更新后内部命令可能变化。
  失败时会回退到「正常显示新窗口」，不会留下隐形窗口。
- 运行时日志可在 Windhawk 的 mod 日志中查看，关键字为 `[est-v2]`。
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- ReuseExistingTab: true
  $name: 复用已打开的同路径标签页
  $description: >-
    目标文件夹已经在某个 Explorer 标签页中打开时，优先切换到该标签页。
    关闭后总是新建标签页（仍然会恢复「在文件夹中显示」的选中项）。
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <climits>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
//
// The 0xA21B/0xA221/0xA021 command IDs are Windows-internal shell commands.
// They were verified on Windows 11 build 26300:
//   0xA21B  PostMessage(tabWindow, WM_COMMAND, 0xA21B, 0)        -> new tab
//   0xA221  SendMessage(frame,    WM_COMMAND, 0xA221, index + 1)  -> select tab
//   0xA021  SendMessage(tabWindow,WM_COMMAND, 0xA021, 1)         -> close tab
// The index for 0xA221 is 1-based and follows the tab-bar order.
// ---------------------------------------------------------------------------
constexpr WCHAR kCabinetClass[] = L"CabinetWClass";
constexpr WCHAR kTabClass[]     = L"ShellTabWindowClass";

constexpr UINT kCmdNewTab    = 0xA21B;
constexpr UINT kCmdSelectTab = 0xA221;
constexpr UINT kCmdCloseTab  = 0xA021;

constexpr WCHAR kControlPanelClsid[] =
    L"::{26EE0668-A00A-44D7-9371-BEB064C98683}";

constexpr DWORD kPathTimeoutMs        = 500;
constexpr DWORD kTabWindowTimeoutMs   = 800;
constexpr DWORD kTabBrowserTimeoutMs  = 900;
constexpr DWORD kSwitchVerifyMs       = 250;
constexpr DWORD kNavigateVerifyMs     = 800;
constexpr DWORD kBackgroundActivateWaitMs = 500;
constexpr DWORD kSelectionTimeoutMs   = 1200;
constexpr DWORD kPruneWaitMs          = 250;

constexpr DWORD kPollFastMs = 2;
constexpr DWORD kPollSlowMs = 10;

// ---------------------------------------------------------------------------
// Minimal COM RAII. Keeping this local avoids depending on WRL/ATL being
// available in Windhawk's bundled compiler.
// ---------------------------------------------------------------------------
template <typename T>
class ComPtr {
  public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.Detach()) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other)
            Attach(other.Detach());
        return *this;
    }

    T* Get() const { return ptr_; }
    T** Put() {
        Reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    void Attach(T* ptr) {
        Reset();
        ptr_ = ptr;
    }

    T* Detach() {
        T* ptr = ptr_;
        ptr_ = nullptr;
        return ptr;
    }

  private:
    T* ptr_ = nullptr;
};

// ---------------------------------------------------------------------------
// Globals and lifecycle
// ---------------------------------------------------------------------------
using CreateWindowExW_t = decltype(&CreateWindowExW);
using ShowWindow_t      = decltype(&ShowWindow);

static CreateWindowExW_t CreateWindowExW_Original;
static ShowWindow_t      ShowWindow_Original;

static volatile LONG g_unloading      = 0;
static volatile LONG g_activeThreads  = 0;
static bool          g_reuseExistingTab = true;

static CRITICAL_SECTION g_tabCreateLock;
static CRITICAL_SECTION g_lifecycleLock;

// Thread-local state for the thread that is inside the original
// CreateWindowExW call.  While this is set, ShowWindow must not let the
// not-yet-registered CabinetWClass window flash on screen.
static thread_local bool  t_inCabinetCreate = false;
static thread_local HWND  t_pendingShowHwnd = nullptr;
static thread_local int   t_pendingShowCmd  = 0;

struct RedirectInfo {
    HWND newWindow;
    HWND foregroundAtCreate;
};

// ---------------------------------------------------------------------------
// Phantom windows
//
// A redirected window is created normally but immediately made fully
// transparent with WS_EX_LAYERED while keeping WS_VISIBLE.  WS_VISIBLE is
// required: the shell only registers visible windows in IShellWindows, and
// that registration is how the target path and the selected items are read.
//
// Phantom entries stay registered until the HWND is destroyed, so a window
// that is still closing can never be picked as the destination window.
// ---------------------------------------------------------------------------
enum class PhantomState {
    Pending,     // redirect still running; restore it if we abort/unload
    Redirected,  // redirect succeeded and WM_CLOSE was posted; never show it
};

struct PhantomEntry {
    HWND         hwnd;
    PhantomState state;
    int          showCmd;  // last ShowWindow command we blocked, 0 if none
};

static CRITICAL_SECTION g_phantomLock;
static std::vector<PhantomEntry> g_phantoms;
static volatile LONG g_phantomCount = 0;

static void PrunePhantomsLocked() {
    for (size_t i = 0; i < g_phantoms.size();) {
        if (!IsWindow(g_phantoms[i].hwnd))
            g_phantoms.erase(g_phantoms.begin() + i);
        else
            i++;
    }
    InterlockedExchange(&g_phantomCount, static_cast<LONG>(g_phantoms.size()));
}

static void PrunePhantoms() {
    if (InterlockedCompareExchange(&g_phantomCount, 0, 0) == 0)
        return;
    EnterCriticalSection(&g_phantomLock);
    PrunePhantomsLocked();
    LeaveCriticalSection(&g_phantomLock);
}

static bool IsPhantomWindow(HWND hwnd) {
    if (!hwnd || InterlockedCompareExchange(&g_phantomCount, 0, 0) == 0)
        return false;

    bool found = false;
    EnterCriticalSection(&g_phantomLock);
    for (const auto& entry : g_phantoms) {
        if (entry.hwnd == hwnd) {
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_phantomLock);
    return found;
}

static void AddPhantom(HWND hwnd, int showCmd) {
    EnterCriticalSection(&g_phantomLock);
    g_phantoms.push_back({hwnd, PhantomState::Pending, showCmd});
    InterlockedExchange(&g_phantomCount, static_cast<LONG>(g_phantoms.size()));
    LeaveCriticalSection(&g_phantomLock);
}

static void SetPhantomShowCmd(HWND hwnd, int showCmd) {
    if (!hwnd || InterlockedCompareExchange(&g_phantomCount, 0, 0) == 0)
        return;

    EnterCriticalSection(&g_phantomLock);
    for (auto& entry : g_phantoms) {
        if (entry.hwnd == hwnd) {
            entry.showCmd = showCmd;
            break;
        }
    }
    LeaveCriticalSection(&g_phantomLock);
}

// Returns the saved ShowWindow command, or 0 if the window wasn't registered.
static int RemovePhantom(HWND hwnd) {
    int showCmd = 0;
    if (!hwnd || InterlockedCompareExchange(&g_phantomCount, 0, 0) == 0)
        return 0;

    EnterCriticalSection(&g_phantomLock);
    for (size_t i = 0; i < g_phantoms.size(); i++) {
        if (g_phantoms[i].hwnd == hwnd) {
            showCmd = g_phantoms[i].showCmd;
            g_phantoms.erase(g_phantoms.begin() + i);
            break;
        }
    }
    InterlockedExchange(&g_phantomCount, static_cast<LONG>(g_phantoms.size()));
    LeaveCriticalSection(&g_phantomLock);
    return showCmd;
}

static void MarkPhantomRedirected(HWND hwnd) {
    if (!hwnd || InterlockedCompareExchange(&g_phantomCount, 0, 0) == 0)
        return;

    EnterCriticalSection(&g_phantomLock);
    for (auto& entry : g_phantoms) {
        if (entry.hwnd == hwnd) {
            entry.state = PhantomState::Redirected;
            break;
        }
    }
    LeaveCriticalSection(&g_phantomLock);
}

static std::vector<HWND> GetPendingPhantoms() {
    std::vector<HWND> pending;
    if (InterlockedCompareExchange(&g_phantomCount, 0, 0) == 0)
        return pending;

    EnterCriticalSection(&g_phantomLock);
    for (const auto& entry : g_phantoms) {
        if (entry.state == PhantomState::Pending && IsWindow(entry.hwnd))
            pending.push_back(entry.hwnd);
    }
    LeaveCriticalSection(&g_phantomLock);
    return pending;
}

// ---------------------------------------------------------------------------
// Small Win32 helpers
// ---------------------------------------------------------------------------
static bool IsCabinetClass(HWND hwnd) {
    WCHAR className[64];
    return hwnd && GetClassNameW(hwnd, className, ARRAYSIZE(className)) &&
           wcscmp(className, kCabinetClass) == 0;
}

static bool IsTabClass(HWND hwnd) {
    WCHAR className[64];
    return hwnd && GetClassNameW(hwnd, className, ARRAYSIZE(className)) &&
           wcscmp(className, kTabClass) == 0;
}

static std::wstring GetWindowTextString(HWND hwnd) {
    std::wstring text;
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0)
        return text;

    text.resize(static_cast<size_t>(length) + 1);
    int copied = GetWindowTextW(hwnd, text.data(),
                                static_cast<int>(text.size()));
    if (copied <= 0)
        text.clear();
    else
        text.resize(static_cast<size_t>(copied));
    return text;
}

static bool IsSameProcessWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// Direct children of a CabinetWClass frame that are tab windows.  The child
// z-order is NOT the tab-bar order (the active tab is normally first), so this
// is only used to snapshot/diff handles or to find an arbitrary tab to post
// the "new tab" command to.  Tab-bar order comes from IShellWindows instead.
static std::vector<HWND> SnapshotTabWindows(HWND frame) {
    std::vector<HWND> tabs;
    if (!frame || !IsWindow(frame))
        return tabs;

    HWND child = GetWindow(frame, GW_CHILD);
    while (child) {
        if (IsTabClass(child))
            tabs.push_back(child);
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return tabs;
}

static bool ContainsWindow(const std::vector<HWND>& list, HWND hwnd) {
    for (HWND item : list) {
        if (item == hwnd)
            return true;
    }
    return false;
}

// Cheap Win32-only check used by CreateWindowExW_Hook before it hides the new
// window.  If there is no existing same-process Explorer window to merge into
// (first window after Explorer restart, or all windows just closed), the new
// window can be created and shown normally without ever becoming a phantom.
struct VisibleCabinetSearch {
    bool found;
};

static BOOL CALLBACK FindVisibleCabinetProc(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<VisibleCabinetSearch*>(lParam);
    if (!hwnd || !IsWindowVisible(hwnd) || !IsSameProcessWindow(hwnd) ||
        !IsCabinetClass(hwnd) || IsPhantomWindow(hwnd)) {
        return TRUE;
    }
    search->found = true;
    return FALSE;
}

static bool HasVisibleCabinetWindow() {
    VisibleCabinetSearch search{false};
    EnumWindows(FindVisibleCabinetProc, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// The frame title is "<active tab title><localized suffix>" (for example
// "Folder 和 3 个其他选项卡 - 文件资源管理器").  Each tab window's own title is
// the tab's display name, so the active tab can be identified without parsing
// the localized suffix or relying on window positions (which are unusable
// while the frame is minimized).
static bool TitleHasPrefix(const std::wstring& title,
                           const std::wstring& prefix) {
    if (prefix.empty() || title.size() < prefix.size())
        return false;
    if (title.compare(0, prefix.size(), prefix) != 0)
        return false;
    if (title.size() == prefix.size())
        return true;
    WCHAR next = title[prefix.size()];
    return next == L' ' || next == L'-' || next == L'\u2014';
}

static HWND GetActiveTabWindow(HWND frame) {
    std::wstring frameTitle = GetWindowTextString(frame);
    if (frameTitle.empty())
        return nullptr;

    HWND best = nullptr;
    HWND child = GetWindow(frame, GW_CHILD);
    while (child) {
        if (IsTabClass(child)) {
            std::wstring tabTitle = GetWindowTextString(child);
            if (TitleHasPrefix(frameTitle, tabTitle)) {
                // If several tabs share the same display name, any of them is
                // indistinguishable from the title alone.  The first one is
                // good enough for verification.
                best = child;
                break;
            }
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return best;
}

// Z-order rank: 0 is the topmost top-level window.  Used to prefer the
// Explorer window the user most likely interacted with when there is no
// usable foreground Explorer window at creation time.
static int GetZOrderRank(HWND target) {
    int rank = 0;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (h == target)
            return rank;
        if (++rank > 10000)
            break;
    }
    return INT_MAX;
}

static void BringFrameToForeground(HWND frame) {
    if (!IsWindow(frame))
        return;

    if (IsIconic(frame) && ShowWindow_Original)
        ShowWindow_Original(frame, SW_RESTORE);

    DWORD pid = 0;
    GetWindowThreadProcessId(frame, &pid);
    if (pid)
        AllowSetForegroundWindow(pid);
    SetForegroundWindow(frame);
}

// ---------------------------------------------------------------------------
// Shell path helpers
// ---------------------------------------------------------------------------
static bool IsClsidPath(const std::wstring& path) {
    return path.size() >= 3 && path[0] == L':' && path[1] == L':' &&
           path[2] == L'{';
}

static bool IsRedirectablePath(const std::wstring& path) {
    if (path.empty())
        return false;
    if (path.find(kControlPanelClsid) != std::wstring::npos)
        return false;
    if (path.compare(0, 9, L"shell:::{") == 0)
        return false;
    return true;
}

static std::wstring NormalizePath(const std::wstring& input) {
    std::wstring path = input;

    // Filesystem paths may have a trailing separator.  Keep drive roots
    // ("C:\") intact and don't touch URL-like strings.
    bool isFileSystem =
        (path.size() >= 2 && path[1] == L':') ||
        (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
    if (isFileSystem) {
        while (path.size() > 3 &&
               (path.back() == L'\\' || path.back() == L'/')) {
            path.pop_back();
        }
        for (WCHAR& ch : path) {
            if (ch == L'/')
                ch = L'\\';
        }
    }
    return path;
}

static bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty())
        return false;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

static std::wstring GetPidlPath(IDispatch* disp) {
    std::wstring result;
    if (!disp)
        return result;

    ComPtr<IServiceProvider> serviceProvider;
    if (FAILED(disp->QueryInterface(IID_PPV_ARGS(serviceProvider.Put()))))
        return result;

    ComPtr<IShellBrowser> shellBrowser;
    if (FAILED(serviceProvider->QueryService(
            SID_STopLevelBrowser, IID_PPV_ARGS(shellBrowser.Put()))))
        return result;

    ComPtr<IShellView> shellView;
    if (FAILED(shellBrowser->QueryActiveShellView(shellView.Put())))
        return result;

    ComPtr<IFolderView> folderView;
    if (FAILED(shellView->QueryInterface(IID_PPV_ARGS(folderView.Put()))))
        return result;

    ComPtr<IPersistFolder2> persistFolder;
    if (FAILED(folderView->GetFolder(IID_PPV_ARGS(persistFolder.Put()))))
        return result;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(persistFolder->GetCurFolder(&pidl)) || !pidl)
        return result;

    PWSTR name = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING,
                                     &name)) &&
        name) {
        result = name;
        CoTaskMemFree(name);
    }
    CoTaskMemFree(pidl);
    return result;
}

// Returns the folder path.  fromUrl is set when the value came from
// LocationURL; a PIDL-only path is less trustworthy while the window is still
// loading (it may transiently be the default Home/This PC location), so the
// redirect thread waits for READYSTATE_COMPLETE before using such a path.
static std::wstring GetBrowserPath(IDispatch* disp, IWebBrowser2* browser,
                                   bool* fromUrl = nullptr) {
    if (fromUrl)
        *fromUrl = false;

    std::wstring result;
    if (!browser)
        return result;

    BSTR url = nullptr;
    if (SUCCEEDED(browser->get_LocationURL(&url)) && url && *url) {
        WCHAR filePath[MAX_PATH * 2];
        DWORD length = ARRAYSIZE(filePath);
        if (SUCCEEDED(PathCreateFromUrlW(url, filePath, &length, 0)))
            result = filePath;
        else
            result = url;
        if (fromUrl && !result.empty())
            *fromUrl = true;
    }
    if (url)
        SysFreeString(url);

    if (result.empty())
        result = GetPidlPath(disp);
    return result;
}

static HWND QueryTabWindow(IDispatch* disp) {
    HWND tabWnd = nullptr;
    if (!disp)
        return nullptr;

    ComPtr<IServiceProvider> serviceProvider;
    if (FAILED(disp->QueryInterface(IID_PPV_ARGS(serviceProvider.Put()))))
        return nullptr;

    ComPtr<IShellBrowser> shellBrowser;
    if (FAILED(serviceProvider->QueryService(
            SID_STopLevelBrowser, IID_PPV_ARGS(shellBrowser.Put()))))
        return nullptr;

    shellBrowser->GetWindow(&tabWnd);
    return tabWnd;
}

// ---------------------------------------------------------------------------
// Shell window / tab snapshot
// ---------------------------------------------------------------------------
struct TabRecord {
    HWND                 frame = nullptr;
    HWND                 tabWnd = nullptr;
    int                  indexInFrame = 0;
    std::wstring         path;  // normalized
    ComPtr<IDispatch>    disp;
    ComPtr<IWebBrowser2> browser;

    TabRecord() = default;
    TabRecord(TabRecord&&) = default;
    TabRecord& operator=(TabRecord&&) = default;
    TabRecord(const TabRecord&) = delete;
    TabRecord& operator=(const TabRecord&) = delete;
};

struct FrameRecord {
    HWND               frame = nullptr;
    int                zOrder = INT_MAX;
    HWND               activeTabWnd = nullptr;
    bool               activePathRedirectable = false;
    std::vector<TabRecord> tabs;
};

static FrameRecord* FindFrameRecord(std::vector<FrameRecord>& frames,
                                    HWND frame) {
    for (auto& record : frames) {
        if (record.frame == frame)
            return &record;
    }
    return nullptr;
}

// Enumerates IShellWindows once and groups entries by frame.  The collection
// order within a frame is the tab-bar order (measured on build 26300), which
// is what 0xA221's 1-based index refers to.
//
// Filters:
//   - same process only: IShellBrowser/IFolderView don't marshal reliably
//     across processes;
//   - visible and not a phantom (the transparent duplicate being redirected);
//   - CabinetWClass top-level windows only.
static bool EnumerateFrames(IShellWindows* shellWindows, HWND excludeFrame,
                            std::vector<FrameRecord>& frames) {
    frames.clear();
    if (!shellWindows)
        return false;

    long count = 0;
    if (FAILED(shellWindows->get_Count(&count)))
        return false;

    for (long i = 0; i < count; i++) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        ComPtr<IDispatch> disp;
        if (FAILED(shellWindows->Item(index, disp.Put())) || !disp)
            continue;

        ComPtr<IWebBrowser2> browser;
        if (FAILED(disp->QueryInterface(IID_PPV_ARGS(browser.Put()))))
            continue;

        SHANDLE_PTR hwndValue = 0;
        if (FAILED(browser->get_HWND(&hwndValue)))
            continue;
        HWND frame = reinterpret_cast<HWND>(
            static_cast<ULONG_PTR>(hwndValue));

        if (!frame || frame == excludeFrame || !IsWindow(frame) ||
            !IsWindowVisible(frame) || IsPhantomWindow(frame) ||
            !IsSameProcessWindow(frame) || !IsCabinetClass(frame)) {
            continue;
        }

        FrameRecord* frameRecord = FindFrameRecord(frames, frame);
        if (!frameRecord) {
            frames.push_back(FrameRecord{});
            frameRecord = &frames.back();
            frameRecord->frame = frame;
            frameRecord->zOrder = GetZOrderRank(frame);
        }

        TabRecord tab;
        tab.frame = frame;
        tab.tabWnd = QueryTabWindow(disp.Get());
        tab.indexInFrame = static_cast<int>(frameRecord->tabs.size());
        tab.disp = std::move(disp);
        tab.browser = std::move(browser);

        bool fromUrl = false;
        tab.path = NormalizePath(
            GetBrowserPath(tab.disp.Get(), tab.browser.Get(), &fromUrl));
        if (!fromUrl && !tab.path.empty()) {
            // A PIDL-only path is only trustworthy after loading completes;
            // while a tab is navigating it can still be the old/default
            // location.  Treat it as unknown for matching purposes.
            READYSTATE readyState = READYSTATE_LOADING;
            tab.browser->get_ReadyState(&readyState);
            if (readyState != READYSTATE_COMPLETE)
                tab.path.clear();
        }
        frameRecord->tabs.push_back(std::move(tab));
    }

    for (auto& frameRecord : frames) {
        frameRecord.activeTabWnd = GetActiveTabWindow(frameRecord.frame);

        // The existing window is only a valid destination if its active tab
        // is a normal folder, matching the old mod's Control Panel exclusion.
        // If the active tab path can't be read, fall back to any redirectable
        // tab in the window.
        for (const auto& tab : frameRecord.tabs) {
            if (tab.tabWnd && tab.tabWnd == frameRecord.activeTabWnd &&
                IsRedirectablePath(tab.path)) {
                frameRecord.activePathRedirectable = true;
                break;
            }
        }
        if (!frameRecord.activePathRedirectable) {
            for (const auto& tab : frameRecord.tabs) {
                if (IsRedirectablePath(tab.path)) {
                    frameRecord.activePathRedirectable = true;
                    break;
                }
            }
        }
    }

    return true;
}

// Finds the entry of a specific tab in IShellWindows and returns its browser.
// Used after a new tab window has been captured by handle.
static bool FindTabBrowser(IShellWindows* shellWindows, HWND frame,
                           HWND tabWnd, DWORD timeoutMs, TabRecord& out) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;

    for (;;) {
        if (g_unloading || !IsWindow(frame) || !IsWindow(tabWnd))
            return false;

        long count = 0;
        if (FAILED(shellWindows->get_Count(&count)))
            return false;

        for (long i = 0; i < count; i++) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;

            ComPtr<IDispatch> disp;
            if (FAILED(shellWindows->Item(index, disp.Put())) || !disp)
                continue;

            ComPtr<IWebBrowser2> browser;
            if (FAILED(disp->QueryInterface(IID_PPV_ARGS(browser.Put()))))
                continue;

            SHANDLE_PTR hwndValue = 0;
            if (FAILED(browser->get_HWND(&hwndValue)))
                continue;
            HWND entryFrame = reinterpret_cast<HWND>(
                static_cast<ULONG_PTR>(hwndValue));
            if (entryFrame != frame)
                continue;

            if (QueryTabWindow(disp.Get()) != tabWnd)
                continue;

            out.frame = frame;
            out.tabWnd = tabWnd;
            out.disp = std::move(disp);
            out.browser = std::move(browser);
            out.path = NormalizePath(
                GetBrowserPath(out.disp.Get(), out.browser.Get()));
            return true;
        }

        if (GetTickCount64() >= deadline)
            return false;
        Sleep(kPollFastMs);
    }
}

// Returns the 0-based tab-bar index of tabWnd within frame, or -1.  The index
// is derived from the IShellWindows collection order, which matches the tab-bar
// order on the builds this mod targets.
static int FindTabIndex(IShellWindows* shellWindows, HWND frame, HWND tabWnd) {
    if (!shellWindows || !frame || !tabWnd)
        return -1;

    long count = 0;
    if (FAILED(shellWindows->get_Count(&count)))
        return -1;

    int index = 0;
    for (long i = 0; i < count; i++) {
        VARIANT varIndex;
        VariantInit(&varIndex);
        varIndex.vt = VT_I4;
        varIndex.lVal = i;

        ComPtr<IDispatch> disp;
        if (FAILED(shellWindows->Item(varIndex, disp.Put())) || !disp)
            continue;

        ComPtr<IWebBrowser2> browser;
        if (FAILED(disp->QueryInterface(IID_PPV_ARGS(browser.Put()))))
            continue;

        SHANDLE_PTR hwndValue = 0;
        if (FAILED(browser->get_HWND(&hwndValue)))
            continue;
        HWND entryFrame = reinterpret_cast<HWND>(
            static_cast<ULONG_PTR>(hwndValue));
        if (entryFrame != frame)
            continue;

        HWND entryTab = QueryTabWindow(disp.Get());
        if (entryTab == tabWnd)
            return index;
        index++;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Navigation / activation
// ---------------------------------------------------------------------------
static bool NavigateBrowser(IWebBrowser2* browser, const std::wstring& path) {
    if (!browser || path.empty())
        return false;

    VARIANT target;
    VariantInit(&target);
    VARIANT empty;
    VariantInit(&empty);

    HRESULT hr = E_FAIL;

    if (IsClsidPath(path)) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0,
                                         nullptr)) &&
            pidl) {
            UINT pidlSize = ILGetSize(pidl);
            SAFEARRAY* array = SafeArrayCreateVector(VT_UI1, 0, pidlSize);
            if (array) {
                void* data = nullptr;
                if (SUCCEEDED(SafeArrayAccessData(array, &data))) {
                    memcpy(data, pidl, pidlSize);
                    SafeArrayUnaccessData(array);

                    target.vt = VT_ARRAY | VT_UI1;
                    target.parray = array;
                    hr = browser->Navigate2(&target, &empty, &empty, &empty,
                                            &empty);
                }
                SafeArrayDestroy(array);
            }
            CoTaskMemFree(pidl);
        }
    }

    if (FAILED(hr)) {
        // Regular filesystem paths, file URLs, and http(s) URLs are all fine
        // as BSTR targets.  This also serves as a fallback for CLSID strings
        // when PIDL parsing fails.
        VariantInit(&target);
        target.vt = VT_BSTR;
        target.bstrVal = SysAllocString(path.c_str());
        if (target.bstrVal) {
            hr = browser->Navigate2(&target, &empty, &empty, &empty, &empty);
            SysFreeString(target.bstrVal);
        }
    }

    Wh_Log(L"[est-v2] Navigate2 path='%s' hr=0x%08X", path.c_str(), hr);
    return SUCCEEDED(hr);
}

enum class ActiveCheck {
    Yes,
    No,
    Unknown,
};

// Checks whether tabWnd is the active tab.  The frame title starts with the
// active tab's own window title; this works even for minimized windows.
// If the title can't be matched at all, return Unknown and let the caller
// trust a successful internal command instead of falling back unnecessarily.
static ActiveCheck CheckActiveTab(HWND frame, HWND tabWnd, DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool everSawActive = false;

    for (;;) {
        if (!IsWindow(frame) || !IsWindow(tabWnd))
            return ActiveCheck::No;

        HWND active = GetActiveTabWindow(frame);
        if (active) {
            everSawActive = true;
            if (active == tabWnd)
                return ActiveCheck::Yes;
        }

        if (GetTickCount64() >= deadline)
            return everSawActive ? ActiveCheck::No : ActiveCheck::Unknown;
        if (g_unloading)
            return ActiveCheck::Unknown;
        Sleep(5);
    }
}

// Waits until a (possibly background) tab has committed its navigation to the
// target path, so switching to it won't briefly show the tab's previous
// Home/This PC page.  Returns false on timeout, in which case the caller
// switches anyway.
static bool WaitForBrowserTarget(IWebBrowser2* browser,
                                 const std::wstring& targetPath,
                                 DWORD timeoutMs) {
    if (!browser || targetPath.empty())
        return false;

    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        BSTR url = nullptr;
        if (SUCCEEDED(browser->get_LocationURL(&url)) && url && *url) {
            WCHAR filePath[MAX_PATH * 2];
            DWORD length = ARRAYSIZE(filePath);
            std::wstring path;
            if (SUCCEEDED(PathCreateFromUrlW(url, filePath, &length, 0)))
                path = filePath;
            else
                path = url;
            SysFreeString(url);

            if (SamePath(NormalizePath(path), targetPath))
                return true;
        } else if (url) {
            SysFreeString(url);
        }

        READYSTATE readyState = READYSTATE_LOADING;
        browser->get_ReadyState(&readyState);
        if (readyState == READYSTATE_COMPLETE)
            return true;  // special locations may not expose a comparable URL

        if (GetTickCount64() >= deadline || g_unloading)
            return false;
        Sleep(kPollSlowMs);
    }
}

// The internal command takes a 1-based index and only works for indices 1..9;
// out-of-range values are ignored (verified: lParam=9 on a 3-tab window did
// nothing).  Index 8 therefore only works when there are exactly 9 tabs.
// Everything else uses Navigate2, which always activates the tab but reloads
// the view.
static bool CanUseSelectCommand(int index, int totalTabs) {
    if (index < 0 || totalTabs <= 0)
        return false;
    if (index < 8)
        return true;
    return index == 8 && totalTabs == 9;
}

static bool SendSelectTabCommand(HWND frame, int index) {
    DWORD_PTR result = 0;
    SetLastError(0);
    LRESULT delivered =
        SendMessageTimeoutW(frame, WM_COMMAND,
                            static_cast<WPARAM>(kCmdSelectTab),
                            static_cast<LPARAM>(index + 1),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result);
    bool ok = delivered != 0 || GetLastError() != ERROR_TIMEOUT;
    Wh_Log(L"[est-v2] select tab frame=%p index=%d ok=%d result=%p", frame,
           index, ok ? 1 : 0, reinterpret_cast<void*>(result));
    return ok;
}

// Only closes a tab that is currently active.  The internal close command is
// known to close the active tab; refusing to send it for a background tab
// guarantees that a failed redirect can never close one of the user's tabs,
// even if the command's targeting changes in a future Windows build.  A rare
// failed redirect may leave our blank tab behind instead.
static void CloseTabWindow(HWND tabWnd) {
    if (!tabWnd || !IsWindow(tabWnd))
        return;

    HWND frame = GetParent(tabWnd);
    if (!frame || GetActiveTabWindow(frame) != tabWnd) {
        Wh_Log(L"[est-v2] not closing background tab %p", tabWnd);
        return;
    }

    DWORD_PTR result = 0;
    SendMessageTimeoutW(tabWnd, WM_COMMAND,
                        static_cast<WPARAM>(kCmdCloseTab),
                        static_cast<LPARAM>(1),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result);
    Wh_Log(L"[est-v2] close tab %p result=0x%llX", tabWnd,
           static_cast<unsigned long long>(result));
}

// Best-effort cleanup for a tab that we created but failed to use.  If it is
// currently active it can be closed directly; otherwise try to activate it
// first (the new tab is normally the last one, so its index is selectable when
// the window has at most 9 tabs).  Never closes a tab that is not confirmed to
// be ours/active.
static void CloseNewTabSafely(IShellWindows* shellWindows, HWND frame,
                              HWND tabWnd) {
    if (!IsWindow(tabWnd) || !IsWindow(frame))
        return;

    if (GetActiveTabWindow(frame) == tabWnd) {
        CloseTabWindow(tabWnd);
        return;
    }

    int totalTabs = static_cast<int>(SnapshotTabWindows(frame).size());
    int index = FindTabIndex(shellWindows, frame, tabWnd);
    if (index >= 0 && CanUseSelectCommand(index, totalTabs) &&
        SendSelectTabCommand(frame, index) &&
        CheckActiveTab(frame, tabWnd, 250) == ActiveCheck::Yes) {
        CloseTabWindow(tabWnd);
        return;
    }

    Wh_Log(L"[est-v2] cannot safely close new tab %p", tabWnd);
}

// ---------------------------------------------------------------------------
// Selected items
// ---------------------------------------------------------------------------
static bool QuerySelectedItems(IDispatch* disp,
                               std::vector<std::wstring>& items) {
    items.clear();
    if (!disp)
        return false;

    ComPtr<IServiceProvider> serviceProvider;
    if (FAILED(disp->QueryInterface(IID_PPV_ARGS(serviceProvider.Put()))))
        return false;

    ComPtr<IShellBrowser> shellBrowser;
    if (FAILED(serviceProvider->QueryService(
            SID_STopLevelBrowser, IID_PPV_ARGS(shellBrowser.Put()))))
        return false;

    ComPtr<IShellView> shellView;
    if (FAILED(shellBrowser->QueryActiveShellView(shellView.Put())))
        return false;

    ComPtr<IFolderView2> folderView;
    if (FAILED(shellView->QueryInterface(IID_PPV_ARGS(folderView.Put()))))
        return false;

    ComPtr<IShellItemArray> selection;
    if (FAILED(folderView->GetSelection(FALSE, selection.Put())) ||
        !selection) {
        return true;  // no selection, not an error
    }

    DWORD count = 0;
    if (FAILED(selection->GetCount(&count)))
        return false;

    for (DWORD i = 0; i < count; i++) {
        ComPtr<IShellItem> item;
        if (FAILED(selection->GetItemAt(i, item.Put())) || !item)
            continue;

        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,
                                           &name)) &&
            name) {
            items.emplace_back(name);
            CoTaskMemFree(name);
        }
    }
    return true;
}

// Reads the selection from the source (duplicate) window.  The duplicate is
// kept alive while the destination tab is prepared, so it has time to finish
// navigating; this is why selection is queried late instead of blocking the
// redirect before the tab exists.
static bool GetSourceSelection(const TabRecord& source,
                               std::vector<std::wstring>& items) {
    if (!source.disp || !source.browser)
        return false;

    ULONGLONG deadline = GetTickCount64() + kSelectionTimeoutMs;

    for (;;) {
        READYSTATE readyState = READYSTATE_LOADING;
        source.browser->get_ReadyState(&readyState);

        items.clear();
        QuerySelectedItems(source.disp.Get(), items);
        if (!items.empty())
            return true;

        if (readyState == READYSTATE_COMPLETE)
            return true;  // ready and genuinely no selection
        if (GetTickCount64() >= deadline || g_unloading)
            return false;

        Sleep(kPollSlowMs);
    }
}

static bool SelectItemsInTab(const TabRecord& target,
                             const std::wstring& folderPath,
                             const std::vector<std::wstring>& itemPaths) {
    if (itemPaths.empty())
        return true;
    if (!target.disp || !target.browser)
        return false;

    PIDLIST_ABSOLUTE folderPidl = nullptr;
    if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &folderPidl, 0,
                                  nullptr)) ||
        !folderPidl) {
        Wh_Log(L"[est-v2] cannot parse folder path for selection");
        return false;
    }

    std::vector<PIDLIST_ABSOLUTE> absolutePidls;
    std::vector<PCUITEMID_CHILD> childPidls;

    for (const auto& itemPath : itemPaths) {
        PIDLIST_ABSOLUTE itemPidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(itemPath.c_str(), nullptr, &itemPidl,
                                         0, nullptr)) &&
            itemPidl) {
            PCUITEMID_CHILD child = ILFindChild(folderPidl, itemPidl);
            if (child) {
                absolutePidls.push_back(itemPidl);
                childPidls.push_back(child);
            } else {
                CoTaskMemFree(itemPidl);
            }
        }
    }

    if (childPidls.empty()) {
        CoTaskMemFree(folderPidl);
        Wh_Log(L"[est-v2] no child PIDLs for selection");
        return false;
    }

    ULONGLONG deadline = GetTickCount64() + kSelectionTimeoutMs;
    bool success = false;

    for (;;) {
        READYSTATE readyState = READYSTATE_LOADING;
        target.browser->get_ReadyState(&readyState);

        if (readyState == READYSTATE_COMPLETE) {
            ComPtr<IServiceProvider> serviceProvider;
            if (SUCCEEDED(target.disp->QueryInterface(
                    IID_PPV_ARGS(serviceProvider.Put())))) {
                ComPtr<IShellBrowser> shellBrowser;
                if (SUCCEEDED(serviceProvider->QueryService(
                        SID_STopLevelBrowser,
                        IID_PPV_ARGS(shellBrowser.Put())))) {
                    ComPtr<IShellView> shellView;
                    if (SUCCEEDED(shellBrowser->QueryActiveShellView(
                            shellView.Put()))) {
                        ComPtr<IFolderView> folderView;
                        if (SUCCEEDED(shellView->QueryInterface(
                                IID_PPV_ARGS(folderView.Put())))) {
                            HRESULT hr = folderView->SelectAndPositionItems(
                                static_cast<UINT>(childPidls.size()),
                                childPidls.data(), nullptr,
                                SVSI_SELECT | SVSI_DESELECTOTHERS |
                                    SVSI_FOCUSED | SVSI_ENSUREVISIBLE);
                            if (SUCCEEDED(hr)) {
                                Wh_Log(L"[est-v2] selected %zu items",
                                       childPidls.size());
                                success = true;
                            } else {
                                Wh_Log(L"[est-v2] SelectAndPositionItems "
                                       L"failed hr=0x%08X",
                                       hr);
                            }
                        }
                    }
                }
            }
        }

        if (success || GetTickCount64() >= deadline || g_unloading)
            break;
        Sleep(kPollSlowMs);
    }

    for (auto pidl : absolutePidls)
        CoTaskMemFree(pidl);
    CoTaskMemFree(folderPidl);
    return success;
}

// ---------------------------------------------------------------------------
// Restore / close a phantom window
// ---------------------------------------------------------------------------
static void RestoreNewWindow(HWND hwnd) {
    int showCmd = RemovePhantom(hwnd);
    if (!hwnd || !IsWindow(hwnd))
        return;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_LAYERED) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          exStyle & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    if (!ShowWindow_Original)
        return;
    if (showCmd)
        ShowWindow_Original(hwnd, showCmd);
    else if (!IsWindowVisible(hwnd))
        ShowWindow_Original(hwnd, SW_SHOW);
}

static void CloseRedirectedWindow(HWND hwnd) {
    MarkPhantomRedirected(hwnd);
    if (IsWindow(hwnd))
        PostMessageW(hwnd, WM_CLOSE, 0, 0);

    // Give the closing window a short chance to disappear, then drop dead
    // phantom entries so the ShowWindow fast path stays useful.  This wait is
    // after the user-visible work and only affects the background thread.
    ULONGLONG deadline = GetTickCount64() + kPruneWaitMs;
    while (IsWindow(hwnd) && GetTickCount64() < deadline && !g_unloading)
        Sleep(5);
    PrunePhantoms();
}

// ---------------------------------------------------------------------------
// Choosing a destination window
// ---------------------------------------------------------------------------
static FrameRecord* PickPrimaryFrame(std::vector<FrameRecord>& frames,
                                     HWND foregroundAtCreate) {
    if (foregroundAtCreate) {
        for (auto& frame : frames) {
            if (frame.frame == foregroundAtCreate &&
                frame.activePathRedirectable) {
                return &frame;
            }
        }
    }

    FrameRecord* best = nullptr;
    for (auto& frame : frames) {
        if (!frame.activePathRedirectable)
            continue;
        if (!best || frame.zOrder < best->zOrder)
            best = &frame;
    }
    return best;
}

// Finds the destination window that already contains targetPath.  Returns:
//   NoMatch  - no tab with that path exists;
//   Switched - the tab is now active;
//   Failed   - a matching tab exists but could not be activated; the caller
//              should create a new tab in matchedFrame.
enum class ReuseResult {
    NoMatch,
    Switched,
    Failed,
};

static ReuseResult TryReuseExistingTab(std::vector<FrameRecord>& frames,
                                       const std::wstring& targetPath,
                                       HWND preferredFrame,
                                       HWND& matchedFrame,
                                       TabRecord& matchedTab) {
    matchedFrame = nullptr;

    std::vector<FrameRecord*> order;
    order.reserve(frames.size());
    bool hasPreferred = false;
    if (preferredFrame) {
        for (auto& frame : frames) {
            if (frame.frame == preferredFrame) {
                order.push_back(&frame);
                hasPreferred = true;
                break;
            }
        }
    }
    for (auto& frame : frames) {
        if (hasPreferred && frame.frame == preferredFrame)
            continue;
        order.push_back(&frame);
    }
    // Prefer topmost windows for the second pass.
    size_t sortStart = hasPreferred ? 1 : 0;
    for (size_t i = sortStart; i < order.size(); i++) {
        for (size_t j = i; j > sortStart &&
                           order[j]->zOrder < order[j - 1]->zOrder;
             j--) {
            std::swap(order[j], order[j - 1]);
        }
    }

    for (FrameRecord* frame : order) {
        if (!frame || !IsWindow(frame->frame) || IsPhantomWindow(frame->frame))
            continue;

        for (auto& tab : frame->tabs) {
            if (!tab.tabWnd || !tab.browser || tab.path.empty())
                continue;
            if (!SamePath(tab.path, targetPath))
                continue;

            if (!matchedFrame)
                matchedFrame = frame->frame;

            if (!IsWindow(tab.tabWnd))
                continue;

            std::vector<HWND> tabWindows = SnapshotTabWindows(frame->frame);
            int totalTabs = static_cast<int>(tabWindows.size());

            bool switched = false;
            std::wstring mode;

            if (CanUseSelectCommand(tab.indexInFrame, totalTabs) &&
                SendSelectTabCommand(frame->frame, tab.indexInFrame)) {
                ActiveCheck check =
                    CheckActiveTab(frame->frame, tab.tabWnd, kSwitchVerifyMs);
                if (check != ActiveCheck::No) {
                    switched = true;
                    mode = check == ActiveCheck::Yes ? L"command"
                                                     : L"command-unverified";
                }
            }

            if (!switched) {
                if (NavigateBrowser(tab.browser.Get(), targetPath)) {
                    ActiveCheck check = CheckActiveTab(
                        frame->frame, tab.tabWnd, kNavigateVerifyMs);
                    if (check != ActiveCheck::No) {
                        switched = true;
                        mode = check == ActiveCheck::Yes ? L"navigate"
                                                         : L"navigate-unverified";
                    }
                }
            }

            if (switched) {
                Wh_Log(L"[est-v2] reuse tab frame=%p tab=%p index=%d mode=%s",
                       frame->frame, tab.tabWnd, tab.indexInFrame,
                       mode.c_str());
                matchedTab = std::move(tab);
                matchedFrame = frame->frame;
                return ReuseResult::Switched;
            }

            Wh_Log(L"[est-v2] reuse match but activation failed frame=%p "
                   L"tab=%p index=%d",
                   frame->frame, tab.tabWnd, tab.indexInFrame);
            // Remember the frame for the "new tab + selection" fallback, but
            // keep looking in case another matching tab can be activated.
            matchedFrame = frame->frame;
        }
    }

    return matchedFrame ? ReuseResult::Failed : ReuseResult::NoMatch;
}

// ---------------------------------------------------------------------------
// Waiting for the duplicate window's path
// ---------------------------------------------------------------------------
static bool WaitForNewWindowPath(IShellWindows* shellWindows, HWND newWindow,
                                 std::wstring& path, TabRecord& source) {
    ULONGLONG deadline = GetTickCount64() + kPathTimeoutMs;

    for (;;) {
        if (!IsWindow(newWindow))
            return false;

        long count = 0;
        if (SUCCEEDED(shellWindows->get_Count(&count))) {
            for (long i = 0; i < count; i++) {
                VARIANT index;
                VariantInit(&index);
                index.vt = VT_I4;
                index.lVal = i;

                ComPtr<IDispatch> disp;
                if (FAILED(shellWindows->Item(index, disp.Put())) || !disp)
                    continue;

                ComPtr<IWebBrowser2> browser;
                if (FAILED(disp->QueryInterface(
                        IID_PPV_ARGS(browser.Put()))))
                    continue;

                SHANDLE_PTR hwndValue = 0;
                if (FAILED(browser->get_HWND(&hwndValue)))
                    continue;
                HWND frame = reinterpret_cast<HWND>(
                    static_cast<ULONG_PTR>(hwndValue));
                if (frame != newWindow)
                    continue;

                bool fromUrl = false;
                source.frame = newWindow;
                source.tabWnd = QueryTabWindow(disp.Get());
                source.disp = std::move(disp);
                source.browser = std::move(browser);
                source.path = NormalizePath(GetBrowserPath(
                    source.disp.Get(), source.browser.Get(), &fromUrl));

                // A PIDL-only path can be the default Home/This PC location
                // while the real navigation is still pending.  Only trust it
                // once the browser is done loading.
                bool usable = !source.path.empty();
                if (usable && !fromUrl) {
                    READYSTATE readyState = READYSTATE_LOADING;
                    source.browser->get_ReadyState(&readyState);
                    usable = readyState == READYSTATE_COMPLETE;
                }

                if (usable) {
                    path = source.path;
                    return true;
                }

                // The entry exists but the path isn't ready yet; keep the
                // browser reference for selection later and keep polling.
                break;
            }
        }

        if (GetTickCount64() >= deadline || g_unloading)
            return false;
        Sleep(kPollFastMs);
    }
}

// ---------------------------------------------------------------------------
// New tab creation
// ---------------------------------------------------------------------------
static HWND WaitForNewTabWindow(HWND frame,
                                const std::vector<HWND>& beforeTabs,
                                DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;

    for (;;) {
        std::vector<HWND> tabs = SnapshotTabWindows(frame);
        for (HWND tab : tabs) {
            if (!ContainsWindow(beforeTabs, tab))
                return tab;
        }

        if (GetTickCount64() >= deadline || g_unloading)
            return nullptr;
        Sleep(kPollFastMs);
    }
}

static bool CreateAndNavigateNewTab(IShellWindows* shellWindows, HWND frame,
                                    const std::wstring& path,
                                    TabRecord& newTab) {
    if (!IsWindow(frame) || !IsCabinetClass(frame))
        return false;

    // Keep this tab visible while the new tab is being prepared.  Without
    // this, the newly created tab can become active immediately and show its
    // default Home/This PC page until Navigate2 commits.
    HWND previousActive = GetActiveTabWindow(frame);

    EnterCriticalSection(&g_tabCreateLock);

    std::vector<HWND> beforeTabs = SnapshotTabWindows(frame);
    if (beforeTabs.empty()) {
        LeaveCriticalSection(&g_tabCreateLock);
        Wh_Log(L"[est-v2] no tab window to post 0xA21B");
        return false;
    }

    // Any existing tab window accepts the "new tab" command.
    if (!PostMessageW(beforeTabs[0], WM_COMMAND,
                      static_cast<WPARAM>(kCmdNewTab), 0)) {
        LeaveCriticalSection(&g_tabCreateLock);
        Wh_Log(L"[est-v2] PostMessage 0xA21B failed");
        return false;
    }

    HWND newTabWnd = WaitForNewTabWindow(frame, beforeTabs,
                                         kTabWindowTimeoutMs);
    if (!newTabWnd) {
        LeaveCriticalSection(&g_tabCreateLock);
        Wh_Log(L"[est-v2] new tab window not detected");
        return false;
    }

    // The handle is now unique and ours; later redirects can proceed.
    LeaveCriticalSection(&g_tabCreateLock);

    int totalTabs = static_cast<int>(SnapshotTabWindows(frame).size());
    int previousIndex =
        (previousActive && previousActive != newTabWnd)
            ? FindTabIndex(shellWindows, frame, previousActive)
            : -1;
    bool canHideUntilReady =
        previousIndex >= 0 && CanUseSelectCommand(previousIndex, totalTabs);
    if (canHideUntilReady) {
        // If 0xA21B activated the new tab, switch back right away.
        SendSelectTabCommand(frame, previousIndex);
    }

    if (!FindTabBrowser(shellWindows, frame, newTabWnd,
                        kTabBrowserTimeoutMs, newTab)) {
        Wh_Log(L"[est-v2] browser for new tab %p not found", newTabWnd);
        CloseNewTabSafely(shellWindows, frame, newTabWnd);
        return false;
    }

    if (!NavigateBrowser(newTab.browser.Get(), path)) {
        Wh_Log(L"[est-v2] navigate new tab failed");
        CloseNewTabSafely(shellWindows, frame, newTabWnd);
        return false;
    }

    // Navigate2 activates the tab.  If we can switch tabs with 0xA221, keep
    // the previous tab visible until the new tab has committed the target
    // path, then switch to it.  This makes the new tab appear already at the
    // destination instead of briefly flashing Home/This PC.
    int newIndex = FindTabIndex(shellWindows, frame, newTabWnd);
    if (canHideUntilReady && newIndex >= 0 && newIndex != previousIndex &&
        CanUseSelectCommand(newIndex, totalTabs)) {
        SendSelectTabCommand(frame, previousIndex);
        bool committed = WaitForBrowserTarget(newTab.browser.Get(), path,
                                              kBackgroundActivateWaitMs);
        SendSelectTabCommand(frame, newIndex);
        if (CheckActiveTab(frame, newTabWnd, 250) == ActiveCheck::No) {
            // The switch-in didn't stick (or the index was stale); an extra
            // Navigate2 is the guaranteed activation fallback.
            NavigateBrowser(newTab.browser.Get(), path);
        }
        Wh_Log(L"[est-v2] new tab activated after target commit=%d",
               committed ? 1 : 0);
    }

    Wh_Log(L"[est-v2] new tab frame=%p tab=%p path='%s'", frame, newTabWnd,
           path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Redirect thread
// ---------------------------------------------------------------------------
static bool HasOtherExplorerFrame(IShellWindows* shellWindows,
                                  HWND excludeFrame) {
    long count = 0;
    if (FAILED(shellWindows->get_Count(&count)))
        return false;

    for (long i = 0; i < count; i++) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        ComPtr<IDispatch> disp;
        if (FAILED(shellWindows->Item(index, disp.Put())) || !disp)
            continue;

        ComPtr<IWebBrowser2> browser;
        if (FAILED(disp->QueryInterface(IID_PPV_ARGS(browser.Put()))))
            continue;

        SHANDLE_PTR hwndValue = 0;
        if (FAILED(browser->get_HWND(&hwndValue)))
            continue;
        HWND frame = reinterpret_cast<HWND>(
            static_cast<ULONG_PTR>(hwndValue));

        if (!frame || frame == excludeFrame || !IsWindow(frame) ||
            !IsWindowVisible(frame) || IsPhantomWindow(frame) ||
            !IsSameProcessWindow(frame) || !IsCabinetClass(frame)) {
            continue;
        }
        return true;
    }
    return false;
}

struct ThreadCountGuard {
    ~ThreadCountGuard() { InterlockedDecrement(&g_activeThreads); }
};

static DWORD WINAPI RedirectThread(LPVOID parameter) {
    ThreadCountGuard threadGuard;
    auto* info = static_cast<RedirectInfo*>(parameter);
    HWND newWindow = info->newWindow;
    HWND foregroundAtCreate = info->foregroundAtCreate;
    delete info;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        Wh_Log(L"[est-v2] CoInitializeEx failed");
        RestoreNewWindow(newWindow);
        return 0;
    }

    struct CoUninitGuard {
        ~CoUninitGuard() { CoUninitialize(); }
    } coUninitGuard;

    Wh_Log(L"[est-v2] redirect start new=%p fg=%p", newWindow,
           foregroundAtCreate);

    ComPtr<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(shellWindows.Put())))) {
        Wh_Log(L"[est-v2] CLSID_ShellWindows failed");
        RestoreNewWindow(newWindow);
        return 0;
    }

    // Fast path for the first window / Explorer restart: if there is no
    // other Explorer window, show this one normally instead of waiting for a
    // path that will never be redirected.
    if (!HasOtherExplorerFrame(shellWindows.Get(), newWindow)) {
        Wh_Log(L"[est-v2] no other Explorer window, restore");
        RestoreNewWindow(newWindow);
        return 0;
    }

    // Phase 1: read the duplicate window's path.  Keep its per-tab COM
    // objects: the window stays alive until the destination tab is ready, so
    // the selected items can be read after navigation has completed.
    std::wstring path;
    TabRecord sourceTab;
    if (!WaitForNewWindowPath(shellWindows.Get(), newWindow, path,
                              sourceTab)) {
        Wh_Log(L"[est-v2] path not available, restore");
        RestoreNewWindow(newWindow);
        return 0;
    }

    if (!IsRedirectablePath(path)) {
        Wh_Log(L"[est-v2] non-redirectable path '%s', restore", path.c_str());
        RestoreNewWindow(newWindow);
        return 0;
    }

    // Phase 2: snapshot all eligible tabs once.
    std::vector<FrameRecord> frames;
    if (!EnumerateFrames(shellWindows.Get(), newWindow, frames) ||
        frames.empty()) {
        Wh_Log(L"[est-v2] no eligible Explorer frames, restore");
        RestoreNewWindow(newWindow);
        return 0;
    }

    FrameRecord* primary = PickPrimaryFrame(frames, foregroundAtCreate);

    // Phase 3: reuse an existing tab if possible.
    if (g_reuseExistingTab) {
        HWND matchedFrame = nullptr;
        TabRecord matchedTab;
        ReuseResult reuse = TryReuseExistingTab(
            frames, path, primary ? primary->frame : nullptr, matchedFrame,
            matchedTab);

        if (reuse == ReuseResult::Switched) {
            HWND frame = matchedFrame;
            BringFrameToForeground(frame);

            std::vector<std::wstring> selectedItems;
            if (GetSourceSelection(sourceTab, selectedItems) &&
                !selectedItems.empty()) {
                SelectItemsInTab(matchedTab, path, selectedItems);
            }

            CloseRedirectedWindow(newWindow);
            Wh_Log(L"[est-v2] done reuse frame=%p path='%s'", frame,
                   path.c_str());
            return 0;
        }

        if (reuse == ReuseResult::Failed && matchedFrame) {
            // The user asked for "new tab + selection" when switching an
            // existing tab isn't possible.
            TabRecord newTab;
            if (CreateAndNavigateNewTab(shellWindows.Get(), matchedFrame,
                                        path, newTab)) {
                BringFrameToForeground(matchedFrame);

                std::vector<std::wstring> selectedItems;
                if (GetSourceSelection(sourceTab, selectedItems) &&
                    !selectedItems.empty()) {
                    SelectItemsInTab(newTab, path, selectedItems);
                }

                CloseRedirectedWindow(newWindow);
                Wh_Log(L"[est-v2] done new-tab-after-reuse-failure path='%s'",
                       path.c_str());
                return 0;
            }

            Wh_Log(L"[est-v2] new tab after reuse failure failed, restore");
            RestoreNewWindow(newWindow);
            return 0;
        }
    }

    // Phase 4: create a new tab in the best existing window.
    if (!primary) {
        Wh_Log(L"[est-v2] no primary window, restore");
        RestoreNewWindow(newWindow);
        return 0;
    }

    TabRecord newTab;
    if (!CreateAndNavigateNewTab(shellWindows.Get(), primary->frame, path,
                                 newTab)) {
        Wh_Log(L"[est-v2] new tab creation failed, restore");
        RestoreNewWindow(newWindow);
        return 0;
    }

    BringFrameToForeground(primary->frame);

    // Phase 5: restore the "show in folder" selection in the destination tab.
    std::vector<std::wstring> selectedItems;
    if (GetSourceSelection(sourceTab, selectedItems) &&
        !selectedItems.empty()) {
        SelectItemsInTab(newTab, path, selectedItems);
    }

    // Phase 6: only now is it safe to close the duplicate.
    CloseRedirectedWindow(newWindow);
    Wh_Log(L"[est-v2] done new-tab frame=%p path='%s'", primary->frame,
           path.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static bool StartRedirectThread(HWND hwnd, HWND foregroundAtCreate) {
    auto* info = new (std::nothrow) RedirectInfo{hwnd, foregroundAtCreate};
    if (!info)
        return false;

    EnterCriticalSection(&g_lifecycleLock);
    if (g_unloading) {
        LeaveCriticalSection(&g_lifecycleLock);
        delete info;
        return false;
    }

    InterlockedIncrement(&g_activeThreads);
    HANDLE thread =
        CreateThread(nullptr, 0, RedirectThread, info, 0, nullptr);
    if (!thread) {
        InterlockedDecrement(&g_activeThreads);
        LeaveCriticalSection(&g_lifecycleLock);
        delete info;
        return false;
    }
    CloseHandle(thread);
    LeaveCriticalSection(&g_lifecycleLock);
    return true;
}

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName, DWORD dwStyle, int x,
                                 int y, int width, int height,
                                 HWND hWndParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID lpParam) {
    if (g_unloading)
        return CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                        dwStyle, x, y, width, height,
                                        hWndParent, hMenu, hInstance, lpParam);

    bool isCabinet =
        lpClassName && !IS_INTRESOURCE(lpClassName) &&
        wcscmp(lpClassName, kCabinetClass) == 0 && !(dwStyle & WS_CHILD);
    if (!isCabinet) {
        return CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                        dwStyle, x, y, width, height,
                                        hWndParent, hMenu, hInstance, lpParam);
    }

    // Shift+open keeps the normal "new window" behavior.
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        return CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                        dwStyle, x, y, width, height,
                                        hWndParent, hMenu, hInstance, lpParam);
    }

    // Drop entries for windows that are already gone before testing phantom
    // membership; an HWND can be reused by the window system.
    PrunePhantoms();

    // First window / no other Explorer window: don't hide it at all.  This is
    // a cheap Win32-only check (no COM on the UI thread); it also covers the
    // "all windows closed, open a new one" case, where the window must show
    // normally.
    if (!HasVisibleCabinetWindow()) {
        return CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                        dwStyle, x, y, width, height,
                                        hWndParent, hMenu, hInstance, lpParam);
    }

    HWND foregroundAtCreate = GetForegroundWindow();

    t_inCabinetCreate = true;
    t_pendingShowHwnd = nullptr;
    t_pendingShowCmd = 0;

    HWND hwnd = CreateWindowExW_Original(
        dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, width, height,
        hWndParent, hMenu, hInstance, lpParam);

    t_inCabinetCreate = false;

    if (!hwnd)
        return hwnd;

    // If the mod is being unloaded while the window is being created, any
    // ShowWindow call we blocked during WM_CREATE must be replayed; otherwise
    // the window could stay hidden with no redirect thread left to restore it.
    if (g_unloading) {
        if (t_pendingShowHwnd == hwnd && t_pendingShowCmd &&
            ShowWindow_Original) {
            ShowWindow_Original(hwnd, t_pendingShowCmd);
        }
        return hwnd;
    }

    // Transparent but still WS_VISIBLE: the shell must register the window in
    // IShellWindows so its path and selection can be read.
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);

    int showCmd = (t_pendingShowHwnd == hwnd) ? t_pendingShowCmd : 0;
    AddPhantom(hwnd, showCmd);

    if (!StartRedirectThread(hwnd, foregroundAtCreate)) {
        Wh_Log(L"[est-v2] cannot start redirect thread for %p", hwnd);
        RestoreNewWindow(hwnd);
    }

    return hwnd;
}

BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow) {
    if (g_unloading)
        return ShowWindow_Original(hWnd, nCmdShow);

    // ShowWindow can be called from inside the original CreateWindowExW
    // before the HWND is registered as a phantom.
    if (t_inCabinetCreate && IsCabinetClass(hWnd)) {
        t_pendingShowHwnd = hWnd;
        t_pendingShowCmd = nCmdShow;
        return TRUE;
    }

    if (IsPhantomWindow(hWnd)) {
        SetPhantomShowCmd(hWnd, nCmdShow);
        return TRUE;
    }

    return ShowWindow_Original(hWnd, nCmdShow);
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------
BOOL Wh_ModInit() {
    InitializeCriticalSection(&g_phantomLock);
    InitializeCriticalSection(&g_tabCreateLock);
    InitializeCriticalSection(&g_lifecycleLock);

    g_reuseExistingTab = Wh_GetIntSetting(L"ReuseExistingTab") != 0;

    if (!Wh_SetFunctionHook(reinterpret_cast<void*>(CreateWindowExW),
                            reinterpret_cast<void*>(CreateWindowExW_Hook),
                            reinterpret_cast<void**>(
                                &CreateWindowExW_Original))) {
        Wh_Log(L"[est-v2] failed to hook CreateWindowExW");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(reinterpret_cast<void*>(ShowWindow),
                            reinterpret_cast<void*>(ShowWindow_Hook),
                            reinterpret_cast<void**>(&ShowWindow_Original))) {
        Wh_Log(L"[est-v2] failed to hook ShowWindow");
        return FALSE;
    }

    Wh_Log(L"[est-v2] loaded, reuse=%d", g_reuseExistingTab ? 1 : 0);
    return TRUE;
}

void Wh_ModUninit() {
    EnterCriticalSection(&g_lifecycleLock);
    InterlockedExchange(&g_unloading, 1);
    LeaveCriticalSection(&g_lifecycleLock);

    // Redirect threads can still be polling COM or waiting for a tab.  They
    // check g_unloading and restore their duplicate window before exiting.
    // Wait for them so the mod DLL isn't unloaded under their feet.
    for (int i = 0; i < 500; i++) {
        if (InterlockedCompareExchange(&g_activeThreads, 0, 0) == 0)
            break;
        Sleep(10);
    }

    for (HWND hwnd : GetPendingPhantoms())
        RestoreNewWindow(hwnd);

    // Intentionally not deleting the critical sections: a thread that hit
    // the timeout above may still be unwinding.
    Wh_Log(L"[est-v2] unloaded");
}
