#include "GameHooks.h"
#include "Log.h"
#include "Overlay.h"
#include "GameOffsets.h"   // ../shared : game::FromRVA + the hand-found RVAs

#include <MinHook.h>
#include <string>

namespace sow {

GameHooks& GameHooks::Get() { static GameHooks instance; return instance; }

// Claim-once so the overlay is installed exactly one time no matter which path gets there first
// (the front-end trigger on the game thread, or the worker-thread fallback).
static volatile LONG s_overlayClaimed = 0;
static bool          s_ctorLogged     = false;
static bool          g_menuLive       = false;   // set when the front-end root layer is constructed
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
    g_menuLive = true; s_locCap = 300;   // arm the loc-key logger for the menu-build window
    EnsureOverlay("frontend-rootlayer-ctor");
    return oRootLayerCtor(self, a2, a3);   // forward to original (trampoline; never destructive)
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
            WalkMenuList(container, 0x4b8, 0x4b0, "L0", false);
            WalkMenuList(container, 0x490, 0x488, "L1", true);
            WalkMenuList(container, 0x568, 0x560, "L2", false);
            log.Line("[menuprobe] done");
            RelabelExperiment(container);
        }
    }
    return oItemRefresh(self);
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

    // Archive file-find (load-time, safe) — to locate + in-memory-patch the menu-definition resource.
    void* ffTgt = reinterpret_cast<void*>(game::FromRVA(0x1403ddad8ull));
    if (MH_CreateHook(ffTgt, reinterpret_cast<void*>(&HookFindFile),
                      reinterpret_cast<void**>(&oFindFile)) == MH_OK && MH_EnableHook(ffTgt) == MH_OK) {
        log.Line("[gamehooks] archive file-find hooked (menu-resource probe)");
    } else {
        log.Line("[gamehooks] hook archive file-find FAILED");
    }
}

void GameHooks::InstallOverlayFallback() {
    EnsureOverlay("worker-fallback");
}

}  // namespace sow
