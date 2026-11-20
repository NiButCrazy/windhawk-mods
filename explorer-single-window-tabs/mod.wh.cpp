// ==WindhawkMod==
// @id              nbc-explorer-single-window-tabs
// @name            Explorer 单窗口标签页
// @description     把新的explorer窗口重定向至新窗口
// @version         0.10
// @author          Ni But Crazy
// @github          https://github.com/almas-cp
// @homepage        https://github.com/almas-cp
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -lshlwapi -lshell32 -luuid -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*...*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <new>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define WC_CABINET   L"CabinetWClass"
#define WC_SHELLTAB  L"ShellTabWindowClass"
#define CMD_NEW_TAB  0xA21B  // Internal Explorer "new tab" command

// CLSIDs for special windows that should NOT be redirected into tabs
static const WCHAR kControlPanelCLSID[] =
    L"::{26EE0668-A00A-44D7-9371-BEB064C98683}";

static HWND g_lastPrimaryWindow = nullptr;
static thread_local bool g_creatingExplorerWindow = false;

// Forward declaration (needed by RedirectThread before ShowWindow hook)
using ShowWindow_t = decltype(&ShowWindow);
static ShowWindow_t ShowWindow_Original;

struct RedirectInfo {
    HWND newWindow;
};

// ---------------------------------------------------------------------------
// Windows created by a redirect that is still in flight.  They are made
// transparent but deliberately keep WS_VISIBLE (the shell only registers
// visible windows in IShellWindows), so they look like a normal Explorer
// window to FindExistingExplorer and must be excluded from it.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_phantomLock;
static std::vector<HWND> g_phantomWindows;

static bool IsPhantomWindow(HWND hwnd) {
    bool found = false;
    EnterCriticalSection(&g_phantomLock);
    for (auto h : g_phantomWindows) {
        if (h == hwnd) {
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_phantomLock);
    return found;
}

static void AddPhantomWindow(HWND hwnd) {
    EnterCriticalSection(&g_phantomLock);
    g_phantomWindows.push_back(hwnd);
    LeaveCriticalSection(&g_phantomLock);
}

static void RemovePhantomWindow(HWND hwnd) {
    EnterCriticalSection(&g_phantomLock);
    for (size_t i = 0; i < g_phantomWindows.size(); i++) {
        if (g_phantomWindows[i] == hwnd) {
            g_phantomWindows.erase(g_phantomWindows.begin() + i);
            break;
        }
    }
    LeaveCriticalSection(&g_phantomLock);
}

// Keeps a phantom window listed for as long as its redirect is running.
struct PhantomGuard {
    HWND hwnd;
    ~PhantomGuard() {
        if (hwnd)
            RemovePhantomWindow(hwnd);
    }
};

// ---------------------------------------------------------------------------
// Check whether a path is a normal folder that can be opened as a tab.
// Returns false for Control Panel and other special shell locations.
// ---------------------------------------------------------------------------
static bool IsRedirectablePath(const std::wstring& path) {
    if (path.find(kControlPanelCLSID) != std::wstring::npos)
        return false;

    if (path.compare(0, 9, L"shell:::{" ) == 0)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Restore a window that was hidden for redirect but needs to be shown
// (e.g. Control Panel, or abort cases).
// ---------------------------------------------------------------------------
static void AbortRedirect(HWND hwnd) {
    if (g_lastPrimaryWindow == hwnd)
        g_lastPrimaryWindow = nullptr;

    if (!IsWindow(hwnd))
        return;

    // Remove transparency and restore the window
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_LAYERED) {
        SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow_Original(hwnd, SW_SHOW);
}

// ---------------------------------------------------------------------------
// Find an existing visible File Explorer window (not Control Panel).
// Uses IShellWindows COM to check each window's actual path.
// ---------------------------------------------------------------------------
static HWND FindExistingExplorer(HWND exclude) {
    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
        return nullptr;

    long count = 0;
    pSW->get_Count(&count);

    HWND result = nullptr;
    for (long i = 0; i < count; i++) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt   = VT_I4;
        idx.lVal = i;

        IDispatch* pDisp = nullptr;
        if (FAILED(pSW->Item(idx, &pDisp)) || !pDisp)
            continue;

        IWebBrowser2* pBrowser = nullptr;
        if (SUCCEEDED(pDisp->QueryInterface(IID_PPV_ARGS(&pBrowser)))) {
            SHANDLE_PTR hwndPtr = 0;
            pBrowser->get_HWND(&hwndPtr);
            HWND hwnd =
                reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndPtr));

            if (hwnd && hwnd != exclude && IsWindowVisible(hwnd) &&
                !IsPhantomWindow(hwnd)) {
                // Get the window's path to verify it's real File Explorer
                std::wstring path;

                BSTR url = nullptr;
                if (SUCCEEDED(pBrowser->get_LocationURL(&url)) && url &&
                    wcslen(url) > 0) {
                    WCHAR filePath[MAX_PATH * 2];
                    DWORD len = MAX_PATH * 2;
                    if (SUCCEEDED(PathCreateFromUrlW(url, filePath, &len, 0)))
                        path = filePath;
                    else
                        path = url;
                    SysFreeString(url);
                } else {
                    if (url) SysFreeString(url);

                    // Fallback: PIDL path (for "This PC", etc.)
                    IServiceProvider* pSP = nullptr;
                    if (SUCCEEDED(
                            pDisp->QueryInterface(IID_PPV_ARGS(&pSP)))) {
                        IShellBrowser* pSB = nullptr;
                        if (SUCCEEDED(pSP->QueryService(
                                SID_STopLevelBrowser, IID_PPV_ARGS(&pSB)))) {
                            IShellView* pSV = nullptr;
                            if (SUCCEEDED(
                                    pSB->QueryActiveShellView(&pSV))) {
                                IFolderView* pFV = nullptr;
                                if (SUCCEEDED(pSV->QueryInterface(
                                        IID_PPV_ARGS(&pFV)))) {
                                    IPersistFolder2* pPF = nullptr;
                                    if (SUCCEEDED(pFV->GetFolder(
                                            IID_PPV_ARGS(&pPF)))) {
                                        PIDLIST_ABSOLUTE pidl = nullptr;
                                        if (SUCCEEDED(
                                                pPF->GetCurFolder(&pidl))) {
                                            PWSTR name = nullptr;
                                            if (SUCCEEDED(
                                                    SHGetNameFromIDList(
                                                        pidl,
                                                        SIGDN_DESKTOPABSOLUTEPARSING,
                                                        &name))) {
                                                path = name;
                                                CoTaskMemFree(name);
                                            }
                                            CoTaskMemFree(pidl);
                                        }
                                        pPF->Release();
                                    }
                                    pFV->Release();
                                }
                                pSV->Release();
                            }
                            pSB->Release();
                        }
                        pSP->Release();
                    }
                }

                // Only use this window if its path is redirectable
                // (i.e. not Control Panel or other special shell locations)
                if (!path.empty() && IsRedirectablePath(path)) {
                    result = hwnd;
                    pBrowser->Release();
                    pDisp->Release();
                    break;
                }
            }
            pBrowser->Release();
        }
        pDisp->Release();
    }
    pSW->Release();
    return result;
}

// ---------------------------------------------------------------------------
// COM: Extract navigation path from an Explorer window by HWND
// ---------------------------------------------------------------------------
static std::wstring GetExplorerWindowPath(IShellWindows* pSW,
                                          HWND targetHwnd) {
    std::wstring result;
    if (!pSW)
        return result;

    long count = 0;
    pSW->get_Count(&count);

    for (long i = 0; i < count; i++) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt   = VT_I4;
        idx.lVal = i;

        IDispatch* pDisp = nullptr;
        if (FAILED(pSW->Item(idx, &pDisp)) || !pDisp)
            continue;

        IWebBrowser2* pBrowser = nullptr;
        if (SUCCEEDED(pDisp->QueryInterface(IID_PPV_ARGS(&pBrowser)))) {
            SHANDLE_PTR hwndPtr = 0;
            pBrowser->get_HWND(&hwndPtr);

            if (reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndPtr)) ==
                targetHwnd) {
                // Method 1: LocationURL → filesystem path
                BSTR url = nullptr;
                if (SUCCEEDED(pBrowser->get_LocationURL(&url)) && url &&
                    wcslen(url) > 0) {
                    WCHAR path[MAX_PATH * 2];
                    DWORD pathLen = MAX_PATH * 2;
                    if (SUCCEEDED(PathCreateFromUrlW(url, path, &pathLen, 0)))
                        result = path;
                    else
                        result = url;
                    SysFreeString(url);
                } else {
                    if (url) SysFreeString(url);
                }

                // Method 2: PIDL via IShellBrowser (for special folders)
                if (result.empty()) {
                    IServiceProvider* pSP = nullptr;
                    if (SUCCEEDED(
                            pDisp->QueryInterface(IID_PPV_ARGS(&pSP)))) {
                        IShellBrowser* pSB = nullptr;
                        if (SUCCEEDED(pSP->QueryService(
                                SID_STopLevelBrowser, IID_PPV_ARGS(&pSB)))) {
                            IShellView* pSV = nullptr;
                            if (SUCCEEDED(
                                    pSB->QueryActiveShellView(&pSV))) {
                                IFolderView* pFV = nullptr;
                                if (SUCCEEDED(pSV->QueryInterface(
                                        IID_PPV_ARGS(&pFV)))) {
                                    IPersistFolder2* pPF = nullptr;
                                    if (SUCCEEDED(pFV->GetFolder(
                                            IID_PPV_ARGS(&pPF)))) {
                                        PIDLIST_ABSOLUTE pidl = nullptr;
                                        if (SUCCEEDED(
                                                pPF->GetCurFolder(&pidl))) {
                                            PWSTR name = nullptr;
                                            if (SUCCEEDED(
                                                    SHGetNameFromIDList(
                                                        pidl,
                                                        SIGDN_DESKTOPABSOLUTEPARSING,
                                                        &name))) {
                                                result = name;
                                                CoTaskMemFree(name);
                                            }
                                            CoTaskMemFree(pidl);
                                        }
                                        pPF->Release();
                                    }
                                    pFV->Release();
                                }
                                pSV->Release();
                            }
                            pSB->Release();
                        }
                        pSP->Release();
                    }
                }

                pBrowser->Release();
                pDisp->Release();
                break;
            }
            pBrowser->Release();
        }
        pDisp->Release();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Per-tab identity.
//
// Every tab of a File Explorer window is a separate IShellWindows entry, but
// IWebBrowser2::get_HWND() reports the frame (CabinetWClass) handle for all of
// them, so tabs cannot be told apart by that handle.  The tab's own window is
// only reachable through IShellBrowser::GetWindow(); it is unique per tab and
// stays valid across navigation, which makes it the reliable way to identify
// the tab a redirect has just created.
// ---------------------------------------------------------------------------
struct ShellEntry {
    HWND          tabWnd;   // per-tab window, may be nullptr
    IWebBrowser2* browser;  // holds a reference; release with ReleaseEntries()
};

static std::vector<ShellEntry> GetWindowTabs(IShellWindows* pSW, HWND frame) {
    std::vector<ShellEntry> tabs;

    long count = 0;
    if (FAILED(pSW->get_Count(&count)))
        return tabs;

    for (long i = 0; i < count; i++) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt   = VT_I4;
        idx.lVal = i;

        IDispatch* pDisp = nullptr;
        if (FAILED(pSW->Item(idx, &pDisp)) || !pDisp)
            continue;

        IWebBrowser2* pBrowser = nullptr;
        if (SUCCEEDED(pDisp->QueryInterface(IID_PPV_ARGS(&pBrowser)))) {
            SHANDLE_PTR hwndPtr = 0;
            pBrowser->get_HWND(&hwndPtr);

            if (reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndPtr)) ==
                frame) {
                HWND tabWnd = nullptr;
                IServiceProvider* pSP = nullptr;
                if (SUCCEEDED(pDisp->QueryInterface(IID_PPV_ARGS(&pSP)))) {
                    IShellBrowser* pSB = nullptr;
                    if (SUCCEEDED(pSP->QueryService(
                            SID_STopLevelBrowser, IID_PPV_ARGS(&pSB)))) {
                        pSB->GetWindow(&tabWnd);
                        pSB->Release();
                    }
                    pSP->Release();
                }

                ShellEntry entry = {tabWnd, pBrowser};  // keeps the ref
                tabs.push_back(entry);
                pDisp->Release();
                continue;
            }
            pBrowser->Release();
        }
        pDisp->Release();
    }
    return tabs;
}

static std::vector<ShellEntry> GetWindowTabs(HWND frame) {
    std::vector<ShellEntry> tabs;

    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
        return tabs;

    tabs = GetWindowTabs(pSW, frame);
    pSW->Release();
    return tabs;
}

static void ReleaseEntries(std::vector<ShellEntry>& entries) {
    for (auto& e : entries) {
        if (e.browser)
            e.browser->Release();
    }
    entries.clear();
}

// Snapshot tab HWNDs without COM.  Every tab is a direct ShellTabWindowClass
// child of the CabinetWClass frame, so handle-set membership (all FindNewTab
// needs) doesn't require an IShellWindows enumeration.
static std::vector<HWND> SnapshotTabWindows(HWND frame) {
    std::vector<HWND> tabs;
    if (!frame || !IsWindow(frame))
        return tabs;

    HWND child = GetWindow(frame, GW_CHILD);
    while (child) {
        WCHAR className[64];
        if (GetClassNameW(child, className, ARRAYSIZE(className)) &&
            wcscmp(className, WC_SHELLTAB) == 0) {
            tabs.push_back(child);
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return tabs;
}

// ---------------------------------------------------------------------------
// Wait for a tab of 'frame' that was not present in 'beforeTabs' to appear and
// return it (with a reference to its browser).  Polls until timeoutMs elapses.
// ---------------------------------------------------------------------------
static bool FindNewTab(HWND frame, const std::vector<HWND>& beforeTabs,
                       ShellEntry& result, int timeoutMs) {
    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
        return false;

    bool found = false;
    DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);

    for (;;) {
        std::vector<ShellEntry> entries = GetWindowTabs(pSW, frame);

        // Only act when exactly one tab is new.  If the set is ambiguous
        // (a tab opened concurrently, or an entry is momentarily missing
        // while the shell reorders its tab windows) guessing would risk
        // navigating the wrong tab, so keep polling instead.
        ShellEntry candidate = {nullptr, nullptr};
        int newCount = 0;

        for (auto& e : entries) {
            if (!e.tabWnd)
                continue;

            bool known = false;
            for (auto h : beforeTabs) {
                if (h == e.tabWnd) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                newCount++;
                if (newCount == 1) {
                    candidate = e;
                    e.browser = nullptr;  // transfer the reference
                }
            }
        }

        if (newCount > 1) {
            if (candidate.browser)
                candidate.browser->Release();
            candidate = {nullptr, nullptr};
        }
        ReleaseEntries(entries);

        if (candidate.browser) {
            result = candidate;
            found = true;
            break;
        }

        if (GetTickCount() >= deadline)
            break;
        Sleep(15);
    }

    pSW->Release();
    return found;
}

// ---------------------------------------------------------------------------
// COM: Navigate one specific tab to the target path.
// ---------------------------------------------------------------------------
static bool NavigateTab(const ShellEntry& entry, const std::wstring& path) {
    IWebBrowser2* pBrowser = entry.browser;
    if (!pBrowser)
        return false;

    VARIANT target;
    VariantInit(&target);
    VARIANT empty;
    VariantInit(&empty);
    HRESULT hr = E_FAIL;

    // CLSID paths (e.g. `::{...}`) need PIDL-based navigation
    // because Navigate2 with a BSTR doesn't handle them.
    if (path.size() >= 3 && path[0] == L':' && path[1] == L':' &&
        path[2] == L'{') {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0,
                                         nullptr)) && pidl) {
            UINT pidlSize = ILGetSize(pidl);
            SAFEARRAY* sa = SafeArrayCreateVector(VT_UI1, 0, pidlSize);
            if (sa) {
                void* data = nullptr;
                SafeArrayAccessData(sa, &data);
                memcpy(data, pidl, pidlSize);
                SafeArrayUnaccessData(sa);

                target.vt     = VT_ARRAY | VT_UI1;
                target.parray = sa;
                hr = pBrowser->Navigate2(&target, &empty, &empty, &empty,
                                          &empty);
                SafeArrayDestroy(sa);
            }
            CoTaskMemFree(pidl);
        }
    } else {
        // Regular filesystem path — BSTR works fine
        target.vt      = VT_BSTR;
        target.bstrVal = SysAllocString(path.c_str());
        hr = pBrowser->Navigate2(&target, &empty, &empty, &empty, &empty);
        SysFreeString(target.bstrVal);
    }

    Wh_Log(L"Navigate2 tab=0x%p hr=0x%08X", entry.tabWnd, hr);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// COM: Quick check whether an Explorer window has finished navigating.
// Returns true if READYSTATE_COMPLETE, false if still loading or on error.
// ---------------------------------------------------------------------------
static bool IsExplorerReady(HWND targetHwnd) {
    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
        return false;

    long count = 0;
    pSW->get_Count(&count);
    bool ready = false;

    for (long i = 0; i < count; i++) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt   = VT_I4;
        idx.lVal = i;

        IDispatch* pDisp = nullptr;
        if (FAILED(pSW->Item(idx, &pDisp)) || !pDisp)
            continue;

        IWebBrowser2* pBrowser = nullptr;
        if (SUCCEEDED(pDisp->QueryInterface(IID_PPV_ARGS(&pBrowser)))) {
            SHANDLE_PTR hwndPtr = 0;
            pBrowser->get_HWND(&hwndPtr);

            if (reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndPtr)) ==
                targetHwnd) {
                READYSTATE rs = READYSTATE_LOADING;
                pBrowser->get_ReadyState(&rs);
                ready = (rs == READYSTATE_COMPLETE);
                pBrowser->Release();
                pDisp->Release();
                break;
            }
            pBrowser->Release();
        }
        pDisp->Release();
    }
    pSW->Release();
    return ready;
}

// ---------------------------------------------------------------------------
// COM: Extract the list of currently selected items from an Explorer window.
// Returns absolute parsing paths (e.g. "C:\Users\...\file.txt") for each
// selected item.  Returns an empty vector if nothing is selected or on failure.
// ---------------------------------------------------------------------------
static std::vector<std::wstring> GetWindowSelectedItems(HWND targetHwnd) {
    std::vector<std::wstring> items;

    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
        return items;

    long count = 0;
    pSW->get_Count(&count);

    for (long i = 0; i < count; i++) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt   = VT_I4;
        idx.lVal = i;

        IDispatch* pDisp = nullptr;
        if (FAILED(pSW->Item(idx, &pDisp)) || !pDisp)
            continue;

        IWebBrowser2* pBrowser = nullptr;
        if (SUCCEEDED(pDisp->QueryInterface(IID_PPV_ARGS(&pBrowser)))) {
            SHANDLE_PTR hwndPtr = 0;
            pBrowser->get_HWND(&hwndPtr);

            if (reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndPtr)) ==
                targetHwnd) {
                // Navigate: IShellWindows → IShellBrowser → IShellView → IFolderView2
                IServiceProvider* pSP = nullptr;
                if (SUCCEEDED(
                        pDisp->QueryInterface(IID_PPV_ARGS(&pSP)))) {
                    IShellBrowser* pSB = nullptr;
                    if (SUCCEEDED(pSP->QueryService(
                            SID_STopLevelBrowser, IID_PPV_ARGS(&pSB)))) {
                        IShellView* pSV = nullptr;
                        if (SUCCEEDED(
                                pSB->QueryActiveShellView(&pSV))) {
                            IFolderView2* pFV2 = nullptr;
                            if (SUCCEEDED(pSV->QueryInterface(
                                    IID_PPV_ARGS(&pFV2)))) {
                                IShellItemArray* pSIA = nullptr;
                                // GetSelection(FALSE) = no UI interaction
                                if (SUCCEEDED(pFV2->GetSelection(
                                        FALSE, &pSIA)) &&
                                    pSIA) {
                                    DWORD itemCount = 0;
                                    if (SUCCEEDED(
                                            pSIA->GetCount(&itemCount))) {
                                        for (DWORD j = 0; j < itemCount;
                                             j++) {
                                            IShellItem* pSI = nullptr;
                                            if (SUCCEEDED(pSIA->GetItemAt(
                                                    j, &pSI)) &&
                                                pSI) {
                                                PWSTR displayName =
                                                    nullptr;
                                                if (SUCCEEDED(
                                                        pSI->GetDisplayName(
                                                            SIGDN_DESKTOPABSOLUTEPARSING,
                                                            &displayName)) &&
                                                    displayName) {
                                                    items.push_back(
                                                        displayName);
                                                    CoTaskMemFree(
                                                        displayName);
                                                }
                                                pSI->Release();
                                            }
                                        }
                                    }
                                    pSIA->Release();
                                }
                                pFV2->Release();
                            }
                            pSV->Release();
                        }
                        pSB->Release();
                    }
                    pSP->Release();
                }
                pBrowser->Release();
                pDisp->Release();
                break;
            }
            pBrowser->Release();
        }
        pDisp->Release();
    }
    pSW->Release();
    return items;
}

// ---------------------------------------------------------------------------
// COM: Select items inside the newest tab of a primary Explorer window.
// Converts item paths → child PIDLs via SHParseDisplayName + ILFindChild,
// then polls up to ~3s for the tab's folder view to become ready before
// calling IFolderView::SelectAndPositionItems with the PIDL array.
// Returns true if selection succeeded, false otherwise.
// ---------------------------------------------------------------------------
static bool SelectItemsInNewTab(HWND primaryHwnd,
                                const std::wstring& folderPath,
                                const std::vector<std::wstring>& itemPaths,
                                HWND tabWnd) {
    if (itemPaths.empty())
        return true;  // Nothing to select — success

    // Parse the folder path to an absolute PIDL once (used for ILFindChild)
    PIDLIST_ABSOLUTE pidlFolder = nullptr;
    if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &pidlFolder,
                                  0, nullptr)) ||
        !pidlFolder)
        return false;

    // Build an array of child PIDLs (relative to pidlFolder) for the items
    // we want to select.  pidlItems keeps the absolute PIDLs alive; childPIDLs
    // points into them.
    std::vector<PIDLIST_ABSOLUTE> pidlItems;
    std::vector<PCUITEMID_CHILD>  childPIDLs;

    for (size_t k = 0; k < itemPaths.size(); k++) {
        PIDLIST_ABSOLUTE pidlItem = nullptr;
        if (SUCCEEDED(SHParseDisplayName(itemPaths[k].c_str(), nullptr,
                                         &pidlItem, 0, nullptr)) &&
            pidlItem) {
            PCUITEMID_CHILD child = ILFindChild(pidlFolder, pidlItem);
            if (child) {
                pidlItems.push_back(pidlItem);   // keep alive
                childPIDLs.push_back(child);
            } else {
                CoTaskMemFree(pidlItem);
            }
        }
    }

    if (childPIDLs.empty()) {
        CoTaskMemFree(pidlFolder);
        Wh_Log(L"No resolved child PIDLs — cannot select items");
        return false;
    }

    // ---- Poll for the new tab to finish navigating, then select ----
    // Check immediately first (attempt 0, no sleep), then poll.
    bool success = false;

    for (int attempt = 0; attempt < 20; attempt++) {
        if (attempt > 0)
            Sleep(30);

        std::vector<ShellEntry> entries = GetWindowTabs(primaryHwnd);
        bool found = false;

        for (auto& e : entries) {
            if (!found && e.tabWnd == tabWnd && e.browser) {
                // Verify the browser is done loading
                READYSTATE rs = READYSTATE_LOADING;
                e.browser->get_ReadyState(&rs);
                if (rs != READYSTATE_COMPLETE) {
                    continue;  // Not ready yet, retry
                }

                // Get the shell view
                IServiceProvider* pSP = nullptr;
                if (SUCCEEDED(e.browser->QueryInterface(
                        IID_PPV_ARGS(&pSP)))) {
                    IShellBrowser* pSB = nullptr;
                    if (SUCCEEDED(pSP->QueryService(
                            SID_STopLevelBrowser, IID_PPV_ARGS(&pSB)))) {
                        IShellView* pSV = nullptr;
                        if (SUCCEEDED(pSB->QueryActiveShellView(&pSV))) {
                            IFolderView* pFV = nullptr;
                            if (SUCCEEDED(pSV->QueryInterface(
                                    IID_PPV_ARGS(&pFV)))) {
                                // Select all items at once
                                HRESULT hr = pFV->SelectAndPositionItems(
                                    static_cast<UINT>(childPIDLs.size()),
                                    childPIDLs.data(),
                                    nullptr,  // POINT* apt
                                    SVSI_SELECT | SVSI_DESELECTOTHERS |
                                        SVSI_FOCUSED | SVSI_ENSUREVISIBLE);
                                if (SUCCEEDED(hr)) {
                                    Wh_Log(L"Selected %zu items "
                                           L"in new tab",
                                           childPIDLs.size());
                                    success = true;
                                } else {
                                    Wh_Log(L"SelectAndPositionItems "
                                           L"failed: 0x%08X",
                                           hr);
                                }

                                pFV->Release();
                                found = true;  // Done with this tab
                            }
                            pSV->Release();
                        }
                        pSB->Release();
                    }
                    pSP->Release();
                }
            }
        }

        ReleaseEntries(entries);

        if (found)
            break;
    }

    for (auto pidl : pidlItems)
        CoTaskMemFree(pidl);
    CoTaskMemFree(pidlFolder);
    return success;
}

// ---------------------------------------------------------------------------
// Redirect thread: read path/selection → close duplicate → create/navigate tab
// ---------------------------------------------------------------------------
static DWORD WINAPI RedirectThread(LPVOID param) {
    auto* info    = static_cast<RedirectInfo*>(param);
    HWND  newWnd  = info->newWindow;
    HWND  primary = nullptr;
    delete info;

    PhantomGuard phantomGuard;
    phantomGuard.hwnd = newWnd;

    ULONGLONG threadStart = GetTickCount64();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    // Phase 1: extract path (max ~100ms).  Reuse one IShellWindows instance
    // for the whole poll loop; the old code re-created it on every retry.
    // Check immediately first — by the time the thread starts the window has
    // usually navigated.
    std::wstring path;
    IShellWindows* pathShellWindows = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&pathShellWindows))) &&
        pathShellWindows) {
        path = GetExplorerWindowPath(pathShellWindows, newWnd);
        if (path.empty()) {
            for (int attempt = 0;
                 attempt < 20 && IsWindow(newWnd);
                 attempt++) {
                Sleep(5);
                path = GetExplorerWindowPath(pathShellWindows, newWnd);
                if (!path.empty())
                    break;
            }
        }
        pathShellWindows->Release();
    }
    ULONGLONG pathDone = GetTickCount64();

    // The hook no longer blocks the Explorer UI thread on IShellWindows;
    // resolve the destination window here instead.
    if (!IsWindow(primary))
        primary = FindExistingExplorer(newWnd);
    ULONGLONG primaryDone = GetTickCount64();

    if (path.empty() || !primary || !IsWindow(primary) ||
        newWnd == primary) {
        Wh_Log(L"[v1-timing] abort path=%llu primary=%llu",
               static_cast<unsigned long long>(pathDone - threadStart),
               static_cast<unsigned long long>(primaryDone - pathDone));
        AbortRedirect(newWnd);
        CoUninitialize();
        return 0;
    }

    // Don't redirect Control Panel or other special shell windows
    if (!IsRedirectablePath(path)) {
        Wh_Log(L"Non-redirectable path, allowing window: %s", path.c_str());
        AbortRedirect(newWnd);
        CoUninitialize();
        return 0;
    }

    // Phase 2: read the SHOpenFolderAndSelectItems selection while the
    // duplicate window is still alive.  It must be read before closing the
    // duplicate; applying it to the new tab happens after navigation.
    ULONGLONG selectionStart = GetTickCount64();
    std::vector<std::wstring> selectedItems = GetWindowSelectedItems(newWnd);
    if (selectedItems.empty() && !IsExplorerReady(newWnd)) {
        for (int attempt = 0; attempt < 15 && IsWindow(newWnd); attempt++) {
            Sleep(10);
            selectedItems = GetWindowSelectedItems(newWnd);
            if (!selectedItems.empty()) break;
            if (IsExplorerReady(newWnd)) break;
        }
    }
    ULONGLONG selectionDone = GetTickCount64();

    // Find a tab window to send the command to before closing the duplicate,
    // so a missing ShellTabWindowClass can still restore the original window.
    HWND shellTab = FindWindowExW(primary, nullptr, WC_SHELLTAB, nullptr);
    if (!shellTab) {
        Wh_Log(L"ShellTabWindowClass not found on primary %p", primary);
        Wh_Log(L"[v1-timing] abort-no-shelltab path=%llu primary=%llu "
               L"selection=%llu",
               static_cast<unsigned long long>(pathDone - threadStart),
               static_cast<unsigned long long>(primaryDone - pathDone),
               static_cast<unsigned long long>(selectionDone -
                                               selectionStart));
        AbortRedirect(newWnd);
        CoUninitialize();
        return 0;
    }

    // Close the duplicate before posting 0xA21B.  WM_CLOSE is queued first,
    // so the Explorer UI thread can stop working on the duplicate's pending
    // navigation instead of finishing it before handling the tab command.
    // This is the main reason the tab used to appear late.  Trade-off: if tab
    // creation/navigation fails there is no duplicate left to restore.
    if (g_lastPrimaryWindow == newWnd)
        g_lastPrimaryWindow = nullptr;
    PostMessageW(newWnd, WM_CLOSE, 0, 0);

    // Snapshot the window's tab HWNDs with a cheap Win32 child enumeration;
    // the new tab is the one handle that isn't in this set.
    std::vector<HWND> tabsBefore = SnapshotTabWindows(primary);
    ULONGLONG snapshotDone = GetTickCount64();

    // Create a new tab via internal WM_COMMAND.
    PostMessageW(shellTab, WM_COMMAND, CMD_NEW_TAB, 0);
    ULONGLONG postDone = GetTickCount64();

    // Only operate on the tab that was just created.  If its handle cannot be
    // identified, never fall back to navigating one of the user's existing
    // tabs; leave the blank tab and log the failure (the duplicate has
    // already been closed at this point).
    bool navigated = false;
    ShellEntry newTab = {nullptr, nullptr};

    if (FindNewTab(primary, tabsBefore, newTab, 800)) {
        Wh_Log(L"New tab identified: 0x%p", newTab.tabWnd);
    } else {
        Wh_Log(L"New tab not identified; refusing to touch existing tabs");
    }
    ULONGLONG foundDone = GetTickCount64();

    if (newTab.browser)
        navigated = NavigateTab(newTab, path);
    ULONGLONG navDone = GetTickCount64();

    if (!navigated) {
        Wh_Log(L"[v1-timing] abort path=%llu primary=%llu selection=%llu "
               L"snapshot=%llu post=%llu find=%llu navigate=%llu",
               static_cast<unsigned long long>(pathDone - threadStart),
               static_cast<unsigned long long>(primaryDone - pathDone),
               static_cast<unsigned long long>(selectionDone -
                                               selectionStart),
               static_cast<unsigned long long>(snapshotDone - selectionDone),
               static_cast<unsigned long long>(postDone - snapshotDone),
               static_cast<unsigned long long>(foundDone - postDone),
               static_cast<unsigned long long>(navDone - foundDone));
        if (newTab.browser)
            newTab.browser->Release();
        CoUninitialize();
        return 0;
    }

    // Bring the target window forward as soon as the tab is navigated; the
    // selection can finish afterwards without delaying the visible result.
    if (IsIconic(primary))
        ShowWindow(primary, SW_RESTORE);

    DWORD pid = 0;
    GetWindowThreadProcessId(primary, &pid);
    AllowSetForegroundWindow(pid);
    SetForegroundWindow(primary);

    ULONGLONG selectStart = GetTickCount64();
    if (newTab.tabWnd && !selectedItems.empty()) {
        Wh_Log(L"Re-applying selection of %zu items", selectedItems.size());
        SelectItemsInNewTab(primary, path, selectedItems, newTab.tabWnd);
    }
    ULONGLONG selectDone = GetTickCount64();

    if (newTab.browser)
        newTab.browser->Release();

    ULONGLONG totalDone = GetTickCount64();

    Wh_Log(L"[v1-timing] path=%llu primary=%llu selection=%llu snapshot=%llu "
           L"post=%llu find=%llu navigate=%llu select=%llu total=%llu",
           static_cast<unsigned long long>(pathDone - threadStart),
           static_cast<unsigned long long>(primaryDone - pathDone),
           static_cast<unsigned long long>(selectionDone - selectionStart),
           static_cast<unsigned long long>(snapshotDone - selectionDone),
           static_cast<unsigned long long>(postDone - snapshotDone),
           static_cast<unsigned long long>(foundDone - postDone),
           static_cast<unsigned long long>(navDone - foundDone),
           static_cast<unsigned long long>(selectDone - selectStart),
           static_cast<unsigned long long>(totalDone - threadStart));
    Wh_Log(L"%s → %s", navigated ? L"Redirected" : L"Failed", path.c_str());

    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Cheap Win32-only check used by CreateWindowExW_Hook.  It replaces the
// synchronous IShellWindows enumeration that used to run on the Explorer UI
// thread before the new window was even created.  The real destination window
// is now found in RedirectThread, off the UI thread.
// ---------------------------------------------------------------------------
static bool IsCabinetWindowClass(HWND hwnd) {
    WCHAR className[64];
    return hwnd && GetClassNameW(hwnd, className, ARRAYSIZE(className)) &&
           wcscmp(className, WC_CABINET) == 0;
}

static BOOL CALLBACK FindVisibleCabinetProc(HWND hwnd, LPARAM lParam) {
    auto* found = reinterpret_cast<bool*>(lParam);
    if (hwnd && IsWindowVisible(hwnd) && !IsPhantomWindow(hwnd) &&
        IsCabinetWindowClass(hwnd)) {
        *found = true;
        return FALSE;
    }
    return TRUE;
}

static bool HasVisibleCabinetWindow() {
    bool found = false;
    EnumWindows(FindVisibleCabinetProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

// ---------------------------------------------------------------------------
// Hook: CreateWindowExW
// ---------------------------------------------------------------------------
using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName,
                                  LPCWSTR lpWindowName, DWORD dwStyle,
                                  int X, int Y, int nWidth, int nHeight,
                                  HWND hWndParent, HMENU hMenu,
                                  HINSTANCE hInstance, LPVOID lpParam) {
    auto original = [=]() {
        return CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                        dwStyle, X, Y, nWidth, nHeight,
                                        hWndParent, hMenu, hInstance, lpParam);
    };

    bool isCabinet = lpClassName && !IS_INTRESOURCE(lpClassName) &&
                     wcscmp(lpClassName, WC_CABINET) == 0;
    if (!isCabinet) {
        return original();
    }

    // Bypass if Shift is held
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        return original();
    }

    ULONGLONG hookStart = GetTickCount64();

    // First window / no other Explorer window: don't hide it at all.  This is
    // a cheap Win32 check; the actual destination window is resolved later in
    // RedirectThread so the UI thread isn't blocked by an IShellWindows scan.
    if (!HasVisibleCabinetWindow()) {
        return original();
    }
    ULONGLONG precheckDone = GetTickCount64();

    Wh_Log(L"Creating Explorer window");
    g_creatingExplorerWindow = true;

    HWND hwnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                          dwStyle, X, Y, nWidth, nHeight,
                                          hWndParent, hMenu, hInstance,
                                          lpParam);

    ULONGLONG createDone = GetTickCount64();
    Wh_Log(L"[v1-timing] hook precheck=%llu create=%llu",
           static_cast<unsigned long long>(precheckDone - hookStart),
           static_cast<unsigned long long>(createDone - precheckDone));
    Wh_Log(L"Created window %p", hwnd);
    g_creatingExplorerWindow = false;

    if (hwnd) {
        // Make window fully transparent instead of stripping WS_VISIBLE.
        // This keeps WS_VISIBLE set so the window registers in IShellWindows
        // (required for path extraction of special folders like Recycle Bin)
        // but the user never sees it.
        SetWindowLongW(hwnd, GWL_EXSTYLE,
                       GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);

        g_lastPrimaryWindow = hwnd;
        AddPhantomWindow(hwnd);

        // Redirect — window is transparent, the thread finds the destination
        // window and closes/restores this duplicate.
        auto* ri = new (std::nothrow) RedirectInfo{hwnd};
        if (!ri) {
            AbortRedirect(hwnd);
            return hwnd;
        }

        HANDLE hThread =
            CreateThread(nullptr, 0, RedirectThread, ri, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            Wh_Log(L"CreateThread failed for %p", hwnd);
            delete ri;
            AbortRedirect(hwnd);
        }
    }

    return hwnd;
}

// ---------------------------------------------------------------------------
// Hook: ShowWindow — prevent the redirected window from flashing
// ---------------------------------------------------------------------------
BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow) {
    if (g_creatingExplorerWindow || hWnd == g_lastPrimaryWindow) {
        WCHAR className[64];
        if (GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
            wcscmp(className, WC_CABINET) == 0) {
            Wh_Log(
                L"Blocked ShowWindow for %p %s", hWnd,
                g_creatingExplorerWindow ? L"(creating)" : L"(last created)");
            return TRUE;
        }
    }
    return ShowWindow_Original(hWnd, nCmdShow);
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------
BOOL Wh_ModInit() {
    InitializeCriticalSection(&g_phantomLock);

    Wh_SetFunctionHook(reinterpret_cast<void*>(CreateWindowExW),
                        reinterpret_cast<void*>(CreateWindowExW_Hook),
                        reinterpret_cast<void**>(&CreateWindowExW_Original));
    Wh_SetFunctionHook(reinterpret_cast<void*>(ShowWindow),
                        reinterpret_cast<void*>(ShowWindow_Hook),
                        reinterpret_cast<void**>(&ShowWindow_Original));
    return TRUE;
}

// Not deleting g_phantomLock here: a redirect thread may still be running.
void Wh_ModUninit() {}
