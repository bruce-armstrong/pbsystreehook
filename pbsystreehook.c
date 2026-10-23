/* pbsystreehook.c - PowerBuilder 2025 R2 IDE System Tree context-menu extension (prototype)
 *
 * Copyright (c) 2026 Bruce Armstrong
 * Licensed under the MIT License. See the LICENSE file in this directory for the
 * full text (https://opensource.org/licenses/MIT).
 *
 * A thread-level hook maps this DLL into the PB250 IDE process. When the System
 * Tree's right-click menu is about to appear (WM_INITMENUPOPUP) and the click
 * landed on a tree node, we read the node under the cursor from PBTreeView32_100,
 * remember the object + library, and append our own items - whose labels and
 * actions are read from pbsystreehook.ini next to this DLL. When one is chosen
 * (WM_COMMAND) we run its configured action with the object + library filled in.
 *
 * Config (pbsystreehook.ini, re-read on every popup so edits apply with no rebuild):
 *   [pbsystreehook]
 *   Items=2
 *   [Item1]
 *   Label=Run my tool on "%o"
 *   Command=C:\Tools\mytool.exe        ; an exe/bat/... or a built-in (:clipboard, :msgbox)
 *   Args="%o" "%L"
 *   Dir=C:\Tools                       ; optional working directory
 * Placeholders in Label/Args: %o object name, %l library as shown,
 *   %L full .pbl path, %n newline, %% literal percent.
 *
 * Owner-draw / theming / sizing. Our items are MF_OWNERDRAW so we can paint them
 * in the IDE's theme. To control their SIZE as well as their look we must own
 * WM_MEASUREITEM / WM_DRAWITEM and return TRUE, else the IDE's menu owner forces a
 * default size. A plain message hook can't out-vote the window proc, so we subclass
 * the menu-owner window. The IDE swaps that proc back between context menus, so the
 * forward hook RE-INSTALLS our proc on every WM_INITMENUPOPUP (SetWindowLongPtr;
 * comctl32's SetWindowSubclass silently no-ops once its bookkeeping is stale).
 * Theme colors are sampled from the live menu; row/separator heights are matched to
 * the IDE's own items.
 *
 * This is the user's own IDE on their own machine; everything here uses documented
 * Win32 APIs.
 *
 * Build (MinGW-w64, 32-bit, from the i686 shell):
 *   i686-w64-mingw32-gcc -O2 -shared -o pbsystreehook.dll pbsystreehook.c -lcomctl32 -luser32 -lgdi32 -Wl,--kill-at
 */

#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <commctrl.h>      /* tree-view messages / TVHITTESTINFO / HTREEITEM */
#include <wchar.h>
#include <stdarg.h>

#define MAX_ITEMS      16
#define MENU_ID_BASE   0x9100      /* must not collide with PB's own command ids */
#define ID_SEP         (MENU_ID_BASE + 0)
#define ID_FIRST_ITEM  (MENU_ID_BASE + 1)         /* items: ID_FIRST_ITEM .. +MAX_ITEMS */
#define LEFT_GUTTER    38          /* line our text up with the IDE's text column */
#define RIGHT_PAD      28

/* ---- diagnostic log, switched on at runtime from pbsystreehook.ini ------------- */
static BOOL      g_logOn = FALSE;              /* [pbsystreehook] Log=1 turns it on  */
static wchar_t   g_logPath[MAX_PATH] = L"";    /* [pbsystreehook] LogFile=, or default */

static void Log(const char *fmt, ...)
{
    if (!g_logOn || !g_logPath[0]) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); wvsprintfA(buf, fmt, ap); va_end(ap);
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    wsprintfA(ts, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    DWORD n;
    WriteFile(f, ts, lstrlenA(ts), &n, NULL);
    WriteFile(f, buf, lstrlenA(buf), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

static HINSTANCE g_hinst    = NULL;
static HHOOK     g_hCwp     = NULL;   /* forward hook: re-subclass + append on popup */
static HHOOK     g_hGetMsg  = NULL;   /* WM_COMMAND                                  */

/* One menu item. For our separator only isSep/text matter. */
typedef struct {
    int     isSep;
    wchar_t text[400];                /* expanded display label (measure/draw use this) */
    wchar_t label[300];               /* raw Label= from the ini                        */
    wchar_t command[MAX_PATH];        /* program path, or :clipboard / :msgbox          */
    wchar_t args[600];                /* raw Args= (placeholders expanded at launch)    */
    wchar_t dir[MAX_PATH];            /* optional working directory                     */
} CFGITEM;

static CFGITEM   g_sep = { 1, L"", L"", L"", L"", L"" };
static CFGITEM   g_items[MAX_ITEMS];
static int       g_nitems = 0;

/* Captured at WM_INITMENUPOPUP, consumed at WM_COMMAND. */
static wchar_t   g_obj[512];
static wchar_t   g_lib[512];
static BOOL      g_haveNode = FALSE;
static HFONT     g_menuFont = NULL;
static int       g_pbItemH  = 0;      /* height the IDE gives its own normal items */
static int       g_pbSepH   = 0;      /* height the IDE gives its own separators   */
static HWND      g_owner    = NULL;   /* menu-owner window we've subclassed         */
static WNDPROC   g_prevProc = NULL;   /* the IDE's proc we chain to                 */

static BOOL IsOurId(UINT id) { return id >= ID_SEP && id <= (UINT)(ID_FIRST_ITEM + MAX_ITEMS); }

/* ---- config ------------------------------------------------------------- */

static void ConfigPath(wchar_t *out, int cch)
{
    GetModuleFileNameW(g_hinst, out, cch);
    wchar_t *s = wcsrchr(out, L'\\');
    if (s) wcscpy(s + 1, L"pbsystreehook.ini"); else wcscpy(out, L"pbsystreehook.ini");
}

/* g_lib looks like  name.pbl (C:\folder\path)  -> build  C:\folder\path\name.pbl  */
static void LibPath(wchar_t *out, int cch)
{
    out[0] = 0;
    const wchar_t *op = wcsstr(g_lib, L" (");
    if (!op) { lstrcpynW(out, g_lib, cch); return; }
    wchar_t name[260];
    int n = (int)(op - g_lib);
    if (n > 259) n = 259;
    wcsncpy(name, g_lib, n); name[n] = 0;
    wchar_t folder[MAX_PATH];
    lstrcpynW(folder, op + 2, MAX_PATH);
    wchar_t *cp = wcsrchr(folder, L')');
    if (cp) *cp = 0;
    _snwprintf(out, cch, L"%s\\%s", folder, name);
    out[cch - 1] = 0;
}

/* Copy src into out, replacing %o %l %L %n %%. */
static void Expand(wchar_t *out, int cch, const wchar_t *src)
{
    wchar_t lib[MAX_PATH];
    LibPath(lib, MAX_PATH);
    int o = 0;
    for (int i = 0; src[i] && o < cch - 1; i++) {
        if (src[i] != L'%') { out[o++] = src[i]; continue; }
        wchar_t c = src[i + 1];
        const wchar_t *rep = NULL;
        if      (c == L'o') rep = g_obj;
        else if (c == L'l') rep = g_lib;
        else if (c == L'L') rep = lib;
        else if (c == L'n') { out[o++] = L'\r'; if (o < cch - 1) out[o++] = L'\n'; i++; continue; }
        else if (c == L'%') { out[o++] = L'%'; i++; continue; }
        else { out[o++] = src[i]; continue; }
        i++;
        for (int k = 0; rep[k] && o < cch - 1; k++) out[o++] = rep[k];
    }
    out[o] = 0;
}

static void LoadConfig(void)
{
    g_nitems = 0;
    wchar_t ini[MAX_PATH];
    ConfigPath(ini, MAX_PATH);

    /* Logging: [pbsystreehook] Log=1, optional LogFile= (defaults to pbsystreehook.log by the DLL). */
    g_logOn = GetPrivateProfileIntW(L"pbsystreehook", L"Log", 0, ini) != 0;
    GetPrivateProfileStringW(L"pbsystreehook", L"LogFile", L"", g_logPath, MAX_PATH, ini);
    if (g_logOn && !g_logPath[0]) {
        GetModuleFileNameW(g_hinst, g_logPath, MAX_PATH);
        wchar_t *s = wcsrchr(g_logPath, L'\\');
        if (s) wcscpy(s + 1, L"pbsystreehook.log"); else wcscpy(g_logPath, L"pbsystreehook.log");
    }

    int count = GetPrivateProfileIntW(L"pbsystreehook", L"Items", 0, ini);
    if (count > MAX_ITEMS) count = MAX_ITEMS;
    for (int i = 1; i <= count; i++) {
        wchar_t sec[32];
        wsprintfW(sec, L"Item%d", i);
        CFGITEM *it = &g_items[g_nitems];
        ZeroMemory(it, sizeof *it);
        GetPrivateProfileStringW(sec, L"Label", L"", it->label, 300, ini);
        if (!it->label[0]) continue;              /* skip blank / missing items */
        GetPrivateProfileStringW(sec, L"Command", L"", it->command, MAX_PATH, ini);
        GetPrivateProfileStringW(sec, L"Args",    L"", it->args,    600,      ini);
        GetPrivateProfileStringW(sec, L"Dir",     L"", it->dir,     MAX_PATH, ini);
        g_nitems++;
    }
    Log("LoadConfig: %d item(s) from \"%S\"", g_nitems, ini);
}

static void CopyToClipboard(const wchar_t *s)
{
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void *p = GlobalLock(g);
        if (p) { memcpy(p, s, bytes); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
        else GlobalFree(g);
    }
    CloseClipboard();
}

static void RunItem(const CFGITEM *it)
{
    wchar_t args[1200];
    Expand(args, 1200, it->args);

    Log("RunItem: command=\"%S\" args=\"%S\"", it->command, args);

    if (it->command[0] == L':') {                 /* built-in actions */
        if (!lstrcmpiW(it->command, L":clipboard"))
            CopyToClipboard(args);
        else if (!lstrcmpiW(it->command, L":msgbox"))
            MessageBoxW(NULL, args, L"pbsystreehook", MB_OK | MB_TOPMOST);
        else
            Log("RunItem: unknown built-in \"%S\"", it->command);
        return;
    }
    if (it->command[0]) {
        HINSTANCE r = ShellExecuteW(NULL, L"open", it->command, args[0] ? args : NULL,
                                    it->dir[0] ? it->dir : NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) Log("RunItem: ShellExecute failed (code %d)", (int)(INT_PTR)r);
    }
}

/* ---- helpers (run inside the PB process) -------------------------------- */

static HFONT MenuFont(void)
{
    if (!g_menuFont) {
        NONCLIENTMETRICSW nm;
        nm.cbSize = sizeof nm;
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0))
            g_menuFont = CreateFontIndirectW(&nm.lfMenuFont);
    }
    return g_menuFont;
}

static COLORREF Blend(COLORREF a, COLORREF b, int pct)
{
    int r = GetRValue(a) + (GetRValue(b) - GetRValue(a)) * pct / 100;
    int g = GetGValue(a) + (GetGValue(b) - GetGValue(a)) * pct / 100;
    int l = GetBValue(a) + (GetBValue(b) - GetBValue(a)) * pct / 100;
    return RGB(r, g, l);
}

static void ThemeColors(HDC dc, const RECT *rc, COLORREF *bg, COLORREF *text, COLORREF *line)
{
    COLORREF c = GetPixel(dc, rc->right - 4, 4);
    if (c == CLR_INVALID) c = RGB(43, 43, 43);
    *bg = c;
    int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
    *text = (lum < 128) ? RGB(240, 240, 240) : RGB(16, 16, 16);
    *line = Blend(c, *text, 30);
}

static HWND FindTree(void)
{
    static HWND cached = NULL;
    if (cached && IsWindow(cached)) return cached;
    cached = NULL;

    HWND top = NULL;
    while ((top = FindWindowExW(NULL, top, L"PBFRAME", NULL)) != NULL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(top, &pid);
        if (pid != GetCurrentProcessId()) continue;

        HWND tree = top;
        const wchar_t *chain[] = { L"pbsheet0", L"pbwork0",
                                   L"_PLViewClass_", L"_UBBrowserViewParentClass_",
                                   L"PBTreeView32_100" };
        for (int i = 0; i < 5 && tree; i++)
            tree = FindWindowExW(tree, NULL, chain[i], NULL);
        if (tree) { cached = tree; return cached; }
    }
    return NULL;
}

static void NodeText(HWND tree, HTREEITEM hi, wchar_t *buf, int cch)
{
    TVITEMW it;
    ZeroMemory(&it, sizeof it);
    it.mask       = TVIF_TEXT | TVIF_HANDLE;
    it.hItem      = hi;
    it.pszText    = buf;
    it.cchTextMax = cch;
    buf[0] = 0;
    SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&it);
}

static BOOL CaptureNode(void)
{
    g_haveNode = FALSE;
    g_obj[0] = g_lib[0] = 0;

    HWND tree = FindTree();
    if (!tree) return FALSE;

    POINT pt;
    GetCursorPos(&pt);
    POINT client = pt;
    ScreenToClient(tree, &client);

    TVHITTESTINFO ht;
    ZeroMemory(&ht, sizeof ht);
    ht.pt = client;
    HTREEITEM hi = (HTREEITEM)SendMessageW(tree, TVM_HITTEST, 0, (LPARAM)&ht);
    if (!hi || !(ht.flags & (TVHT_ONITEM | TVHT_ONITEMICON | TVHT_ONITEMLABEL)))
        return FALSE;

    NodeText(tree, hi, g_obj, 512);

    HTREEITEM parent = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)hi);
    if (parent) NodeText(tree, parent, g_lib, 512);

    g_haveNode = (g_obj[0] != 0);
    return g_haveNode;
}

/* ---- owner-draw ---------------------------------------------------------- */

static void MeasureItem(HWND owner, MEASUREITEMSTRUCT *mis)
{
    CFGITEM *d = (CFGITEM *)mis->itemData;
    if (d && d->isSep) {
        mis->itemWidth  = 8;
        mis->itemHeight = g_pbSepH ? (UINT)g_pbSepH : 6;
        return;
    }
    HDC dc = GetDC(owner);
    HFONT old = (HFONT)SelectObject(dc, MenuFont());
    SIZE sz = { 0, 0 };
    if (d) GetTextExtentPoint32W(dc, d->text, lstrlenW(d->text), &sz);
    SelectObject(dc, old);
    ReleaseDC(owner, dc);
    mis->itemWidth  = sz.cx + LEFT_GUTTER + RIGHT_PAD;
    mis->itemHeight = g_pbItemH ? (UINT)g_pbItemH : ((sz.cy < 20) ? 20 : sz.cy + 4);
}

static void DrawItem(DRAWITEMSTRUCT *dis)
{
    CFGITEM *d = (CFGITEM *)dis->itemData;
    COLORREF bg, text, line;
    ThemeColors(dis->hDC, &dis->rcItem, &bg, &text, &line);

    if (d && d->isSep) {
        HBRUSH b = CreateSolidBrush(bg);
        FillRect(dis->hDC, &dis->rcItem, b);
        DeleteObject(b);
        int y = (dis->rcItem.top + dis->rcItem.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, line);
        HPEN oldp = (HPEN)SelectObject(dis->hDC, pen);
        MoveToEx(dis->hDC, dis->rcItem.left + 4, y, NULL);
        LineTo(dis->hDC, dis->rcItem.right - 4, y);
        SelectObject(dis->hDC, oldp);
        DeleteObject(pen);
        return;
    }

    BOOL enabled = !(dis->itemState & ODS_DISABLED);
    BOOL sel = (dis->itemState & ODS_SELECTED) != 0;
    HBRUSH b = CreateSolidBrush(sel ? Blend(bg, text, 26) : bg);
    FillRect(dis->hDC, &dis->rcItem, b);
    DeleteObject(b);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, enabled ? text : Blend(bg, text, 45));   /* dim if disabled */
    HFONT old = (HFONT)SelectObject(dis->hDC, MenuFont());
    RECT r = dis->rcItem;
    r.left += LEFT_GUTTER;
    if (d) DrawTextW(dis->hDC, d->text, -1, &r,
                     DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    SelectObject(dis->hDC, old);
}

/* ---- subclass of the menu-owner window ---------------------------------- */

static LRESULT CALLBACK OwnerProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_MEASUREITEM) {
        MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)l;
        if (mis->CtlType == ODT_MENU && IsOurId(mis->itemID)) {
            MeasureItem(h, mis);
            return TRUE;
        }
        if (mis->CtlType == ODT_MENU) {        /* an IDE item: let it size, then learn */
            LRESULT r = CallWindowProcW(g_prevProc, h, msg, w, l);
            if (mis->itemID == 0) { if ((int)mis->itemHeight > 0) g_pbSepH = (int)mis->itemHeight; }
            else if ((int)mis->itemHeight > g_pbItemH) g_pbItemH = (int)mis->itemHeight;
            return r;
        }
    }
    else if (msg == WM_DRAWITEM) {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)l;
        if (dis->CtlType == ODT_MENU && IsOurId(dis->itemID)) {
            DrawItem(dis);
            return TRUE;
        }
    }
    else if (msg == WM_NCDESTROY) {
        WNDPROC prev = g_prevProc;
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)prev);
        if (g_owner == h) g_owner = NULL;
        return CallWindowProcW(prev, h, msg, w, l);
    }
    return CallWindowProcW(g_prevProc, h, msg, w, l);
}

/* (Re)install our proc on the owner each popup; the IDE swaps its own back between
 * menus. Only save the IDE's proc when ours isn't already in place (no self-chain). */
static void EnsureSubclass(HWND owner)
{
    if (owner != g_owner && g_owner && IsWindow(g_owner) &&
        (WNDPROC)GetWindowLongPtrW(g_owner, GWLP_WNDPROC) == OwnerProc)
        SetWindowLongPtrW(g_owner, GWLP_WNDPROC, (LONG_PTR)g_prevProc);

    if ((WNDPROC)GetWindowLongPtrW(owner, GWLP_WNDPROC) != OwnerProc) {
        g_prevProc = (WNDPROC)SetWindowLongPtrW(owner, GWLP_WNDPROC, (LONG_PTR)OwnerProc);
        g_owner = owner;
    }
}

static void AppendOurItems(HMENU menu)
{
    /* Remove any copy from a previous popup (in case the IDE reuses the menu). */
    RemoveMenu(menu, ID_SEP, MF_BYCOMMAND);
    for (int i = 0; i < MAX_ITEMS; i++) RemoveMenu(menu, ID_FIRST_ITEM + i, MF_BYCOMMAND);

    AppendMenuW(menu, MF_OWNERDRAW | MF_DISABLED, ID_SEP, (LPCWSTR)&g_sep);

    if (g_nitems == 0) {
        ZeroMemory(&g_items[0], sizeof g_items[0]);
        wcscpy(g_items[0].text, L"(edit pbsystreehook.ini to add menu items)");
        AppendMenuW(menu, MF_OWNERDRAW | MF_DISABLED, ID_FIRST_ITEM, (LPCWSTR)&g_items[0]);
        return;
    }
    for (int i = 0; i < g_nitems; i++) {
        Expand(g_items[i].text, 400, g_items[i].label);
        AppendMenuW(menu, MF_OWNERDRAW, ID_FIRST_ITEM + i, (LPCWSTR)&g_items[i]);
    }
}

/* ---- hook procedures (run in the PB thread) ----------------------------- */

LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        CWPSTRUCT *p = (CWPSTRUCT *)lParam;
        if (p->message == WM_INITMENUPOPUP) {
            if (HIWORD(p->lParam) == 0 && CaptureNode()) {
                LoadConfig();           /* first: this sets the logging flag */
                Log("INITPOPUP owner=%08x obj=\"%S\" lib=\"%S\"",
                    (unsigned)(UINT_PTR)p->hwnd, g_obj, g_lib);
                EnsureSubclass(p->hwnd);
                AppendOurItems((HMENU)p->wParam);
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        MSG *m = (MSG *)lParam;
        if (m->message == WM_COMMAND && HIWORD(m->wParam) == 0) {
            int id = LOWORD(m->wParam);
            if (id >= ID_FIRST_ITEM && id < ID_FIRST_ITEM + g_nitems && g_haveNode) {
                RunItem(&g_items[id - ID_FIRST_ITEM]);
                m->message = WM_NULL;       /* swallow it so PB doesn't also react */
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

/* ---- exports called by the loader (in the loader's process) ------------- */

__declspec(dllexport) BOOL InstallHook(DWORD threadId)
{
    g_hCwp    = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, g_hinst, threadId);
    g_hGetMsg = SetWindowsHookExW(WH_GETMESSAGE,  GetMsgProc,  g_hinst, threadId);
    return g_hCwp && g_hGetMsg;
}

__declspec(dllexport) void RemoveHook(void)
{
    if (g_hCwp)    UnhookWindowsHookEx(g_hCwp);
    if (g_hGetMsg) UnhookWindowsHookEx(g_hGetMsg);
    g_hCwp = g_hGetMsg = NULL;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_owner && IsWindow(g_owner) &&
            (WNDPROC)GetWindowLongPtrW(g_owner, GWLP_WNDPROC) == OwnerProc)
            SetWindowLongPtrW(g_owner, GWLP_WNDPROC, (LONG_PTR)g_prevProc);
        if (g_menuFont) DeleteObject(g_menuFont);
    }
    return TRUE;
}
