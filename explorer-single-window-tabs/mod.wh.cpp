// ==WindhawkMod==
// @id              nbc-explorer-single-window-tabs
// @name            Explorer 单窗口标签页
// @description     把新的explorer窗口重定向至新窗口
// @version         0.8
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
    HWND primaryWindow;
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

            if (hwnd && hwnd != exclude && IsWindowVisible(hwnd)) {
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
static std::wstring GetExplorerWindowPath(HWND targetHwnd) {
    std::wstring result;

    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
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
    pSW->Release();
    return result;
}

// ---------------------------------------------------------------------------
// COM: Count current IShellWindows entries
// ---------------------------------------------------------------------------
static int GetShellWindowCount() {
    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
        return 0;
    long count = 0;
    pSW->get_Count(&count);
    pSW->Release();
    return static_cast<int>(count);
}

// ---------------------------------------------------------------------------
// COM: Navigate the most recently added tab to the target path
// ---------------------------------------------------------------------------
static bool NavigateNewTab(HWND primaryHwnd, const std::wstring& path,
                           int prevCount) {
    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW))))
        return false;

    long count = 0;
    pSW->get_Count(&count);

    bool success = false;
    for (long i = count - 1; i >= 0; i--) {
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
                primaryHwnd) {
                // Only navigate empty/home tabs to avoid clobbering
                BSTR url = nullptr;
                pBrowser->get_LocationURL(&url);
                bool isEmptyOrNew =
                    (!url || wcslen(url) == 0 || i >= prevCount);
                if (url) SysFreeString(url);

                if (isEmptyOrNew) {
                    VARIANT target;
                    VariantInit(&target);
                    VARIANT empty;
                    VariantInit(&empty);
                    HRESULT hr = E_FAIL;

                    // CLSID paths (e.g. `::{...}`) need PIDL-based navigation
                    // because Navigate2 with a BSTR doesn't handle them.
                    if (path.size() >= 3 && path[0] == L':' &&
                        path[1] == L':' && path[2] == L'{') {
                        PIDLIST_ABSOLUTE pidl = nullptr;
                        if (SUCCEEDED(SHParseDisplayName(path.c_str(),
                                nullptr, &pidl, 0, nullptr)) && pidl) {
                            UINT pidlSize = ILGetSize(pidl);
                            SAFEARRAY* sa = SafeArrayCreateVector(
                                VT_UI1, 0, pidlSize);
                            if (sa) {
                                void* data = nullptr;
                                SafeArrayAccessData(sa, &data);
                                memcpy(data, pidl, pidlSize);
                                SafeArrayUnaccessData(sa);

                                target.vt     = VT_ARRAY | VT_UI1;
                                target.parray = sa;
                                hr = pBrowser->Navigate2(&target, &empty,
                                                          &empty, &empty,
                                                          &empty);
                                SafeArrayDestroy(sa);
                            }
                            CoTaskMemFree(pidl);
                        }
                    } else {
                        // Regular filesystem path — BSTR works fine
                        target.vt      = VT_BSTR;
                        target.bstrVal = SysAllocString(path.c_str());
                        hr = pBrowser->Navigate2(&target, &empty,
                                                  &empty, &empty, &empty);
                        SysFreeString(target.bstrVal);
                    }

                    Wh_Log(L"Navigate2 [%d] hr=0x%08X", static_cast<int>(i),
                           hr);
                    success = SUCCEEDED(hr);

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
    return success;
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
                                int prevCount) {
    if (itemPaths.empty())
        return true;  // Nothing to select — success

    // Parse the folder path to an absolute PIDL once (used for ILFindChild)
    PIDLIST_ABSOLUTE pidlFolder = nullptr;
    if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &pidlFolder,
                                  0, nullptr)) ||
        !pidlFolder)
        return false;

    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pSW)))) {
        CoTaskMemFree(pidlFolder);
        return false;
    }

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
        pSW->Release();
        Wh_Log(L"No resolved child PIDLs — cannot select items");
        return false;
    }

    // ---- Poll for the new tab to finish navigating, then select ----
    // Check immediately first (attempt 0, no sleep), then poll.
    bool success = false;

    for (int attempt = 0; attempt < 20; attempt++) {
        if (attempt > 0)
            Sleep(30);

        long count = 0;
        if (FAILED(pSW->get_Count(&count)))
            continue;

        bool found = false;

        // Search back-to-front — newest tabs are at higher indices
        for (long i = count - 1; i >= 0 && !found; i--) {
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
                    primaryHwnd) {
                    // Check that this is a new tab (index >= prevCount)
                    if (i < prevCount) {
                        pBrowser->Release();
                        pDisp->Release();
                        continue;
                    }

                    // Verify the browser is done loading
                    READYSTATE rs = READYSTATE_LOADING;
                    pBrowser->get_ReadyState(&rs);
                    if (rs != READYSTATE_COMPLETE) {
                        pBrowser->Release();
                        pDisp->Release();
                        continue;  // Not ready yet, retry
                    }

                    // Get the shell view
                    IServiceProvider* pSP = nullptr;
                    if (SUCCEEDED(pDisp->QueryInterface(
                            IID_PPV_ARGS(&pSP)))) {
                        IShellBrowser* pSB = nullptr;
                        if (SUCCEEDED(pSP->QueryService(
                                SID_STopLevelBrowser,
                                IID_PPV_ARGS(&pSB)))) {
                            IShellView* pSV = nullptr;
                            if (SUCCEEDED(pSB->QueryActiveShellView(
                                    &pSV))) {
                                IFolderView* pFV = nullptr;
                                if (SUCCEEDED(pSV->QueryInterface(
                                        IID_PPV_ARGS(&pFV)))) {
                                    // Select all items at once
                                    HRESULT hr =
                                        pFV->SelectAndPositionItems(
                                            static_cast<UINT>(
                                                childPIDLs.size()),
                                            childPIDLs.data(),
                                            nullptr,  // POINT* apt
                                            SVSI_SELECT |
                                                SVSI_DESELECTOTHERS |
                                                SVSI_FOCUSED |
                                                SVSI_ENSUREVISIBLE);
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
                pBrowser->Release();
            }
            pDisp->Release();
        }

        if (found)
            break;
    }

    for (auto pidl : pidlItems)
        CoTaskMemFree(pidl);
    CoTaskMemFree(pidlFolder);
    pSW->Release();
    return success;
}

// ---------------------------------------------------------------------------
// Redirect thread: close new window → create tab → navigate
// ---------------------------------------------------------------------------
static DWORD WINAPI RedirectThread(LPVOID param) {
    auto* info    = static_cast<RedirectInfo*>(param);
    HWND  newWnd  = info->newWindow;
    HWND  primary = info->primaryWindow;
    delete info;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    // Phase 1: extract path (max ~100ms).  Check immediately first —
    // by the time the thread starts the window has usually navigated.
    std::wstring path = GetExplorerWindowPath(newWnd);
    if (path.empty()) {
        for (int attempt = 0; attempt < 20 && IsWindow(newWnd); attempt++) {
            Sleep(5);
            path = GetExplorerWindowPath(newWnd);
            if (!path.empty()) break;
        }
    }

    // Phase 2: SHOpenFolderAndSelectItems selection detection.
    // Check immediately; only poll if the window is still loading.
    std::vector<std::wstring> selectedItems;
    if (!path.empty()) {
        selectedItems = GetWindowSelectedItems(newWnd);
        if (selectedItems.empty() && !IsExplorerReady(newWnd)) {
            for (int attempt = 0; attempt < 15 && IsWindow(newWnd);
                 attempt++) {
                Sleep(10);
                selectedItems = GetWindowSelectedItems(newWnd);
                if (!selectedItems.empty()) break;
                if (IsExplorerReady(newWnd)) break;
            }
        }
    }

    // Re-verify primary is still alive
    if (!IsWindow(primary))
        primary = FindExistingExplorer(newWnd);

    if (path.empty() || !primary || !IsWindow(primary) ||
        newWnd == primary) {
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

    // Snapshot tab count before creating a new tab
    int prevCount = GetShellWindowCount() - 1;

    // Close the duplicate window (already transparent via WS_EX_LAYERED)
    PostMessageW(newWnd, WM_CLOSE, 0, 0);

    // Create a new tab via internal WM_COMMAND
    HWND shellTab = FindWindowExW(primary, nullptr, WC_SHELLTAB, nullptr);
    if (!shellTab) {
        Wh_Log(L"ShellTabWindowClass not found on primary %p", primary);
        CoUninitialize();
        return 0;
    }
    PostMessageW(shellTab, WM_COMMAND, CMD_NEW_TAB, 0);

    // Phase 3: navigate the new tab (max ~100ms).
    // Try immediately — PostMessage may already be processed.
    bool navigated = false;
    if (GetShellWindowCount() > prevCount)
        navigated = NavigateNewTab(primary, path, prevCount);

    if (!navigated) {
        for (int wait = 0; wait < 15; wait++) {
            Sleep(10);
            if (GetShellWindowCount() > prevCount) {
                navigated = NavigateNewTab(primary, path, prevCount);
                break;
            }
        }
    }

    // Fallback: try navigating the last matching entry
    if (!navigated)
        navigated = NavigateNewTab(primary, path, 0);

    // SHOpenFolderAndSelectItems: re-select the items in the new tab.
    if (navigated && !selectedItems.empty()) {
        Wh_Log(L"Re-applying selection of %zu items", selectedItems.size());
        SelectItemsInNewTab(primary, path, selectedItems, prevCount);
    }

    // Bring primary window to foreground
    if (IsIconic(primary))
        ShowWindow(primary, SW_RESTORE);

    DWORD pid = 0;
    GetWindowThreadProcessId(primary, &pid);
    AllowSetForegroundWindow(pid);
    SetForegroundWindow(primary);

    Wh_Log(L"%s → %s", navigated ? L"Redirected" : L"Failed", path.c_str());

    CoUninitialize();
    return 0;
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

    HWND existing = FindExistingExplorer(nullptr);

    // First window
    if (!existing) {
        return original();
    }

    Wh_Log(L"Creating Explorer window");
    g_creatingExplorerWindow = true;

    HWND hwnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                          dwStyle, X, Y, nWidth, nHeight,
                                          hWndParent, hMenu, hInstance,
                                          lpParam);

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

        // Redirect — window is transparent, thread will close it
        auto* ri = new RedirectInfo{hwnd, existing};
        HANDLE hThread =
            CreateThread(nullptr, 0, RedirectThread, ri, 0, nullptr);
        if (hThread) CloseHandle(hThread);
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
    Wh_SetFunctionHook(reinterpret_cast<void*>(CreateWindowExW),
                        reinterpret_cast<void*>(CreateWindowExW_Hook),
                        reinterpret_cast<void**>(&CreateWindowExW_Original));
    Wh_SetFunctionHook(reinterpret_cast<void*>(ShowWindow),
                        reinterpret_cast<void*>(ShowWindow_Hook),
                        reinterpret_cast<void**>(&ShowWindow_Original));
    return TRUE;
}

void Wh_ModUninit() {}
