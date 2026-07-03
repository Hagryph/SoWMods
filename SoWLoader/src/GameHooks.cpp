#include "GameHooks.h"
#include "Log.h"
#include "Overlay.h"
#include "Loader.h"        // Loader::OnGameWindow -> open console
#include "GameOffsets.h"   // ../shared : game::FromRVA + the hand-found RVAs

#include <MinHook.h>
#include <string>
#include <string.h>   // _strnicmp
#include <intrin.h>   // _ReturnAddress

namespace sow {

// ---- game main-window creation (RE'd 2026-07-02, kept in shared/GameOffsets.h) ----
// kWinMainThread (FUN_140c24db0) registers + creates THE game window (class L"Shadow of War")
// through the engine's own resolved-API table (NOT the IAT), and kPostWindowInit (0x1411798ac)
// is the first engine call after CreateWindowExW + ShowWindow. Trampolining kPostWindowInit fired
// correctly but proved TOO EARLY for the console: at that instant the window is an unactivated
// 336x239 stub, so the console still won the foreground and became the "main" window. The console
// trigger now lives in WindowWatch (SetWinEventHook in-context): the console opens when the game
// window BECOMES FOREGROUND — the only moment that guarantees it drops behind the game.

GameHooks& GameHooks::Get() { static GameHooks instance; return instance; }

// Claim-once so the overlay is installed exactly one time no matter which path gets there first
// (the front-end trigger on the game thread, or the worker-thread fallback).
static volatile LONG s_overlayClaimed = 0;
static bool          s_ctorLogged     = false;
static bool          g_menuLive       = false;   // set when the front-end root layer is constructed

// Game-state signal = a real member of the front-end root layer. RE showed SoW builds the menu once and
// toggles VISIBILITY (the layer is hidden, not destroyed, on load — so there's no dtor/lifecycle event).
// A HagIPC hardware write breakpoint pinned the exact state variable: front-end + 0x2c (the menu-item
// list's size/flag) is NON-ZERO while the menu is shown and 0 once gameplay takes over. Its writer is a
// shared vector-clear (not cleanly hookable), so we simply READ it off the instance we capture in the
// ctor — a definitive menu-vs-in-save state, no heartbeat, no polling of a timing signal.
static void* volatile  g_frontEnd     = nullptr;   // captured front-end root layer instance
static constexpr unsigned kActiveOff  = 0x2c;      // front-end member: != 0 at menu, 0 in a save
static volatile LONG s_svCap          = 0;       // SetVariable log budget (armed at menu build)
static volatile LONG s_locCap         = 0;       // loc-key log budget (armed at menu build)
static volatile LONG s_facCap         = 0;       // factory-classNode log budget (armed at menu build)

static void EnsureOverlay(const char* who) {
    if (::InterlockedCompareExchange(&s_overlayClaimed, 1, 0) == 0) {
        Log::Get().Line(std::string("[init] installing overlay (trigger: ") + who + ")");
        Overlay::Get().Install();
    }
}

// CUIFrontEndRootLayer::CUIFrontEndRootLayer(this, ...): constructs the front-end menu root UI
// layer and returns `this`. The factory passes up to 3 register args; we forward all of them.
using RootLayerCtorFn = void* (__fastcall*)(void*, void*, void*);
static RootLayerCtorFn oRootLayerCtor = nullptr;

static void* __fastcall HookRootLayerCtor(void* self, void* a2, void* a3) {
    if (!s_ctorLogged) {
        s_ctorLogged = true;
        Log::Get().Line("[frontend] CUIFrontEndRootLayer ctor hit — START MENU UI created");
    }
    g_menuLive = true; s_locCap = 300;                 // arm the loc-key logger for the menu-build window
    g_frontEnd = self;                                 // capture the instance; InSave() reads self+0x2c
    EnsureOverlay("frontend-rootlayer-ctor");
    return oRootLayerCtor(self, a2, a3);                // forward to original (trampoline; never destructive)
}

// Scaleform SetVariable(movie, pathObj, value, p4, flag). pathObj's first field points to the path
// string. Log the paths pushed during the menu build to learn how item labels are set.
using SetVarFn = void* (__fastcall*)(void*, void*, void*, void*, char);
static SetVarFn oSetVar = nullptr;
static void* __fastcall HookSetVar(void* p1, void* p2, void* p3, void* p4, char p5) {
    if (g_menuLive && s_svCap > 0 && p2 && !::IsBadReadPtr(p2, 8)) {
        void* a = *reinterpret_cast<void**>(p2);
        if (a && !::IsBadReadPtr(a, 8)) {
            char* path = *reinterpret_cast<char**>(a);
            if (path && !::IsBadReadPtr(path, 1) && ::InterlockedDecrement(&s_svCap) >= 0) {
                char line[200]; ::wsprintfA(line, "[setvar] %.150s", path);
                Log::Get().Line(line);
            }
        }
    }
    return oSetVar(p1, p2, p3, p4, p5);
}

// Localization: FUN_140471f2c(const char* key) -> wchar_t* localized text (via the StringDatabase).
// Called just before GFxValue::SetText during the menu build. We log the keys and, as a proof,
// override any "Benchmark" key to L"HagUI" so the native Run Benchmark button reads "HagUI".
using LocFn = wchar_t* (__fastcall*)(const char*);
static LocFn oLoc = nullptr;
static wchar_t* __fastcall HookLoc(const char* key) {
    if (key && !::IsBadReadPtr(key, 2) && (unsigned char)key[0] >= 0x20 && (unsigned char)key[0] <= 0x7e) {
        // Our added item's button key -> "HagUI". (Run Benchmark keeps its own text.)
        if (::strcmp(key, "UI_Button_HagUI") == 0) return const_cast<wchar_t*>(L"HagUI");
        if (g_menuLive && s_locCap > 0 && ::InterlockedDecrement(&s_locCap) >= 0) {
            char line[160]; ::wsprintfA(line, "[loc] %.140s", key); Log::Get().Line(line);
        }
    }
    return oLoc(key);
}

// UI object factory FUN_141940900(ctx1, ctx2, classNode). During the front-end build we let the first
// standard menu item be created normally, then call the factory AGAIN with the same (valid) args to
// construct a second, fully-owned duplicate item. Step 1: prove an extra valid item appears.
// NOTE: hooking the generic UI object factory (kUIObjectFactory) CRASHES the game — it is an
// extremely hot, multi-threaded, low-level path and MinHook-trampolining it destabilizes the process
// (crashed even in log-only mode). Do NOT hook it. A native item must be added a different way.

// Archive file-find-by-name: FUN_1403ddad8(tree, const char* name). Load-time (safe to hook). We log
// menu-related names to find which resource carries the front-end menu definition, so we can patch
// its bytes in memory as it's read (in-memory patch — no file override, keeps full mod compatibility).
using FindFileFn = void* (__fastcall*)(void*, const char*);
static FindFileFn oFindFile = nullptr;
static volatile LONG s_ffCap = 200;
static void* __fastcall HookFindFile(void* tree, const char* name) {
    if (name && s_ffCap > 0 && !::IsBadReadPtr(name, 2)) {
        const unsigned char c0 = (unsigned char)name[0];
        const bool menuish = ::strstr(name, "ront") || ::strstr(name, "Front") ||
                             ::strstr(name, "MenuLayer") || ::strstr(name, "menulayer") ||
                             ::strstr(name, "frontend") || ::strstr(name, "FrontEnd");
        const bool spam = ::strstr(name, "preload") || ::strstr(name, "_tex") || ::strstr(name, "tex.");
        if (c0 >= 0x20 && c0 <= 0x7e && menuish && !spam) {
            if (::InterlockedDecrement(&s_ffCap) >= 0) {
                char line[220]; ::wsprintfA(line, "[file] %.190s", name); Log::Get().Line(line);
            }
        }
    }
    return oFindFile(tree, name);
}

// ---- Scaleform resource-open probe + loose-SWF load proof ----
// FUN_141205c80(resMgr, const char* path, uint flags) -> resource handle. Load-time, safe to hook.
// We (1) log the paths the UI opens and capture the UI resource manager, then (2) ONCE, on the game
// thread, open our loose file interface\hagui\HagUI.swf and log the returned handle — proving the
// engine resolves + loads our loose .swf from Game\ (delivery + open, end to end).
using ResOpenFn = void* (__fastcall*)(void*, const char*, unsigned);
static ResOpenFn      oResOpen = nullptr;
static void* volatile g_uiResMgr = nullptr;
static volatile LONG  s_resCap = 60;
static bool           g_ourLoadTried = false;
static bool           g_triggered = false;   // one-shot route-2 hijack load test
static void* volatile g_ourMovie = nullptr;  // our loaded movie resource (ref held)
static bool           g_attached = false;    // one-shot display attach

// One-shot: try opening our loose movie via several (path, flag) combos with the game's OWN valid
// resource manager, DURING the active load phase (called from inside the open hook). oResOpen is the
// trampoline (original), so these calls bypass our hook — no reentrancy. Logs each handle so we learn
// which location/extension/flag the loose-file resolver accepts.
static void ProveLooseSwf(void* resMgr) {
    if (g_ourLoadTried || !resMgr || !oResOpen) return;
    g_ourLoadTried = true;
    auto& log = Log::Get();
    struct Combo { const char* path; unsigned flags; };
    static const Combo combos[] = {
        { "interface\\flash\\HagUI.gfx", 0x10040 },
        { "interface\\flash\\HagUI.swf", 0x10040 },
        { "interface\\hagui\\HagUI.swf", 0x10040 },
        { "interface\\flash\\HagUI.swf", 0x10002 },
    };
    for (const auto& c : combos) {
        void* h = oResOpen(resMgr, c.path, c.flags);
        char line[256];
        ::wsprintfA(line, "[looseproof] open(\"%s\", 0x%x) rm=%p -> handle=%p", c.path, c.flags, resMgr, h);
        if (h) log.Good(line); else log.Line(line);
    }
}

// GFx URL file-opener FUN_14075ca08(resCtx, name, flags) -> GFx File* (a memory-file:
// +0x118 buffer, +0x128 size, +0x130 pos, vtable +0x00). ROUTE-2 HIJACK: when OUR movie name is
// requested, open a real movie to get a valid memory-file, then swap its buffer to OUR .swf bytes.
// The game's own File methods then read our bytes and the whole pipeline renders our movie.
using FileOpenFn = void* (__fastcall*)(void*, const char*, unsigned);
using AllocFn    = void* (__fastcall*)(size_t);
static FileOpenFn oFileOpen = nullptr;
static bool       g_hijackLogged = false;

// Our .swf bytes, loaded once into a game-allocator block (so the memory-file dtor frees cleanly).
static void*  g_swf     = nullptr;
static size_t g_swfSize = 0;

static bool LoadOurSwf() {
    if (g_swf) return true;
    HANDLE h = ::CreateFileW(
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\ShadowOfWar\\Game\\interface\\hagui\\HagUI.swf",
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log::Get().Error("[hijack] cannot open our HagUI.swf on disk"); return false; }
    DWORD sz = ::GetFileSize(h, nullptr);
    void* buf = nullptr;
    if (sz && sz < 0x100000) {
        auto alloc = reinterpret_cast<AllocFn>(game::FromRVA(game::kGameAlloc));
        buf = alloc(sz);
        DWORD rd = 0;
        if (buf && (!::ReadFile(h, buf, sz, &rd, nullptr) || rd != sz)) buf = nullptr;
    }
    ::CloseHandle(h);
    if (!buf) { Log::Get().Error("[hijack] failed to buffer our HagUI.swf"); return false; }
    g_swf = buf; g_swfSize = sz;
    char l[96]; ::wsprintfA(l, "[hijack] our HagUI.swf buffered: %u bytes @ %p", (unsigned)sz, buf);
    Log::Get().Line(l);
    return true;
}

static bool NameContains(const char* name, const char* needle, int nlen) {
    if (!name || ::IsBadReadPtr(name, 6)) return false;
    for (const char* p = name; *p; ++p) if (::_strnicmp(p, needle, nlen) == 0) return true;
    return false;
}

// Dump an object's identity (vtable RVA, refcount, +0x10/+0x60) so we can compare the game's real
// movie-resource against our resolved one before ever calling the view-create with it.
static void DumpObj(const char* tag, void* o) {
    auto& log = Log::Get();
    const uintptr_t base = game::Base();
    auto rva = [&](void* p) -> uintptr_t { uintptr_t v = (uintptr_t)p;
        return (v >= base && v < base + 0x8000000) ? (v - base + 0x140000000ull) : v; };
    if (!o || ::IsBadReadPtr(o, 0x68)) { char l[96]; ::wsprintfA(l, "%s = %p (unreadable)", tag, o); log.Line(l); return; }
    void** vt = *reinterpret_cast<void***>(o);
    void* m10 = *reinterpret_cast<void**>(reinterpret_cast<char*>(o) + 0x10);
    void* m60 = *reinterpret_cast<void**>(reinterpret_cast<char*>(o) + 0x60);
    int   rc  = *reinterpret_cast<int*>(reinterpret_cast<char*>(o) + 8);
    char l[220]; ::wsprintfA(l, "%s=%p vt=0x%p rc=%d [+0x10]=%p [+0x60]=%p",
                             tag, o, (void*)rva(vt), rc, m10, m60);
    log.Good(l);
}

// View create+attach FUN_14128db14(sfSys, movieRes, level) -> view. Phase 1: capture sfSys+level and
// dump a REAL movie-resource so we can attach OUR movie the same way (no call yet — compare first).
using ViewCreateFn = void* (__fastcall*)(void*, void*, unsigned);
static ViewCreateFn   oViewCreate   = nullptr;
static void* volatile g_sfSys       = nullptr;
static unsigned       g_viewLevel   = 0;
static bool           g_viewCaptured = false;

static void* __fastcall HookViewCreate(void* sfSys, void* movieRes, unsigned level) {
    if (!g_viewCaptured) {
        g_viewCaptured = true; g_sfSys = sfSys; g_viewLevel = level;
        char l[128]; ::wsprintfA(l, "[view] real create: sfSys=%p level=0x%x", sfSys, level);
        Log::Get().Good(l);
        DumpObj("[view] real movieRes", movieRes);
    }
    return oViewCreate(sfSys, movieRes, level);
}

// Universal view-register FUN_141290980(viewMgr, level, view). ALL view-create paths funnel here,
// so this captures the front-end's view + the view manager regardless of which path made it.
using ViewRegFn = void* (__fastcall*)(void*, unsigned, void*);
static ViewRegFn      oViewReg = nullptr;
static void* volatile g_viewMgr = nullptr;   // first host
static void*          g_hosts[4] = {};       // front-end hosts in registration order
static volatile LONG  g_hostN = 0;
static volatile LONG  s_vregCap = 12;

// Hook FUN_14122d400(movie, descriptor, p3, p4, p5) = create-host-from-descriptor + bind movie.
// Capture + HOLD a descriptor (vt 0x1422C9DF8) so we can call it with OUR movie -> a fresh,
// render-registered host that nothing else drives. (FUN_14128e918(host,ourMovie) already proven OK.)
using CH2Fn = void* (__fastcall*)(void*, void*, char, void*, void*);
static CH2Fn          oCH2 = nullptr;
static void*          g_descArr[4] = {};   // live descriptors held via addref, in creation order
static volatile LONG  g_descN = 0;
static void* volatile g_p1vt = nullptr;    // the movie arg's vtable (validate our substitution)
static volatile LONG  s_ch2 = 6;

static void* __fastcall HookCH2(void* p1, void* p2, char p3, void* p4, void* p5) {
    void* host = oCH2(p1, p2, p3, p4, p5);
    if (::InterlockedDecrement(&s_ch2) >= 0) {
        LONG i = g_descN;
        if (i < 4 && p2 && !::IsBadReadPtr(p2, 0x10)) {
            g_descArr[i] = p2; g_descN = i + 1;
            ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<char*>(p2) + 8));  // hold it
            if (p1 && !::IsBadReadPtr(p1, 8)) g_p1vt = *reinterpret_cast<void**>(p1);
        }
        DumpObj("[ch2] p1(movie)", p1);
        DumpObj("[ch2] p2(desc)", p2);
    }
    return host;
}

static void* __fastcall HookViewReg(void* viewMgr, unsigned level, void* view) {
    if (::InterlockedDecrement(&s_vregCap) >= 0) {
        if (!g_viewMgr) g_viewMgr = viewMgr;
        LONG i = g_hostN;
        if (i < 4) { g_hosts[i] = viewMgr; g_hostN = i + 1; }
        char l[128]; ::wsprintfA(l, "[vreg] register #%d viewMgr=%p level=0x%x view=%p", (int)i, viewMgr, level, view);
        Log::Get().Good(l);
        DumpObj("[vreg] view", view);
    }
    return oViewReg(viewMgr, level, view);
}
static bool NameIsOurs(const char* name) {
    // Additive only: serve our bytes solely for OUR path. (Shadowing a real menu movie like sp.gfx
    // CRASHES — the game's C++ hard-derefs that movie's clips, which our dummy lacks.)
    return NameContains(name, "hagui", 5);
}

static void* __fastcall HookFileOpen(void* resCtx, const char* name, unsigned flags) {
    if (NameIsOurs(name) && LoadOurSwf()) {
        // Get a real memory-file as a template, then repoint it at our bytes.
        void* tmpl = oFileOpen(resCtx, "interface\\flash\\messageboxes.gfx", flags);
        if (tmpl && !::IsBadReadPtr(tmpl, 0x138)) {
            auto p = reinterpret_cast<uint8_t*>(tmpl);
            *reinterpret_cast<void**> (p + 0x118) = g_swf;                 // buffer base
            *reinterpret_cast<int64_t*>(p + 0x128) = (int64_t)g_swfSize;   // size / end
            *reinterpret_cast<int64_t*>(p + 0x130) = 0;                    // position
            if (!g_hijackLogged) { g_hijackLogged = true;
                char l[160]; ::wsprintfA(l, "[hijack] served our .swf (%u b) as \"%.80s\" via file=%p",
                                         (unsigned)g_swfSize, name, tmpl);
                Log::Get().Good(l); }
            return tmpl;
        }
        Log::Get().Error("[hijack] template open failed");
    }
    return oFileOpen(resCtx, name, flags);
}

static void* __fastcall HookResOpen(void* resMgr, const char* path, unsigned flags) {
    if (path) {
        const bool iface = (::_strnicmp(path, "interface", 9) == 0);
        if (iface && !g_uiResMgr) g_uiResMgr = resMgr;   // capture the UI resource manager
        if (s_resCap > 0 && ::InterlockedDecrement(&s_resCap) >= 0) {   // diagnostic: first N paths
            char line[220]; ::wsprintfA(line, "[resopen] rm=%p f=0x%x %.170s", resMgr, flags, path);
            Log::Get().Line(line);
        }
        return oResOpen(resMgr, path, flags);
    }
    return oResOpen(resMgr, path, flags);
}

// ---- Menu-item probe (route (b) groundwork): read the live START-menu item list ONCE ----
using ItemRefreshFn = void* (__fastcall*)(void*);
static ItemRefreshFn oItemRefresh = nullptr;
static bool s_itemsLogged = false;

// item+0x28 -> name object; name object: (+1 & 2) flag, +0x20 -> char* interned name (per FUN_14018bdb0).
static const char* MenuItemName(void* item) {
    if (!item || ::IsBadReadPtr(item, 0x30)) return nullptr;
    void* nameObj = *reinterpret_cast<void**>(reinterpret_cast<char*>(item) + 0x28);
    if (!nameObj || ::IsBadReadPtr(nameObj, 0x28)) return nullptr;
    if ((*(reinterpret_cast<unsigned char*>(nameObj) + 1) & 2) == 0) return nullptr;
    char* s = *reinterpret_cast<char**>(reinterpret_cast<char*>(nameObj) + 0x20);
    if (!s || ::IsBadReadPtr(s, 1)) return nullptr;
    return s;
}

// Treat p as a name/string object (per FUN_14018bdb0: (+1 & 2) flag, +0x20 -> char*) and log its text.
static void TryStrField(const char* label, void* p) {
    if (!p || ::IsBadReadPtr(p, 0x28)) return;
    if ((*(reinterpret_cast<unsigned char*>(p) + 1) & 2) == 0) return;
    char* s = *reinterpret_cast<char**>(reinterpret_cast<char*>(p) + 0x20);
    if (!s || ::IsBadReadPtr(s, 1)) return;
    char line[256]; ::wsprintfA(line, "       %s -> \"%s\"", label, s);
    Log::Get().Line(line);
}

// If p points at a readable ASCII or UTF-16 string, log a preview (finds labels behind pointers).
static void PreviewPtr(unsigned off, void* p) {
    if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000) return;
    char* c = reinterpret_cast<char*>(p);
    if (!::IsBadReadPtr(c, 8)) {
        bool ascii = c[0] >= 0x20 && (unsigned char)c[0] <= 0x7e;
        for (int i = 0; i < 6 && ascii; i++) { unsigned char ch = c[i]; if (ch == 0) break; if (ch < 0x20 || ch > 0x7e) ascii = false; }
        if (ascii) { char l[128]; ::wsprintfA(l, "         +0x%02x A:\"%.40s\"", off, c); Log::Get().Line(l); }
    }
    wchar_t* w = reinterpret_cast<wchar_t*>(p);
    if (!::IsBadReadPtr(w, 16)) {
        bool uni = w[0] >= 0x20 && w[0] <= 0x7e;
        for (int i = 0; i < 6 && uni; i++) { wchar_t ch = w[i]; if (ch == 0) break; if (ch < 0x20 || ch > 0x7e) uni = false; }
        if (uni) { char nar[128]{}; ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nar, sizeof(nar), nullptr, nullptr);
                   char l[160]; ::wsprintfA(l, "         +0x%02x W:\"%.40s\"", off, nar); Log::Get().Line(l); }
    }
}

// Visited-set recursive scanner for readable strings (ASCII + UTF-16) in an object's child graph,
// to locate the display-label buffer ("START"/"RUN BENCHMARK") baked into the LithTech button widget.
static int   s_scanCap = 0;
static void* s_vis[800];
static int   s_visN = 0;
static bool Seen(void* p) { for (int i = 0; i < s_visN; ++i) if (s_vis[i] == p) return true; if (s_visN < 800) s_vis[s_visN++] = p; return false; }

static void ScanStrings(void* obj, int depth, const char* path) {
    if (s_scanCap <= 0 || !obj || reinterpret_cast<uintptr_t>(obj) < 0x10000 || ::IsBadReadPtr(obj, 0x40) || Seen(obj)) return;
    for (unsigned off = 0; off <= 0xF8 && s_scanCap > 0; off += 8) {
        void* v = *reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + off);
        if (!v || reinterpret_cast<uintptr_t>(v) < 0x10000) continue;
        if (!::IsBadReadPtr(v, 12)) {   // UTF-16 only (ASCII config keys are noise here)
            wchar_t* w = reinterpret_cast<wchar_t*>(v);
            bool u = w[0] >= 0x20 && w[0] <= 0x7e && w[1] >= 0x20 && w[1] <= 0x7e;
            for (int i = 0; i < 5 && u; i++) { wchar_t ch = w[i]; if (ch == 0) break; if (ch < 0x20 || ch > 0x7e) u = false; }
            if (u) { char nar[80]{}; ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nar, sizeof(nar), nullptr, nullptr);
                     char l[160]; ::wsprintfA(l, "    %s+%02x W:\"%.36s\"", path, off, nar); Log::Get().Line(l); --s_scanCap; }
        }
        const uintptr_t pv = reinterpret_cast<uintptr_t>(v);
        if (depth > 0 && (pv < 0x140000000 || pv > 0x148000000)) {
            char np[64]; ::wsprintfA(np, "%s+%02x", path, off);
            ScanStrings(v, depth - 1, np);
        }
    }
}

// Deep-dump an item: qwords 0x00..0xF8, resolving name-objects and previewing any string behind a ptr.
static void DumpItem(void* item) {
    if (!item || ::IsBadReadPtr(item, 0x100)) return;
    for (unsigned off = 0x00; off <= 0xF8; off += 8) {
        void* v = *reinterpret_cast<void**>(reinterpret_cast<char*>(item) + off);
        char line[96]; ::wsprintfA(line, "       +0x%02x = %p", off, v);
        Log::Get().Line(line);
        char lbl[16]; ::wsprintfA(lbl, "+0x%02x", off);
        TryStrField(lbl, v);          // name-object pattern
        PreviewPtr(off, v);           // raw ASCII / UTF-16 behind the pointer
    }
}

// Walk one intrusive list (head at container+headOff, sentinel = container+sentOff;
// node+0x8 = next, node+0x10 = item). If dump, dump each item's struct + string fields.
static void WalkMenuList(void* container, unsigned headOff, unsigned sentOff, const char* tag, bool dump) {
    auto& log = Log::Get();
    char* sentinel = reinterpret_cast<char*>(container) + sentOff;
    char* node = *reinterpret_cast<char**>(reinterpret_cast<char*>(container) + headOff);
    for (int i = 0; i < 64 && node && node != sentinel; ++i) {
        if (::IsBadReadPtr(node, 0x18)) break;
        void* item = *reinterpret_cast<void**>(node + 0x10);
        const char* name = MenuItemName(item);
        void* cls = (item && !::IsBadReadPtr(item, 0x20)) ? *reinterpret_cast<void**>(reinterpret_cast<char*>(item) + 0x18) : nullptr;
        char line[256];
        ::wsprintfA(line, "  [%s] node=%p item=%p cls=%p name=%s", tag, node, item, cls, name ? name : "(null)");
        log.Line(line);
        if (dump && i == 0) { s_visN = 0; s_scanCap = 800; ScanStrings(item, 9, "i"); }  // deep UTF-16 label hunt
        node = *reinterpret_cast<char**>(node + 8);
    }
}

// Experiment: rename an existing item's loc-key buffer in place to "HagUI" to learn (a) whether the
// menu re-localizes the label live and (b) whether a missing loc key falls back to the raw key text.
static char* Deref(void* base, unsigned off, size_t need) {
    if (!base || ::IsBadReadPtr(base, off + 8)) return nullptr;
    void* v = *reinterpret_cast<void**>(reinterpret_cast<char*>(base) + off);
    if (!v || ::IsBadReadPtr(v, need)) return nullptr;
    return reinterpret_cast<char*>(v);
}

// Overwrite the FIRST item's button loc-key (path item+08+00+20+10, key ptr at +00) to "HagUI".
// Tests whether the label re-resolves live and whether a missing key falls back to the raw text.
static void RelabelExperiment(void* container) {
    char* sentinel = reinterpret_cast<char*>(container) + 0x488;
    char* node = *reinterpret_cast<char**>(reinterpret_cast<char*>(container) + 0x490);
    if (!node || node == sentinel || ::IsBadReadPtr(node, 0x18)) return;
    void* item = *reinterpret_cast<void**>(node + 0x10);
    if (!item || ::IsBadReadPtr(item, 0x40)) return;

    char* o1 = Deref(item, 0x08, 0x40); if (!o1) return;
    char* o2 = Deref(o1,  0x00, 0x40); if (!o2) return;
    char* o3 = Deref(o2,  0x20, 0x40); if (!o3) return;
    char* o4 = Deref(o3,  0x10, 0x40); if (!o4) return;

    // Log the widget-class vtables along the chain (relative to image base) so we can decompile the
    // build/text methods in Ghidra and find the build-time loc call.
    const uintptr_t base = game::Base();
    auto vt = [&](char* o) -> uintptr_t { void* v = *reinterpret_cast<void**>(o); uintptr_t p = reinterpret_cast<uintptr_t>(v);
        return (p >= base && p < base + 0x8000000) ? (p - base + 0x140000000ull) : p; };
    char line[220];
    ::wsprintfA(line, "[chain] o1.vt=%p o2.vt=%p o3.vt=%p o4.vt=%p (o4 is the button widget)",
                reinterpret_cast<void*>(vt(o1)), reinterpret_cast<void*>(vt(o2)),
                reinterpret_cast<void*>(vt(o3)), reinterpret_cast<void*>(vt(o4)));
    Log::Get().Good(line);
}

static void* __fastcall HookItemRefresh(void* self) {
    if (!s_itemsLogged && self && !::IsBadReadPtr(self, 0x5410)) {
        void* container = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x53f8);
        int count = *reinterpret_cast<int*>(reinterpret_cast<char*>(self) + 0x5400);
        if (container && !::IsBadReadPtr(container, 0x570)) {
            s_itemsLogged = true;
            auto& log = Log::Get();
            char hdr[160];
            ::wsprintfA(hdr, "[menuprobe] layer=%p container=%p count=%d - walking the 3 item lists:", self, container, count);
            log.Line(hdr);
            WalkMenuList(container, 0x490, 0x488, "L1", false);   // item names only (deep scan off)
            log.Line("[menuprobe] done");
        }
    }
    // One-shot route-2 test: drive OUR path through the resmgr -> resolver -> file-opener HIJACK
    // -> SWF parse -> MovieDef. Non-null handle == our loose .swf loaded additively (game thread).
    if (!g_triggered && g_uiResMgr && oResOpen) {
        g_triggered = true;
        void* h = oResOpen(g_uiResMgr, "interface\\hagui\\HagUI.swf", 0x10040);
        char l[160]; ::wsprintfA(l, "[route2] resmgr open(our path) via hijack -> handle=%p", h);
        if (h) Log::Get().Good(l); else Log::Get().Error(l);
        DumpObj("[route2] our movie", h);
        if (h && !::IsBadReadPtr(h, 0x10)) {   // keep a ref so it survives to the attach
            ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<char*>(h) + 8));
            g_ourMovie = h;
        }
    }
    // Route-2 DISPLAY attempt (game thread): create+register a view for OUR movie on a captured
    // front-end host via FUN_14128e918(host, movieRes). Fires a few frames after the load so state
    // is settled. Heavily logged so we see how far it gets if it faults.
    // Route-2 DISPLAY (fresh host): drive the game's own "show movie" entry with OUR movie. This
    // creates a brand-new host that nothing else drives (no crash) and shows our movie on top.
    // Guarded: only if our movie is the same type the game passes (vtable match).
    if (g_triggered && !g_attached && g_ourMovie && g_descN > 0 && oCH2) {
        static int s_d = 0;
        if (++s_d >= 3) {
            g_attached = true;
            void* ourVt = (!::IsBadReadPtr(g_ourMovie, 8)) ? *reinterpret_cast<void**>(g_ourMovie) : nullptr;
            void* desc = g_descArr[g_descN - 1];   // LAST descriptor = sp.gfx (the visible main menu)
            if (ourVt && ourVt == g_p1vt) {
                char l[200]; ::wsprintfA(l, "[attach2] FUN_14122d400(ourMovie=%p, desc#%d=%p, p3=1, 0, 0) ...",
                                         g_ourMovie, (int)g_descN - 1, desc);
                Log::Get().Good(l);
                void* host = oCH2(g_ourMovie, desc, 1, nullptr, nullptr);
                char l2[128]; ::wsprintfA(l2, "[attach2] fresh host=%p (survived!)", host);
                Log::Get().Good(l2);
            } else {
                char l[160]; ::wsprintfA(l, "[attach2] SKIP: vt mismatch ourVt=%p gameVt=%p", ourVt, g_p1vt);
                Log::Get().Error(l);
            }
        }
    }
    return oItemRefresh(self);
}

// In a save iff the front-end root layer exists AND its active member (self+0x2c) has been cleared to 0
// (menu hidden, gameplay running). Direct state read of the variable pinned by the write breakpoint —
// no heartbeat, no timing. Guarded: null until the menu is first built; SEH-safe on the member read.
bool GameHooks::InSave() {
    void* fe = g_frontEnd;
    if (!fe) return false;
    __try {
        return *reinterpret_cast<volatile int*>(reinterpret_cast<char*>(fe) + kActiveOff) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void GameHooks::Install() {
    auto& log = Log::Get();

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { log.Line("[gamehooks] MH_Initialize failed"); return; }

    void* target = reinterpret_cast<void*>(game::FromRVA(game::kCUIFrontEndRootLayerCtor));
    char addr[32]{}; ::wsprintfA(addr, "0x%p", target);

    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookRootLayerCtor),
                      reinterpret_cast<void**>(&oRootLayerCtor)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        log.Line(std::string("[gamehooks] CUIFrontEndRootLayer ctor hooked @ ") + addr + " — awaiting start menu");
    } else {
        log.Line(std::string("[gamehooks] MinHook create/enable CUIFrontEndRootLayer ctor FAILED @ ") + addr);
    }

    // Menu-item probe: hook the front-end item-refresh so we read the live item list once (route (b)).
    void* itemTgt = reinterpret_cast<void*>(game::FromRVA(game::kFrontEndItemRefresh));
    if (MH_CreateHook(itemTgt, reinterpret_cast<void*>(&HookItemRefresh),
                      reinterpret_cast<void**>(&oItemRefresh)) == MH_OK &&
        MH_EnableHook(itemTgt) == MH_OK) {
        log.Line("[gamehooks] FrontEnd item-refresh hooked (menu-item probe)");
    } else {
        log.Line("[gamehooks] hook FrontEnd item-refresh FAILED");
    }

    // Scaleform SetVariable logger — learn how menu labels are pushed to the movie.
    void* svTgt = reinterpret_cast<void*>(game::FromRVA(0x1412a0ce4ull));
    if (MH_CreateHook(svTgt, reinterpret_cast<void*>(&HookSetVar),
                      reinterpret_cast<void**>(&oSetVar)) == MH_OK && MH_EnableHook(svTgt) == MH_OK) {
        log.Line("[gamehooks] Scaleform SetVariable hooked (label probe)");
    } else {
        log.Line("[gamehooks] hook Scaleform SetVariable FAILED");
    }

    // Localization hook — the real label unlock (loc(key)->wide text, called before SetText).
    void* locTgt = reinterpret_cast<void*>(game::FromRVA(0x140471f2cull));
    if (MH_CreateHook(locTgt, reinterpret_cast<void*>(&HookLoc),
                      reinterpret_cast<void**>(&oLoc)) == MH_OK && MH_EnableHook(locTgt) == MH_OK) {
        log.Line("[gamehooks] Localization hooked (UI_Button_HagUI -> HagUI)");
    } else {
        log.Line("[gamehooks] hook Localization FAILED");
    }

    // Scaleform resource-open (load-time, safe) — capture the UI resource manager + log opened paths.
    void* roTgt = reinterpret_cast<void*>(game::FromRVA(0x141205c80ull));
    if (MH_CreateHook(roTgt, reinterpret_cast<void*>(&HookResOpen),
                      reinterpret_cast<void**>(&oResOpen)) == MH_OK && MH_EnableHook(roTgt) == MH_OK) {
        log.Line("[gamehooks] Scaleform resource-open hooked");
    } else {
        log.Line("[gamehooks] hook Scaleform resource-open FAILED");
    }

    // GFx URL file-opener (route-2 injection point) — hijack: serve our .swf bytes for our path.
    void* foTgt = reinterpret_cast<void*>(game::FromRVA(0x14075ca08ull));
    if (MH_CreateHook(foTgt, reinterpret_cast<void*>(&HookFileOpen),
                      reinterpret_cast<void**>(&oFileOpen)) == MH_OK && MH_EnableHook(foTgt) == MH_OK) {
        log.Line("[gamehooks] GFx file-opener hooked (route-2 hijack)");
    } else {
        log.Line("[gamehooks] hook GFx file-opener FAILED");
    }

    // Movie view create+attach — capture the scaleform system + a real movie-resource shape.
    void* vcTgt = reinterpret_cast<void*>(game::FromRVA(0x14128db14ull));
    if (MH_CreateHook(vcTgt, reinterpret_cast<void*>(&HookViewCreate),
                      reinterpret_cast<void**>(&oViewCreate)) == MH_OK && MH_EnableHook(vcTgt) == MH_OK) {
        log.Line("[gamehooks] movie view-create hooked (attach capture)");
    } else {
        log.Line("[gamehooks] hook movie view-create FAILED");
    }

    // Create-host-from-descriptor — capture + hold a descriptor to reuse with our movie.
    void* chTgt = reinterpret_cast<void*>(game::FromRVA(0x14122d400ull));
    if (MH_CreateHook(chTgt, reinterpret_cast<void*>(&HookCH2),
                      reinterpret_cast<void**>(&oCH2)) == MH_OK && MH_EnableHook(chTgt) == MH_OK) {
        log.Line("[gamehooks] create-host hooked (descriptor capture)");
    } else {
        log.Line("[gamehooks] hook create-host FAILED");
    }

    // Universal view-register — captures the front-end view + view manager (whichever path made it).
    void* vrTgt = reinterpret_cast<void*>(game::FromRVA(0x141290980ull));
    if (MH_CreateHook(vrTgt, reinterpret_cast<void*>(&HookViewReg),
                      reinterpret_cast<void**>(&oViewReg)) == MH_OK && MH_EnableHook(vrTgt) == MH_OK) {
        log.Line("[gamehooks] view-register hooked (front-end view capture)");
    } else {
        log.Line("[gamehooks] hook view-register FAILED");
    }

    // Archive file-find (load-time, safe) — to locate + in-memory-patch the menu-definition resource.
    void* ffTgt = reinterpret_cast<void*>(game::FromRVA(0x1403ddad8ull));
    if (MH_CreateHook(ffTgt, reinterpret_cast<void*>(&HookFindFile),
                      reinterpret_cast<void**>(&oFindFile)) == MH_OK && MH_EnableHook(ffTgt) == MH_OK) {
        log.Line("[gamehooks] archive file-find hooked (menu-resource probe)");
    } else {
        log.Line("[gamehooks] hook archive file-find FAILED");
    }
}

}  // namespace sow
