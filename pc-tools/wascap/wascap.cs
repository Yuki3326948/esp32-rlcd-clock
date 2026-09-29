/*
 * wascap.exe —— 用 WASAPI 回环把"电脑正在播放的声音"采下来,推给 ESP32 板子。
 *
 * 为什么要自己写
 *   ffmpeg 没有 wasapi 输入设备(只能录 dshow),而 dshow 的"立体声混音"
 *   只绑在 Realtek 那一颗芯片上 —— 声音走 USB DAC / HDMI 就录不到。
 *   WASAPI 回环可以对【任意】输出端点开启,而且不用改任何声音路由。
 *
 * 为什么用 C# 而不是 Python
 *   Python 那几个回环库(soundcard / pyaudiowpatch)带编译扩展,
 *   这台机器是 Python 3.14,大概率没有对应轮子。C# 直接对 WASAPI 做
 *   COM 互操作,零依赖,系统自带的 csc.exe 就能编。
 *
 * 【设计目标:尽量少占系统资源】
 *   1. 程序自己推 TCP —— 不需要 Python,不需要 ffmpeg,全程只有这一个进程
 *   2. 程序自己重采样(线性插值,仅当设备不是 48kHz 时才启用)
 *   3. 单线程:采集 -> 转换 -> 发送,全在一个循环里,没有队列没有锁
 *   4. 所有缓冲区只分配一次,循环里不再 new(不给 GC 添活)
 *   5. 事件驱动等待:没有声音时线程真正睡着,不空转烧 CPU;
 *      万一事件模式不支持,才退化成 8ms 轮询
 *
 * 用法
 *   wascap.exe --list                 列出所有输出端点(名字 + 混音格式)
 *   wascap.exe                        推【默认输出设备】的声音到板子
 *   wascap.exe --device USB           按名字子串挑一个输出设备
 *   wascap.exe --host 192.168.1.182   手动指定板子 IP
 *   wascap.exe --gain 0.5             软件衰减(超过 1.0 不放大,避免削顶)
 *
 * 编译(本目录下)
 *   csc /nologo /target:exe /platform:x64 /optimize+ /out:wascap.exe wascap.cs
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

/* ============================================================
 *  1. WASAPI 的 COM 接口声明
 * ============================================================
 *  C# 调 COM 只要有这些 [ComImport] 声明就行 —— 顺序(vtable 顺序)
 *  必须和平台 SDK 里的定义完全一致,否则调用会跑到别的函数上去。
 */

internal enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
internal enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }

/* IMMDeviceEnumerator 的 coclass,用来 new 出接口 */
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
internal class MMDeviceEnumeratorComObject { }

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMMDeviceEnumerator
{
    int EnumAudioEndpoints(EDataFlow dataFlow, uint stateMask,
                           [MarshalAs(UnmanagedType.Interface)] out IMMDeviceCollection devices);
    int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role,
                                [MarshalAs(UnmanagedType.Interface)] out IMMDevice endpoint);
    int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id,
                  [MarshalAs(UnmanagedType.Interface)] out IMMDevice device);
    int RegisterEndpointNotificationCallback(IntPtr client);
    int UnregisterEndpointNotificationCallback(IntPtr client);
}

[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMMDeviceCollection
{
    int GetCount(out uint count);
    int Item(uint index, [MarshalAs(UnmanagedType.Interface)] out IMMDevice device);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMMDevice
{
    int Activate(ref Guid iid, uint clsCtx, IntPtr activationParams,
                 [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    int OpenPropertyStore(uint access, [MarshalAs(UnmanagedType.Interface)] out IPropertyStore store);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    int GetState(out uint state);
}

[ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IPropertyStore
{
    int GetCount(out uint count);
    int GetAt(uint index, out PROPERTYKEY key);
    int GetValue(ref PROPERTYKEY key, out PROPVARIANT value);
    int SetValue(ref PROPERTYKEY key, ref PROPVARIANT value);
    int Commit();
}

[ComImport, Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAudioClient
{
    int Initialize(int shareMode, uint streamFlags, long bufferDuration,
                   long periodicity, IntPtr format, IntPtr sessionGuid);
    int GetBufferSize(out uint frames);
    int GetStreamLatency(out long latency);
    int GetCurrentPadding(out uint padding);
    int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closest);
    int GetMixFormat(out IntPtr format);            /* 用完必须 CoTaskMemFree */
    int GetDevicePeriod(out long defaultPeriod, out long minPeriod);
    int Start();
    int Stop();
    int Reset();
    int SetEventHandle(IntPtr handle);
    int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
}

[ComImport, Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAudioCaptureClient
{
    int GetBuffer(out IntPtr data, out uint frames, out uint flags,
                  out ulong devicePosition, out ulong qpcPosition);
    int ReleaseBuffer(uint frames);
    int GetNextPacketSize(out uint frames);
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal struct PROPERTYKEY { public Guid fmtid; public uint pid; }

/* 只需要读字符串,所以 PROPVARIANT 只声明到能取 LPWSTR 指针就够了 */
[StructLayout(LayoutKind.Sequential)]
internal struct PROPVARIANT
{
    public ushort vt;
    public ushort r1, r2, r3;
    public IntPtr ptr;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct WAVEFORMATEX
{
    public ushort wFormatTag;
    public ushort nChannels;
    public uint nSamplesPerSec;
    public uint nAvgBytesPerSec;
    public ushort nBlockAlign;
    public ushort wBitsPerSample;
    public ushort cbSize;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal struct WAVEFORMATEXTENSIBLE
{
    public WAVEFORMATEX Format;
    public ushort wValidBitsPerSample;
    public uint dwChannelMask;
    public Guid SubFormat;
}

internal static class Com
{
    public static readonly Guid IID_IAudioClient =
        new Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2");
    public static readonly Guid IID_IAudioCaptureClient =
        new Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317");
    public static readonly Guid SUBTYPE_PCM =
        new Guid("00000001-0000-0010-8000-00AA00389B71");
    public static readonly Guid SUBTYPE_IEEE_FLOAT =
        new Guid("00000003-0000-0010-8000-00AA00389B71");

    public const uint CLSCTX_ALL = 23;
    public const uint DEVICE_STATE_ACTIVE = 0x1;
    public const int STREAMFLAGS_LOOPBACK = 0x00020000;
    public const int STREAMFLAGS_EVENTCALLBACK = 0x00040000;
    public const int STREAMFLAGS_NOPERSIST = 0x00080000;
    public const int SHAREMODE_SHARED = 0;
    public const uint BUFFERFLAGS_SILENT = 0x2;

    public const uint VT_LPWSTR = 31;

    [DllImport("ole32.dll")]
    public static extern void CoTaskMemFree(IntPtr p);
    [DllImport("ole32.dll")]
    public static extern int CoInitializeEx(IntPtr reserved, uint flags);
    [DllImport("ole32.dll")]
    public static extern void CoUninitialize();
}

/* ============================================================
 *  2. 音频格式描述
 * ============================================================ */
internal sealed class MixInfo
{
    public int    Rate;          /* 引擎混音采样率(常见 44100 或 48000) */
    public int    Channels;
    public int    Bits;          /* 每样本位数:32 或 16 */
    public bool   IsFloat;       /* 32 位时:是 float 还是 int */
    public int    BlockAlign;

    public override string ToString()
    {
        return string.Format("{0} Hz / {1}ch / {2}bit {3}",
            Rate, Channels, Bits, IsFloat ? "float" : "pcm");
    }
}

/* ============================================================
 *  3. 输出端点枚举 / 查找
 * ============================================================ */
internal sealed class Endpoint
{
    public IMMDevice Device;
    public string    Name;
}

internal static class Devices
{
    private static readonly PROPERTYKEY PKEY_FriendlyName = new PROPERTYKEY
    {
        fmtid = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"),
        pid   = 14
    };

    public static IMMDeviceEnumerator Enumerator()
    {
        return (IMMDeviceEnumerator)(new MMDeviceEnumeratorComObject());
    }

    public static string FriendlyName(IMMDevice dev)
    {
        IPropertyStore store = null;
        try
        {
            if (dev.OpenPropertyStore(0 /*STGM_READ*/, out store) != 0) return null;
            PROPVARIANT pv;
            PROPERTYKEY key = PKEY_FriendlyName;
            if (store.GetValue(ref key, out pv) != 0) return null;
            if (pv.vt != Com.VT_LPWSTR || pv.ptr == IntPtr.Zero) return null;
            return Marshal.PtrToStringUni(pv.ptr);
        }
        catch { return null; }
        finally { if (store != null) Marshal.ReleaseComObject(store); }
    }

    /* 列出所有【活动的】播放端点。只枚举播放(eRender),不碰录音。 */
    public static List<Endpoint> List()
    {
        List<Endpoint> list = new List<Endpoint>();
        IMMDeviceEnumerator en = Enumerator();
        IMMDeviceCollection col = null;
        try
        {
            if (en.EnumAudioEndpoints(EDataFlow.eRender, Com.DEVICE_STATE_ACTIVE, out col) != 0)
                return list;
            uint n;
            if (col.GetCount(out n) != 0) return list;
            for (uint i = 0; i < n; i++)
            {
                IMMDevice d;
                if (col.Item(i, out d) != 0 || d == null) continue;
                Endpoint e = new Endpoint();
                e.Device = d;
                e.Name   = FriendlyName(d);
                if (e.Name == null) e.Name = "(无名)";
                list.Add(e);
            }
        }
        finally
        {
            if (col != null) Marshal.ReleaseComObject(col);
            Marshal.ReleaseComObject(en);
        }
        return list;
    }

    public static Endpoint Default()
    {
        IMMDeviceEnumerator en = Enumerator();
        IMMDevice d = null;
        try
        {
            if (en.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eConsole, out d) != 0)
                return null;
        }
        finally { Marshal.ReleaseComObject(en); }

        Endpoint e = new Endpoint();
        e.Device = d;
        e.Name   = FriendlyName(d);
        if (e.Name == null) e.Name = "(默认输出设备)";
        return e;
    }

    /* 按名字子串挑(不区分大小写) */
    public static Endpoint Match(string sub)
    {
        List<Endpoint> all = List();
        Endpoint found = null;
        for (int i = 0; i < all.Count; i++)
        {
            if (all[i].Name.IndexOf(sub, StringComparison.OrdinalIgnoreCase) >= 0)
            {
                found = all[i];
                break;
            }
        }
        /* 没匹配到的释放掉,免得 COM 引用泄漏 */
        for (int i = 0; i < all.Count; i++)
        {
            if (all[i] != found) Marshal.ReleaseComObject(all[i].Device);
        }
        return found;
    }
}

/* ============================================================
 *  4. 打开回环流
 * ============================================================ */
internal static class Loopback
{
    /*
     * 以【渲染】方式打开一个输出端点,但加上 LOOPBACK 标志 ——
     * 这样收到的是"引擎正要送给这个设备的样本"。
     *
     * 事件驱动优先:SetEventHandle 之后线程可以真正睡着,
     * 没有声音时一点 CPU 都不烧。个别驱动/环境下事件模式对回环不可用,
     * 那就退化成 8ms 轮询(实测这点开销可以忽略)。
     */
    public static bool Open(IMMDevice dev, out IAudioClient client,
                            out IAudioCaptureClient capture, out MixInfo mix,
                            out IntPtr fmtPtr, out bool eventMode)
    {
        client   = null;
        capture  = null;
        mix      = null;
        fmtPtr   = IntPtr.Zero;
        eventMode = false;

        object o;
        Guid iid = Com.IID_IAudioClient;   /* 静态只读字段不能按 ref 传,先拷一份 */
        if (dev.Activate(ref iid, Com.CLSCTX_ALL, IntPtr.Zero, out o) != 0)
            return false;
        client = (IAudioClient)o;

        if (client.GetMixFormat(out fmtPtr) != 0 || fmtPtr == IntPtr.Zero)
            return false;

        mix = Describe(fmtPtr);
        if (mix == null) return false;

        long defPeriod, minPeriod;
        client.GetDevicePeriod(out defPeriod, out minPeriod);

        /* 回环模式下不同驱动的缓冲时长需求不一样,挨个试到成功为止。
           先试事件驱动(省 CPU),不行再退化成轮询。 */
        long[] durations = new long[] { 0, defPeriod, 2000000L, 10000000L };
        int hr = unchecked((int)0x80070057);   /* E_INVALIDARG */

        for (int attempt = 0; attempt < 2; attempt++)
        {
            uint flags = Com.STREAMFLAGS_LOOPBACK;
            if (attempt == 0) flags |= Com.STREAMFLAGS_EVENTCALLBACK;

            for (int i = 0; i < durations.Length; i++)
            {
                hr = client.Initialize(Com.SHAREMODE_SHARED, flags,
                                       durations[i], 0, fmtPtr, IntPtr.Zero);
                if (hr == 0)
                {
                    eventMode = (attempt == 0);
                    break;
                }
            }
            if (hr == 0) break;
        }
        if (hr != 0) return false;

        if (eventMode)
        {
            /* 事件句柄必须在这个接口上设好,否则 Start 之后不会收到通知 */
            IntPtr h = CreateEvent(IntPtr.Zero, false, false, null);
            if (client.SetEventHandle(h) != 0)
            {
                CloseHandle(h);
                eventMode = false;      /* 设不上就当轮询处理 */
            }
            else
            {
                EventHandle = h;
            }
        }

        object cap;
        Guid iidCap = Com.IID_IAudioCaptureClient;
        if (client.GetService(ref iidCap, out cap) != 0) return false;
        capture = (IAudioCaptureClient)cap;

        if (client.Start() != 0) return false;
        return true;
    }

    public static IntPtr EventHandle = IntPtr.Zero;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateEvent(IntPtr attrs, bool manualReset,
                                             bool initialState, string name);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr h);

    public static void Close(IntPtr h)
    {
        if (h != IntPtr.Zero) CloseHandle(h);
    }

    /* 从 WAVEFORMATEX 指针里读出我们需要的那几项 */
    public static MixInfo Describe(IntPtr p)
    {
        if (p == IntPtr.Zero) return null;
        WAVEFORMATEX wf = (WAVEFORMATEX)Marshal.PtrToStructure(p, typeof(WAVEFORMATEX));

        MixInfo m = new MixInfo();
        m.Rate       = (int)wf.nSamplesPerSec;
        m.Channels   = wf.nChannels;
        m.Bits       = wf.wBitsPerSample;
        m.BlockAlign = wf.nBlockAlign;

        /* wFormatTag = 0xFFFE(EXTENSIBLE)时,真正的格式在 SubFormat 里 */
        if (wf.wFormatTag == 0xFFFE && wf.cbSize >= 22)
        {
            WAVEFORMATEXTENSIBLE we =
                (WAVEFORMATEXTENSIBLE)Marshal.PtrToStructure(p, typeof(WAVEFORMATEXTENSIBLE));
            m.IsFloat = (we.SubFormat == Com.SUBTYPE_IEEE_FLOAT);
        }
        else
        {
            m.IsFloat = (wf.wFormatTag == 3 /*WAVE_FORMAT_IEEE_FLOAT*/);
        }
        return m;
    }
}

/* ============================================================
 *  5. 找板子(和 stream_audio.py 保持同一套策略)
 * ============================================================ */
internal static class Board
{
    private const int PORT = 3333;

    /*
     * 注意:缓存里的 IP 【千万不要先探测】。
     * 探测 = 和板子建一次真的 TCP 连接再关掉,板子那边会老老实实走完
     * "开始推流 -> 断开" 的完整流程(静音、改采样率、等待),
     * 紧接着的真连接就撞在它还没缓过来的时候 —— 现象是推 2~3 秒就断。
     * 所以缓存直接信,连不上再清缓存重扫。
     */
    public static string Cached()
    {
        try
        {
            string p = Path.Combine(Paths.ToolsDir, ".last_board_ip");
            if (!File.Exists(p)) return null;
            string ip = File.ReadAllText(p).Trim();
            return ip.Length > 0 ? ip : null;
        }
        catch { return null; }
    }

    public static void Forget()
    {
        try
        {
            string p = Path.Combine(Paths.ToolsDir, ".last_board_ip");
            if (File.Exists(p)) File.Delete(p);
        }
        catch { }
    }

    public static void Remember(string ip)
    {
        try
        {
            File.WriteAllText(Path.Combine(Paths.ToolsDir, ".last_board_ip"), ip);
        }
        catch { }
    }

    private static string LocalIp()
    {
        try
        {
            using (Socket s = new Socket(AddressFamily.InterNetwork,
                                         SocketType.Dgram, ProtocolType.Udp))
            {
                s.Connect("8.8.8.8", 80);      /* 只是问路,不发包 */
                return ((System.Net.IPEndPoint)s.LocalEndPoint).Address.ToString();
            }
        }
        catch { return null; }
    }

    private static bool Probe(string ip, int timeoutMs)
    {
        try
        {
            TcpClient c = new TcpClient();
            IAsyncResult ar = c.BeginConnect(ip, PORT, null, null);
            if (!ar.AsyncWaitHandle.WaitOne(timeoutMs)) { c.Close(); return false; }
            c.EndConnect(ar);
            c.Close();
            return true;
        }
        catch { return false; }
    }

    public static string Scan()
    {
        string me = LocalIp();
        if (me == null) return null;
        string prefix = me.Substring(0, me.LastIndexOf('.') + 1);

        /* 捕获的局部变量不能取 ref,所以用一个单元素数组当共享槽 */
        string[] found = new string[1];
        object gate = new object();
        ParallelOptions opt = new ParallelOptions();
        opt.MaxDegreeOfParallelism = 64;
        Parallel.For(1, 255, opt, delegate(int i)
        {
            if (found[0] != null) return;
            string ip = prefix + i;
            if (ip == me) return;
            if (Probe(ip, 400))
            {
                lock (gate)
                {
                    if (found[0] == null) found[0] = ip;
                }
            }
        });
        if (found[0] != null) Remember(found[0]);
        return found[0];
    }
}

/* ============================================================
 *  6. 目录位置
 * ============================================================ */
internal static class Paths
{
    /* exe 放在 L:\ESP32\tools\wascap\ 下,工具目录就是它的上一级 */
    public static string ToolsDir
    {
        get
        {
            string dir = Path.GetDirectoryName(
                System.Reflection.Assembly.GetExecutingAssembly().Location);
            DirectoryInfo up = Directory.GetParent(dir);
            return up != null ? up.FullName : dir;
        }
    }
}

/* ============================================================
 *  7. 主程序
 * ============================================================ */
internal static class Program
{
    private const int OUT_RATE = 48000;     /* 板子固定吃 48kHz */
    private const int OUT_CH   = 2;
    /* 板子反向发来的停止命令(用户在板子上按了停止键)。
       必须和固件 net_audio.h 里的 NET_AUDIO_STOP_CMD 一致。 */
    private const string STOP_CMD = "STOP";

    private static int    g_gainQ = 256;    /* 音量,256 = 1.0 */
    private static string g_host  = null;
    private static string g_device = null;
    private static bool   g_list  = false;

    private static int Main(string[] argv)
    {
        if (!ParseArgs(argv)) return 2;

        Com.CoInitializeEx(IntPtr.Zero, 0 /*COINIT_MULTITHREADED*/);
        try
        {
            if (g_list)
            {
                Console.WriteLine("活动的输出端点(播放设备):");
                List<Endpoint> all = Devices.List();
                for (int i = 0; i < all.Count; i++)
                {
                    Endpoint e = all[i];
                    IntPtr fmt; MixInfo m = null;
                    object o;
                    Guid iid = Com.IID_IAudioClient;
                    if (e.Device.Activate(ref iid, Com.CLSCTX_ALL,
                                          IntPtr.Zero, out o) == 0)
                    {
                        IAudioClient ac = (IAudioClient)o;
                        if (ac.GetMixFormat(out fmt) == 0 && fmt != IntPtr.Zero)
                        {
                            m = Loopback.Describe(fmt);
                            Com.CoTaskMemFree(fmt);
                        }
                        Marshal.ReleaseComObject(ac);
                    }
                    Console.WriteLine("  " + e.Name + (m != null ? "   [" + m + "]" : ""));
                    Marshal.ReleaseComObject(e.Device);
                }
                Console.WriteLine();
                Console.WriteLine("用 --device <名字的一部分> 挑一个,不填就用【默认输出设备】。");
                return 0;
            }

            return Run();
        }
        catch (Exception ex)
        {
            Console.WriteLine("[X] " + ex.Message);
            return 1;
        }
        finally
        {
            Com.CoUninitialize();
        }
    }

    private static bool ParseArgs(string[] a)
    {
        for (int i = 0; i < a.Length; i++)
        {
            string s = a[i];
            if (s == "--list") { g_list = true; continue; }
            if (s == "--host" && i + 1 < a.Length) { g_host = a[++i]; continue; }
            if (s == "--device" && i + 1 < a.Length) { g_device = a[++i]; continue; }
            if (s == "--gain" && i + 1 < a.Length)
            {
                double g;
                if (!double.TryParse(a[++i],
                        System.Globalization.NumberStyles.Float,
                        System.Globalization.CultureInfo.InvariantCulture, out g) ||
                    g <= 0 || g > 4)
                {
                    Console.WriteLine("[X] --gain 要在 0~4 之间");
                    return false;
                }
                g_gainQ = (int)(g * 256.0 + 0.5);
                continue;
            }
            if (s == "-h" || s == "--help")
            {
                Console.WriteLine("用法: wascap.exe [--list] [--device 名字子串] "
                                  + "[--host 板子IP] [--gain 0.1~4]");
                return false;
            }
            Console.WriteLine("[X] 不认识的参数: " + s);
            return false;
        }
        return true;
    }

    private static int Run()
    {
        /* ---- 挑输出设备 ---- */
        Endpoint ep = (g_device != null) ? Devices.Match(g_device) : Devices.Default();
        if (ep == null)
        {
            Console.WriteLine(g_device != null
                ? "[X] 没找到名字里含 \"" + g_device + "\" 的输出设备。用 --list 看看。"
                : "[X] 拿不到默认输出设备。用 --list 看看有哪些,再用 --device 指定。");
            return 1;
        }

        IAudioClient ac; IAudioCaptureClient cc; MixInfo mix; IntPtr fmt; bool ev;
        if (!Loopback.Open(ep.Device, out ac, out cc, out mix, out fmt, out ev))
        {
            Console.WriteLine("[X] 打不开回环流。可能这个设备被独占,或格式不受支持。");
            return 1;
        }

        bool resample = (mix.Rate != OUT_RATE);

        Console.WriteLine("输出设备 : " + ep.Name);
        Console.WriteLine("混音格式 : " + mix);
        Console.WriteLine("输出格式 : " + OUT_RATE + " Hz / " + OUT_CH + "ch / 16bit"
                          + (resample ? "  (需要重采样)" : "  (直通,不重采样)")
                          + (ev ? "   [事件驱动]" : "   [8ms 轮询]"));

        /* ---- 找板子并连上 ---- */
        TcpClient sock = null;
        string host = null;
        while (sock == null)
        {
            host = g_host;
            if (host == null) host = Board.Cached();
            if (host == null)
            {
                Console.WriteLine("扫描网段找板子 ...");
                host = Board.Scan();
                if (host == null)
                {
                    Console.WriteLine("[!] 没找到板子。确认板子开机、和电脑同一个 WiFi。"
                                      + " 5 秒后重试 (Ctrl+C 退出)");
                    Thread.Sleep(5000);
                    continue;
                }
                Thread.Sleep(500);      /* 扫描时探测过板子,给它一点时间缓过来 */
            }
            if (g_host != null) Board.Remember(host);

            try
            {
                sock = new TcpClient();
                sock.NoDelay = true;                    /* 关掉 Nagle,降延迟 */
                sock.SendBufferSize = 64 * 1024;
                sock.Connect(host, 3333);
                Console.WriteLine("已连接 : " + host + ":3333");
            }
            catch (Exception ex)
            {
                Console.WriteLine("[!] 连不上 " + host + "(" + ex.Message + ")");
                sock = null;
                if (g_host == null)
                {
                    Board.Forget();
                    host = null;        /* 让下一轮重新扫 */
                }
                Thread.Sleep(1500);
            }
        }

        /* 板子重启 / WiFi 掉线时自动重连,不用重新启动本程序 */
        while (true)
        {
            try
            {
                Pump(cc, mix, ac, sock, resample, ev);
                break;                          /* 正常收尾 */
            }
            catch (Exception ex)
            {
                Console.WriteLine("\n[!] 连接中断(" + ex.Message + "),重连中 ...");
            }
            try { sock.Close(); } catch { }
            sock = Reconnect(host);
        }

        /* 收尾:停流 + 释放资源 */
        try { ac.Stop(); } catch { }
        if (Loopback.EventHandle != IntPtr.Zero) Loopback.Close(Loopback.EventHandle);
        if (fmt != IntPtr.Zero) Com.CoTaskMemFree(fmt);
        try { sock.Close(); } catch { }

        Console.WriteLine("已停止");
        return 0;
    }

    /* 断线后一直重试(直到用户 Ctrl+C) */
    private static TcpClient Reconnect(string host)
    {
        while (true)
        {
            try
            {
                TcpClient c = new TcpClient();
                c.NoDelay = true;
                c.SendBufferSize = 64 * 1024;
                c.Connect(host, 3333);
                Console.WriteLine("已重连 : " + host + ":3333");
                return c;
            }
            catch
            {
                Console.Write("\r  等板子回来 ...");
                Thread.Sleep(2000);
            }
        }
    }

    /*
     * 主循环:取一块 -> 转成 48k/2ch/16bit -> 直接发出去。
     * 单线程,没有队列:发送这一小段阻塞的时间(局域网里几十微秒)远小于
     * 一个缓冲块的时长(几毫秒),不会让采集断流。
     */
    private static void Pump(IAudioCaptureClient cc, MixInfo mix, IAudioClient ac,
                             TcpClient sock, bool resample, bool ev)
    {
        byte[] toBytes = null;      /* 转换后的输出缓冲,按需扩容后复用 */
        float[] ch0 = new float[8192];
        float[] ch1 = new float[8192];
        double phase = 0.0;
        float last0 = 0f, last1 = 0f;
        int  rateRatio = resample ? mix.Rate : OUT_RATE;

        NetworkStream ns = sock.GetStream();
        Stopwatch sw = Stopwatch.StartNew();
        long lastTick = 0, sentBytes = 0;

        while (true)
        {
            /* --- 板子有没有让我们停下? --- */
            if (StopRequested(sock, ns)) return;

            /* --- 等数据 --- */
            uint packet;
            if (cc.GetNextPacketSize(out packet) != 0) break;

            if (packet == 0)
            {
                if (ev)
                {
                    /* 事件驱动:睡到引擎敲一下门 */
                    if (WaitEvent(Loopback.EventHandle, 1000) == 0) { /* 超时,重来 */ }
                }
                else
                {
                    Thread.Sleep(8);
                }
                continue;
            }

            /* --- 把这一轮里攒下的所有包都取走 --- */
            while (packet > 0)
            {
                IntPtr data; uint frames, flags; ulong pos, qpc;
                if (cc.GetBuffer(out data, out frames, out flags, out pos, out qpc) != 0)
                    break;

                int needFrames = resample
                    ? (int)((frames * (long)OUT_RATE) / mix.Rate) + 2
                    : (int)frames;
                /* 多留 8KB:重采样跨块那一个样本可能让输出多出一帧,
                   宁可多分一次也不要越界 */
                int needBytes = needFrames * OUT_CH * 2 + 8192;
                if (toBytes == null || toBytes.Length < needBytes)
                    toBytes = new byte[Math.Max(needBytes, 16384)];

                if (ch0.Length < frames) { ch0 = new float[frames]; ch1 = new float[frames]; }

                int outBytes = ConvertBlock(mix, data, frames, flags,
                                            ch0, ch1, toBytes,
                                            resample ? OUT_RATE : mix.Rate,
                                            ref phase, ref last0, ref last1);

                cc.ReleaseBuffer(frames);
                if (cc.GetNextPacketSize(out packet) != 0) break;

                if (outBytes > 0)
                {
                    ns.Write(toBytes, 0, outBytes);
                    sentBytes += outBytes;
                }
            }

            long now = sw.ElapsedMilliseconds;
            if (now - lastTick >= 1000)
            {
                lastTick = now;
                double secs = sentBytes / (double)(OUT_RATE * OUT_CH * 2);
                Console.Write("\r  {0,6:F1} 秒", secs);
            }
        }
        Console.WriteLine();
    }

    /*
     * 板子有没有发停止命令过来?
     * 有  -> true,Pump 直接收尾返回,主循环不会去重连。
     * 没有-> false,接着推。
     * 对端断开要抛出去,让主循环走重连 —— 板子重启后得能自动接上。
     */
    private static bool StopRequested(TcpClient sock, NetworkStream ns)
    {
        /* Poll(0) 不阻塞。必须处理"对端已关闭":那种情况下 Poll 会一直
           报可读、Read 一直返回 0,不抛出去就会空转烧 CPU。 */
        if (!sock.Client.Poll(0, SelectMode.SelectRead)) return false;

        byte[] cmd = new byte[16];
        int n = ns.Read(cmd, 0, cmd.Length);
        if (n <= 0) throw new IOException("板子断开了连接");

        for (int i = 0; i < STOP_CMD.Length; i++)
        {
            if (i >= n || cmd[i] != (byte)STOP_CMD[i]) return false;
        }
        Console.WriteLine("\n[板子] 收到停止命令,停止推流");
        return true;
    }

    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr h, uint ms);

    private static uint WaitEvent(IntPtr h, uint ms)
    {
        return WaitForSingleObject(h, ms);
    }

    /*
     * 把引擎的混音数据转成 48k / 2ch / 16bit 小端。
     * 返回写进 dst 的字节数。
     * 只用线性插值重采样 —— 对"推到小喇叭听歌"这个用途足够了,
     * 而且换来的是【不需要再起一个 ffmpeg 进程】。
     */
    private static int ConvertBlock(MixInfo mix, IntPtr src, uint frames,
                                   uint flags, float[] a, float[] b, byte[] dst,
                                   int srcRate, ref double phase,
                                   ref float last0, ref float last1)
    {
        int n = (int)frames;
        bool silent = (flags & Com.BUFFERFLAGS_SILENT) != 0;

        /* ---- 1) 取样成 float(-1..1),取前两个声道 ---- */
        if (silent)
        {
            for (int i = 0; i < n; i++) { a[i] = 0f; b[i] = 0f; }
        }
        else if (mix.IsFloat && mix.Bits == 32)
        {
            unsafe
            {
                float* p = (float*)src.ToPointer();
                for (int i = 0; i < n; i++)
                {
                    int baseIdx = i * mix.Channels;
                    a[i] = p[baseIdx];
                    b[i] = (mix.Channels > 1) ? p[baseIdx + 1] : p[baseIdx];
                }
            }
        }
        else if (!mix.IsFloat && mix.Bits == 16)
        {
            unsafe
            {
                short* p = (short*)src.ToPointer();
                for (int i = 0; i < n; i++)
                {
                    int baseIdx = i * mix.Channels;
                    a[i] = p[baseIdx] / 32768f;
                    b[i] = (mix.Channels > 1) ? p[baseIdx + 1] / 32768f : a[i];
                }
            }
        }
        else
        {
            /* 32 位定点:取高 16 位 */
            unsafe
            {
                int* p = (int*)src.ToPointer();
                for (int i = 0; i < n; i++)
                {
                    int baseIdx = i * mix.Channels;
                    a[i] = (p[baseIdx] >> 16) / 32768f;
                    b[i] = (mix.Channels > 1) ? (p[baseIdx + 1] >> 16) / 32768f : a[i];
                }
            }
        }

        /* ---- 2) 重采样(同率时直接就是原序列) ---- */
        int produced = 0;
        if (srcRate == OUT_RATE)
        {
            for (int i = 0; i < n; i++)
            {
                WriteSample(dst, ref produced, a[i], b[i]);
            }
        }
        else
        {
            double step = (double)srcRate / OUT_RATE;
            for (int i = 0; i < n; i++)
            {
                /* 上一块的最后一个样本和当前样本之间做线性插值 */
                double t = phase;
                while (t < 1.0)
                {
                    WriteSample(dst,
                                ref produced,
                                (float)(last0 + (a[i] - last0) * t),
                                (float)(last1 + (b[i] - last1) * t));
                    t += step;
                }
                phase = t - 1.0;
                last0 = a[i];
                last1 = b[i];
            }
        }
        return produced;
    }

    private static void WriteSample(byte[] dst, ref int at, float l, float r)
    {
        int il = (int)(l * 32767f * g_gainQ / 256f + (l >= 0 ? 0.5f : -0.5f));
        int ir = (int)(r * 32767f * g_gainQ / 256f + (r >= 0 ? 0.5f : -0.5f));
        if (il > 32767) il = 32767; else if (il < -32768) il = -32768;
        if (ir > 32767) ir = 32767; else if (ir < -32768) ir = -32768;
        dst[at++] = (byte)(il & 0xFF);
        dst[at++] = (byte)((il >> 8) & 0xFF);
        dst[at++] = (byte)(ir & 0xFF);
        dst[at++] = (byte)((ir >> 8) & 0xFF);
    }
}
