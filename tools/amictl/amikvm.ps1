<#
amikvm: the PC's mouse and keyboard drive the A1200 (plan section 10.1.1).

The Amiga is a screen beside the PC's: push the pointer past the PC's right
edge (or left, "edge" in station.local) and it appears on the Amiga screen;
push it past the Amiga screen's edge on that side and it comes back. The
keyboard follows the pointer. Scroll Lock always returns to the PC.

  powershell -ExecutionPolicy Bypass -File tools\amictl\amikvm.ps1

Settings: tools/amictl/station.local (host, port, token), optionally
  "kvm": {"edge": "right", "scale": 1.0, "hotkey": "ScrollLock", "mode": "pointer"}
-Trace FILE writes every record sent and received to FILE.
Mode "pointer" places the Amiga pointer 1:1 (Workbench, most programs);
"raw" sends relative mouse events, which the Amiga accelerates, for games
that read mouse movement (Quake III's mouselook).
Needs amictld 1.3 (its input channel, port 6801). Windows PowerShell 5.1,
nothing to install. Ctrl-C ends it. MIT licence (tools/amictl/LICENSE).
#>
param([string]$Edge = "", [double]$Scale = 0, [string]$Hotkey = "", [string]$Mode = "", [string]$Trace = "")

$ErrorActionPreference = "Stop"
$cfgPath = Join-Path $PSScriptRoot "station.local"
if (-not (Test-Path $cfgPath)) { throw "no $cfgPath (see station.local.example)" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$port = 6801
if ($cfg.PSObject.Properties["input_port"]) { $port = [int]$cfg.input_port }
elseif ($cfg.PSObject.Properties["port"]) { $port = [int]$cfg.port + 1 }
$kvm = $null
if ($cfg.PSObject.Properties["kvm"]) { $kvm = $cfg.kvm }
if (-not $Edge) { $Edge = "right"; if ($kvm -and $kvm.edge) { $Edge = [string]$kvm.edge } }
if ($Scale -le 0) { $Scale = 1.0; if ($kvm -and $kvm.scale) { $Scale = [double]$kvm.scale } }
if (-not $Mode) { $Mode = "pointer"; if ($kvm -and $kvm.mode) { $Mode = [string]$kvm.mode } }
if (-not $Hotkey) { $Hotkey = "ScrollLock"; if ($kvm -and $kvm.hotkey) { $Hotkey = [string]$kvm.hotkey } }
Add-Type -AssemblyName System.Windows.Forms
$vk = [int]([System.Enum]::Parse([System.Windows.Forms.Keys], ($Hotkey -replace "ScrollLock", "Scroll"), $true))

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;

public static class AmiKvm
{
    // ---- Win32 ----
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] struct MSLL { public POINT pt; public uint mouseData, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct KBLL { public uint vk, scan, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public POINT pt; }
    [StructLayout(LayoutKind.Sequential)] struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }
    delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SetWindowsHookEx(int id, HookProc fn, IntPtr mod, uint thread);
    [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr h, int code, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern int GetMessage(out MSG m, IntPtr hwnd, uint min, uint max);
    [DllImport("user32.dll")] static extern bool PostThreadMessage(uint thread, uint msg, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string name);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT p, uint flags);
    [DllImport("user32.dll")] static extern bool GetMonitorInfo(IntPtr mon, ref MONITORINFO mi);
    [DllImport("user32.dll")] static extern IntPtr CreateCursor(IntPtr inst, int x, int y, int w, int h, byte[] and, byte[] xor);
    [DllImport("user32.dll")] static extern bool SetSystemCursor(IntPtr cur, uint id);
    [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint action, uint p, IntPtr pv, uint win);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vk);

    const int WH_KEYBOARD_LL = 13, WH_MOUSE_LL = 14;
    const uint WM_QUIT = 0x12, WM_APP_LEAVE = 0x8001, WM_APP_RETURN = 0x8002, WM_APP_HIDE = 0x8003;
    const int WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_RBUTTONDOWN = 0x204,
              WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MOUSEWHEEL = 0x20A;
    const int WM_KEYDOWN = 0x100, WM_SYSKEYDOWN = 0x104;

    // ---- state ----
    static string host, token; static int port; static bool rightEdge; static double scale; static int hotkey;
    static TcpClient tcp; static NetworkStream net; static readonly object sendLock = new object();
    static volatile bool captured, connected;
    static long lastReply;
    static uint mainThread;
    static POINT park, prev; static bool prevValid; static RECT entryMon;
    static POINT retPos; static long returnUntil; // moves Windows computed from the parking point, after a return
    static double accX, accY;
    static readonly bool[] keyDown = new bool[512];
    static HookProc mouseProc = MouseHook, keyProc = KeyHook; // kept alive
    static IntPtr mouseHook, keyHook;
    static readonly Queue<byte[]> queue = new Queue<byte[]>();
    static int screenW, screenH, moveFlags;
    static volatile int posY16 = 32767;

    // PC set-1 scan code (| 0x100 when extended) -> Amiga raw key
    static readonly Dictionary<int, int> keymap = new Dictionary<int, int>();
    static void Map(int scan, int raw) { keymap[scan] = raw; }
    static void BuildKeymap()
    {
        Map(0x01, 0x45);                                            // Esc
        for (int i = 0; i < 10; i++) Map(0x02 + i, 0x01 + i);       // 1..0
        Map(0x0C, 0x0B); Map(0x0D, 0x0C); Map(0x0E, 0x41); Map(0x0F, 0x42);
        for (int i = 0; i < 12; i++) Map(0x10 + i, 0x10 + i);       // q..]
        Map(0x1C, 0x44); Map(0x1D, 0x63);                           // Return, LCtrl
        for (int i = 0; i < 11; i++) Map(0x1E + i, 0x20 + i);       // a..'
        Map(0x29, 0x00); Map(0x2A, 0x60); Map(0x2B, 0x0D);          // ` LShift backslash
        for (int i = 0; i < 10; i++) Map(0x2C + i, 0x31 + i);       // z../
        Map(0x36, 0x61); Map(0x37, 0x5D); Map(0x38, 0x64); Map(0x39, 0x40); Map(0x3A, 0x62);
        for (int i = 0; i < 10; i++) Map(0x3B + i, 0x50 + i);       // F1..F10
        Map(0x145, 0x5A);                                           // NumLock -> keypad (
        Map(0x47, 0x3D); Map(0x48, 0x3E); Map(0x49, 0x3F); Map(0x4A, 0x4A);
        Map(0x4B, 0x2D); Map(0x4C, 0x2E); Map(0x4D, 0x2F); Map(0x4E, 0x5E);
        Map(0x4F, 0x1D); Map(0x50, 0x1E); Map(0x51, 0x1F); Map(0x52, 0x0F); Map(0x53, 0x3C);
        Map(0x56, 0x30);                                            // ISO <>
        Map(0x57, 0x4B); Map(0x58, 0x6F);                           // F11, F12
        Map(0x11C, 0x43); Map(0x11D, 0x63); Map(0x135, 0x5C); Map(0x138, 0x65); // KP Enter, RCtrl, KP /, RAlt
        Map(0x148, 0x4C); Map(0x150, 0x4D); Map(0x14B, 0x4F); Map(0x14D, 0x4E); // cursor keys
        Map(0x153, 0x46); Map(0x152, 0x47); Map(0x149, 0x48); Map(0x151, 0x49); // Del, Ins, PgUp, PgDn
        Map(0x147, 0x70); Map(0x14F, 0x71);                         // Home, End
        Map(0x15B, 0x66); Map(0x15C, 0x67); Map(0x15D, 0x5F);       // Windows keys -> Amiga, Menu -> Help
    }

    static long Now() { return Environment.TickCount; }

    static void Status(string s) { Console.WriteLine(DateTime.Now.ToString("HH:mm:ss") + "  " + s); }

    // ---- network ----
    static byte[] Rec(int type, int a, int b, int c, int flags)
    {
        return new byte[] { (byte)type, (byte)flags, (byte)(a >> 8), (byte)a, (byte)(b >> 8), (byte)b, (byte)(c >> 8), (byte)c };
    }



    static void FlushMove()   // under lock(queue)
    {
        int dx = (int)accX, dy = (int)accY;
        if (dx == 0 && dy == 0) return;
        accX -= dx; accY -= dy;
        dx = Math.Max(-32000, Math.Min(32000, dx)); dy = Math.Max(-32000, Math.Min(32000, dy));
        queue.Enqueue(Rec(1, dx, dy, 0, moveFlags));
    }

    static void Send(int type, int a, int b, int c, int flags)
    {
        lock (queue) { FlushMove(); queue.Enqueue(Rec(type, a, b, c, flags)); Monitor.Pulse(queue); }
    }

    static bool Connect()
    {
        try {
            Disconnect();
            var t = new TcpClient();
            var ar = t.BeginConnect(host, port, null, null);
            if (!ar.AsyncWaitHandle.WaitOne(1500)) { t.Close(); return false; }
            t.EndConnect(ar);
            t.NoDelay = true;
            var s = t.GetStream();
            s.ReadTimeout = 3000;
            byte[] tok = System.Text.Encoding.ASCII.GetBytes(token);
            byte[] h = new byte[8 + tok.Length];
            h[0] = (byte)'A'; h[1] = (byte)'M'; h[2] = (byte)'I'; h[3] = (byte)'1';
            h[7] = (byte)tok.Length;
            Array.Copy(tok, 0, h, 8, tok.Length);
            s.Write(h, 0, h.Length);
            byte[] r = ReadN(s, 8);
            if (r[0] != 'A' || r[4] != 0 || r[5] != 0 || r[6] != 0 || r[7] != 0) { t.Close(); Status("the station refused the token"); return false; }
            r = ReadN(s, 4);
            screenW = (r[0] << 8) | r[1]; screenH = (r[2] << 8) | r[3];
            s.ReadTimeout = Timeout.Infinite;
            tcp = t; net = s; connected = true;
            lock (queue) queue.Clear();
            new Thread(Receiver) { IsBackground = true }.Start();
            Status("connected to " + host + ":" + port + ", Amiga screen " + screenW + "x" + screenH);
            return true;
        } catch (Exception e) {
            Status("cannot reach the station: " + e.Message);
            return false;
        }
    }

    static void Reconnector()
    {
        for (;;) {
            if (!connected && !captured) Connect();
            Thread.Sleep(3000);
        }
    }

    static void Disconnect()
    {
        connected = false;
        if (tcp != null) { try { tcp.Close(); } catch { } tcp = null; net = null; }
    }

    static byte[] ReadN(NetworkStream s, int n)
    {
        byte[] b = new byte[n];
        int got = 0;
        while (got < n) {
            int k = s.Read(b, got, n - got);
            if (k <= 0) throw new Exception("connection closed");
            got += k;
        }
        return b;
    }

    static void Receiver()
    {
        NetworkStream s = net;
        try {
            for (;;) {
                byte[] r = ReadN(s, 8);
                if (trace != null) Trace("recv", r);
                Interlocked.Exchange(ref lastReply, Now());
                if (r[0] == 0x81 && screenH > 1)
                    posY16 = Math.Max(0, Math.Min(65535, (short)((r[4] << 8) | r[5]) * 65535 / (screenH - 1)));
                if (r[0] == 0x82)
                    PostThreadMessage(mainThread, WM_APP_LEAVE, (IntPtr)((r[2] << 8) | r[3]), IntPtr.Zero);
            }
        } catch {
            if (s == net) {
                connected = false;
                PostThreadMessage(mainThread, WM_APP_RETURN, (IntPtr)1, IntPtr.Zero);
            }
        }
    }

    static void Sender()
    {
        long lastPing = 0;
        for (;;) {
            List<byte[]> batch = new List<byte[]>();
            lock (queue) {
                if (queue.Count == 0) Monitor.Wait(queue, 4);
                FlushMove();
                while (queue.Count > 0) batch.Add(queue.Dequeue());
            }
            if (captured && Now() - lastPing > 400) { batch.Add(Rec(8, 0, 0, 0, 0)); lastPing = Now(); }
            if (captured && Now() - Interlocked.Read(ref lastReply) > 3000) {
                Status("the Amiga stopped answering");
                PostThreadMessage(mainThread, WM_APP_RETURN, (IntPtr)1, IntPtr.Zero);
                Interlocked.Exchange(ref lastReply, Now());
            }
            NetworkStream s = net;
            if (batch.Count == 0 || s == null || !connected) continue;
            try {
                byte[] all = new byte[batch.Count * 8];
                for (int i = 0; i < batch.Count; i++) Array.Copy(batch[i], 0, all, i * 8, 8);
                if (trace != null) foreach (byte[] r in batch) Trace("send", r);
                s.Write(all, 0, all.Length);
            } catch { }
        }
    }

    // ---- capture ----
    static IntPtr blankAnd = IntPtr.Zero;
    static readonly uint[] cursorIds = { 32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32650 };

    static void HideCursor()
    {
        byte[] and = new byte[128], xor = new byte[128];
        for (int i = 0; i < and.Length; i++) and[i] = 0xFF;
        foreach (uint id in cursorIds) {
            IntPtr c = CreateCursor(IntPtr.Zero, 0, 0, 32, 32, and, xor);
            if (c != IntPtr.Zero) SetSystemCursor(c, id);   // the system takes the cursor
        }
    }

    static void ShowCursor() { SystemParametersInfo(0x57, 0, IntPtr.Zero, 0); } // SPI_SETCURSORS

    static RECT MonitorOf(POINT p)
    {
        MONITORINFO mi = new MONITORINFO();
        mi.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
        GetMonitorInfo(MonitorFromPoint(p, 2), ref mi);
        return mi.rcMonitor;
    }

    static void Enter(POINT p)
    {
        if (!connected) return; // the reconnecting thread is at it
        entryMon = MonitorOf(p);
        int h = entryMon.bottom - entryMon.top;
        int y = Math.Max(0, Math.Min(h - 1, p.y - entryMon.top));
        int y16 = h > 1 ? (int)((long)y * 65535 / (h - 1)) : 0;
        park.x = (entryMon.left + entryMon.right) / 2;
        park.y = (entryMon.top + entryMon.bottom) / 2;
        accX = accY = 0;
        Interlocked.Exchange(ref lastReply, Now());
        captured = true;
        SetCursorPos(park.x, park.y);
        PostThreadMessage(mainThread, WM_APP_HIDE, IntPtr.Zero, IntPtr.Zero);
        Send(6, y16, 0, rightEdge ? 0 : 1, 0); // entering from the PC's right: the Amiga's left edge
        Status("on the Amiga  (" + (rightEdge ? "left" : "right") + " edge back, Scroll Lock: back at once)");
    }

    static void Return(int y16, bool fromAmiga)
    {
        if (!captured) return;
        captured = false;
        ShowCursor();
        Array.Clear(keyDown, 0, keyDown.Length);
        if (connected) Send(5, 0, 0, 0, 0);
        int h = entryMon.bottom - entryMon.top;
        int y = entryMon.top + (int)((long)y16 * Math.Max(0, h - 1) / 65535);
        int x = rightEdge ? entryMon.right - 4 : entryMon.left + 3;
        SetCursorPos(x, y);
        retPos.x = x; retPos.y = y; returnUntil = Now() + 300;
        prevValid = false;
        Status(connected ? "on the PC" : "on the PC (disconnected)");
    }

    // ---- hooks ----
    static IntPtr MouseHook(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code < 0) return CallNextHookEx(mouseHook, code, wParam, lParam);
        MSLL m = (MSLL)Marshal.PtrToStructure(lParam, typeof(MSLL));
        if ((m.flags & 1) != 0) return CallNextHookEx(mouseHook, code, wParam, lParam); // injected
        int msg = (int)wParam;
        if (!captured) {
            if (msg == WM_MOUSEMOVE) {
                if (Now() < returnUntil && Math.Abs(m.pt.x - park.x) < 300 && Math.Abs(m.pt.y - park.y) < 300) {
                    retPos.x += m.pt.x - park.x; retPos.y += m.pt.y - park.y;
                    retPos.x = rightEdge ? Math.Min(retPos.x, entryMon.right - 4) : Math.Max(retPos.x, entryMon.left + 3);
                    retPos.y = Math.Max(entryMon.top, Math.Min(entryMon.bottom - 1, retPos.y));
                    SetCursorPos(retPos.x, retPos.y);
                    return (IntPtr)1;
                }
                int vl = GetSystemMetrics(76), vr = vl + GetSystemMetrics(78);
                bool beyond = rightEdge ? m.pt.x >= vr : m.pt.x < vl;
                int edgeX = rightEdge ? vr - 1 : vl;
                bool pushing = prevValid && m.pt.x == edgeX && prev.x == edgeX && Math.Abs(m.pt.y - prev.y) <= 1;
                prev = m.pt; prevValid = true;
                // Not with a PC button down: Windows would never see it let go.
                bool held = (GetAsyncKeyState(1) & 0x8000) != 0 || (GetAsyncKeyState(2) & 0x8000) != 0 || (GetAsyncKeyState(4) & 0x8000) != 0;
                if ((beyond || pushing) && !held) { Enter(m.pt); if (captured) return (IntPtr)1; }
            }
            return CallNextHookEx(mouseHook, code, wParam, lParam);
        }
        switch (msg) {
        case WM_MOUSEMOVE:
            lock (queue) {
                accX += (m.pt.x - park.x) * scale;
                accY += (m.pt.y - park.y) * scale;
                Monitor.Pulse(queue);
            }
            break;
        case WM_LBUTTONDOWN: Send(2, 0, 1, 0, 0); break;
        case WM_LBUTTONUP: Send(2, 0, 0, 0, 0); break;
        case WM_RBUTTONDOWN: Send(2, 1, 1, 0, 0); break;
        case WM_RBUTTONUP: Send(2, 1, 0, 0, 0); break;
        case WM_MBUTTONDOWN: Send(2, 2, 1, 0, 0); break;
        case WM_MBUTTONUP: Send(2, 2, 0, 0, 0); break;
        case WM_MOUSEWHEEL: {
            int d = (short)(m.mouseData >> 16) / 120;
            if (d != 0) Send(3, d, 0, 0, 0);
            break;
        }
        }
        return (IntPtr)1;
    }

    static IntPtr KeyHook(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code < 0) return CallNextHookEx(keyHook, code, wParam, lParam);
        KBLL k = (KBLL)Marshal.PtrToStructure(lParam, typeof(KBLL));
        if ((k.flags & 0x10) != 0) return CallNextHookEx(keyHook, code, wParam, lParam); // injected
        bool down = (int)wParam == WM_KEYDOWN || (int)wParam == WM_SYSKEYDOWN;
        if (k.vk == hotkey) {
            if (!captured) return CallNextHookEx(keyHook, code, wParam, lParam);
            if (down) PostThreadMessage(mainThread, WM_APP_RETURN, IntPtr.Zero, IntPtr.Zero);
            return (IntPtr)1;
        }
        if (!captured) return CallNextHookEx(keyHook, code, wParam, lParam);
        int scan = (int)(k.scan & 0xFF) | ((k.flags & 1) != 0 ? 0x100 : 0);
        if (k.vk == 0xA1) scan = 0x36; // right Shift
        int raw;
        if (keymap.TryGetValue(scan, out raw)) {
            bool repeat = down && keyDown[scan];
            keyDown[scan] = down;
            Send(4, raw, down ? 1 : 0, 0, repeat ? 1 : 0);
        }
        return (IntPtr)1;
    }

    // ---- main ----
    static System.IO.StreamWriter trace;
    static readonly object traceLock = new object();
    static void Trace(string dir, byte[] r)
    {
        lock (traceLock) trace.WriteLine("{0:HH:mm:ss.fff} {1} type {2,3} flags {3} a {4,6} b {5,6} c {6,6}", DateTime.Now, dir, r[0], r[1],
            (short)((r[2] << 8) | r[3]), (short)((r[4] << 8) | r[5]), (short)((r[6] << 8) | r[7]));
    }

    public static void Run(string h, int p, string t, bool right, double s, int hk, bool raw, string tracePath)
    {
        host = h; port = p; token = t; rightEdge = right; scale = s; hotkey = hk; moveFlags = raw ? 0 : 2;
        if (tracePath != "") { trace = new System.IO.StreamWriter(tracePath, false); trace.AutoFlush = true; }
        try { SetProcessDPIAware(); } catch { }
        BuildKeymap();
        mainThread = GetCurrentThreadId();
        Console.CancelKeyPress += delegate(object o, ConsoleCancelEventArgs e) {
            e.Cancel = true;
            PostThreadMessage(mainThread, WM_QUIT, IntPtr.Zero, IntPtr.Zero);
        };
        new Thread(Reconnector) { IsBackground = true }.Start();
        new Thread(Sender) { IsBackground = true }.Start();
        IntPtr mod = GetModuleHandle(null);
        mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseProc, mod, 0);
        keyHook = SetWindowsHookEx(WH_KEYBOARD_LL, keyProc, mod, 0);
        if (mouseHook == IntPtr.Zero || keyHook == IntPtr.Zero) throw new Exception("cannot install the input hooks");
        Status("on the PC: push the pointer past the " + (right ? "right" : "left") + " edge for the Amiga; Ctrl-C ends");
        try {
            MSG m;
            while (GetMessage(out m, IntPtr.Zero, 0, 0) > 0) {
                if (m.message == WM_APP_LEAVE) Return((int)m.wParam, true);
                else if (m.message == WM_APP_HIDE) { if (captured) HideCursor(); }
                else if (m.message == WM_APP_RETURN) {
                    int y16 = posY16;
                    Return(y16, false);
                    if (m.wParam == (IntPtr)1 && connected) Disconnect();
                }
            }
        } finally {
            if (captured) { captured = false; ShowCursor(); }
            if (connected) { Send(5, 0, 0, 0, 0); Thread.Sleep(100); }
            UnhookWindowsHookEx(mouseHook);
            UnhookWindowsHookEx(keyHook);
            Disconnect();
            Status("stopped");
        }
    }
}
'@

[AmiKvm]::Run([string]$cfg.host, $port, [string]$cfg.token, ($Edge -ne "left"), $Scale, $vk, ($Mode -eq "raw"), $Trace)
