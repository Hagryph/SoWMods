using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace HagOverlay;

// A layered, always-on-top window that the OS compositor paints OVER the game's window — its own
// (Chromium/WebView2) renderer, independent of the game's D3D/Scaleform. It is made an OWNED window
// of the game HWND and tracked to the game's client rect, so it is visually "attached" and moves
// with the game. Full-modal: when shown it captures all input (blocks click-through). Toggle: F8.
public partial class MainWindow : Window
{
    // ---- Win32 ----
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern bool RegisterHotKey(IntPtr h, int id, uint mod, uint vk);
    [DllImport("user32.dll")] static extern IntPtr SetWindowLongPtr(IntPtr h, int idx, IntPtr val);
    [DllImport("user32.dll")] static extern bool ClipCursor(IntPtr r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    delegate bool EnumProc(IntPtr h, IntPtr l);

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }

    const int GWLP_HWNDPARENT = -8;
    const uint SWP_NOACTIVATE = 0x10;
    static readonly IntPtr HWND_TOPMOST = new(-1);
    const int HOTKEY_ID = 0xB001;
    const uint VK_F8 = 0x77, MOD_NOREPEAT = 0x4000;
    const int WM_HOTKEY = 0x0312;

    IntPtr _game = IntPtr.Zero, _self = IntPtr.Zero;
    bool _open = false, _ready = false;
    readonly DispatcherTimer _track = new() { Interval = TimeSpan.FromMilliseconds(120) };

    public MainWindow()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    async void OnLoaded(object? sender, RoutedEventArgs e)
    {
        _self = new WindowInteropHelper(this).Handle;
        HwndSource.FromHwnd(_self)!.AddHook(WndProc);
        RegisterHotKey(_self, HOTKEY_ID, MOD_NOREPEAT, VK_F8);

        await Web.EnsureCoreWebView2Async();
        Web.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
        Web.CoreWebView2.Settings.IsStatusBarEnabled = false;
        Web.CoreWebView2.Settings.AreDevToolsEnabled = false;
        Web.CoreWebView2.WebMessageReceived += (o, ev) =>
        {
            if (ev.TryGetWebMessageAsString() == "close") Close2();
        };
        var html = System.IO.Path.Combine(AppContext.BaseDirectory, "web", "index.html");
        Web.CoreWebView2.Navigate(new Uri(html).AbsoluteUri);

        _ready = true;
        _track.Tick += (o, ev) => Track();
        _track.Start();
        HideWindowOffscreen();   // start hidden
    }

    void FindGame()
    {
        if (_game != IntPtr.Zero && IsWindow(_game)) return;
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) =>
        {
            var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
            if (sb.ToString().IndexOf("Middle-earth", StringComparison.OrdinalIgnoreCase) >= 0)
            { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        _game = found;
        if (_game != IntPtr.Zero) SetWindowLongPtr(_self, GWLP_HWNDPARENT, _game);  // own -> follows game z/min
    }

    void Track()
    {
        if (!_open) return;
        FindGame();
        if (_game == IntPtr.Zero || !IsWindow(_game) || IsIconic(_game)) { Close2(); return; }
        GetClientRect(_game, out var rc);
        POINT tl = new() { X = 0, Y = 0 }; ClientToScreen(_game, ref tl);
        SetWindowPos(_self, HWND_TOPMOST, tl.X, tl.Y, rc.R - rc.L, rc.B - rc.T, SWP_NOACTIVATE);
    }

    void Open2()
    {
        if (!_ready) return;
        FindGame();
        if (_game == IntPtr.Zero) return;   // no game -> nothing to overlay
        _open = true;
        WindowState = WindowState.Normal;
        Visibility = Visibility.Visible;
        Track();
        ClipCursor(IntPtr.Zero);            // release any cursor clip the game holds
        SetForegroundWindow(_self);          // full modal: capture all input
        Activate();
        Web.Focus();
        Web.CoreWebView2?.PostWebMessageAsString("opened");
    }

    void Close2()
    {
        _open = false;
        HideWindowOffscreen();
        if (_game != IntPtr.Zero && IsWindow(_game)) SetForegroundWindow(_game);
    }

    // Hide without WPF Collapsed layout thrash: move offscreen + hidden visibility.
    void HideWindowOffscreen()
    {
        Visibility = Visibility.Hidden;
        SetWindowPos(_self, HWND_TOPMOST, -32000, -32000, 8, 8, SWP_NOACTIVATE);
    }

    IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WM_HOTKEY && wParam.ToInt32() == HOTKEY_ID)
        {
            if (_open) Close2(); else Open2();
            handled = true;
        }
        return IntPtr.Zero;
    }
}
