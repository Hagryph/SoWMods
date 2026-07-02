#include "Console.h"

namespace sow {

Console& Console::Get() { static Console c; return c; }

static WORD AttrFor(Lv lv) {
    switch (lv) {
        case Lv::Good:   return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Lv::Warn:   return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Lv::Error:  return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case Lv::Accent: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        default:         return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;   // gray
    }
}

bool Console::Open() {
    if (open_) return true;
    // Opened AFTER the game has initialized (first rendered frame), so the game keeps its taskbar
    // slot and focus: remember the foreground window, alloc, then put ourselves behind it again.
    HWND fg = ::GetForegroundWindow();
    if (!::AllocConsole()) {
        // A console may already be attached; fall through and try to use STDOUT anyway.
    }
    out_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out_ || out_ == INVALID_HANDLE_VALUE) return false;
    ::InitializeCriticalSection(&cs_); csInit_ = true;
    ::SetConsoleTitleW(L"SoWLoader  -  mod console");
    if (HWND cw = ::GetConsoleWindow()) {
        ::SetWindowPos(cw, HWND_BOTTOM, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (fg) ::SetForegroundWindow(fg);
    open_ = true;

    const WORD gold = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    const WORD gray = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    auto raw = [&](const char* s, WORD a) { DWORD w; ::SetConsoleTextAttribute(out_, a); ::WriteConsoleA(out_, s, (DWORD)::lstrlenA(s), &w, nullptr); };
    raw("\r\n", gray);
    raw("  ====================================================\r\n", gold);
    raw("     S o W L o a d e r     Shadow of War mod loader\r\n",     gold);
    raw("     by Hagryph                                       \r\n",  gray);
    raw("  ====================================================\r\n", gold);
    raw("\r\n", gray);
    ::SetConsoleTextAttribute(out_, gray);
    return true;
}

void Console::Write(Lv lv, const std::string& line) {
    if (!open_) return;
    ::EnterCriticalSection(&cs_);
    ::SetConsoleTextAttribute(out_, AttrFor(lv));
    DWORD w = 0;
    ::WriteConsoleA(out_, line.c_str(), static_cast<DWORD>(line.size()), &w, nullptr);
    ::WriteConsoleA(out_, "\r\n", 2, &w, nullptr);
    ::SetConsoleTextAttribute(out_, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    ::LeaveCriticalSection(&cs_);
}

}  // namespace sow
