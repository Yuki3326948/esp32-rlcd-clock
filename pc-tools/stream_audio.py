"""把电脑上的音乐实时推给 ESP32(走 WiFi,不是蓝牙)。

为什么这样设计
    ESP32-S3 没有经典蓝牙,A2DP(蓝牙推音频)用不了。改走 WiFi 之后有个
    很大的好处:**解码可以放在电脑端**。这里用 ffmpeg 把源文件解成
    48kHz 立体声 16bit 裸 PCM,一路 TCP 发过去,板子只管往 I2S 里塞。

    所以:
      * 板子上【不需要任何解码器】,MP3 / FLAC / M4A / OGG / WAV /
        甚至视频文件里的音轨,统统能放 —— 格式支持面完全跟着 ffmpeg 走
      * 音质是实打实的数据,不像蓝牙那样先压缩再传

用法
    python stream_audio.py "D:\\Music\\我的歌单"     # 推整个文件夹
    python stream_audio.py "D:\\Music\\歌.mp3"       # 推单个文件
    python stream_audio.py                           # 只找板子,看它在不在
    python stream_audio.py "a.mp3" --loop            # 循环
    python stream_audio.py "a.mp3" 192.168.1.182     # 手动指定 IP

    不带 IP 时,脚本会自己在本机网段里扫 3333 端口找板子(结果会缓存,
    下次秒连)。板子的 IP 是 DHCP 分的,会变,所以一般不用手填。

    让板子当“WiFi 音箱”(网易云 / 浏览器 / 任何能出声的软件):

      python stream_audio.py --list-devices    # 先看看有哪些录音设备
      python stream_audio.py --capture         # 自动挑回环设备,开始推
      python stream_audio.py --capture "立体声混音 (Realtek High Definition Audio)"

    --capture 走的是系统的“立体声混音”回环:电脑放什么,板子就放什么。
    所以网易云照常用官方客户端放就行,不需要任何第三方接口。
    ⚠️ 前提是你的默认播放设备就是那块声卡;声音若走 HDMI / 显卡输出,
       立体声混音录不到,得另找 WASAPI 回环方案。

按 Ctrl+C 停止,板子会自动回到 TF 卡播放。
"""
import argparse
import concurrent.futures
import os
import socket
import subprocess
import sys
import time

SAMPLE_RATE = 48000
CHANNELS = 2
# 每次从 ffmpeg 读多少字节再发。约 0.17 秒的音频:
# 太小会导致发送过于频繁,太大则开始播放前要等更久
CHUNK = 16384

# 文件夹模式下认这些扩展名(视频的音频轨也算)
AUDIO_EXT = {".mp3", ".flac", ".m4a", ".aac", ".ogg", ".opus", ".wav",
             ".ape", ".wma", ".aif", ".aiff", ".mp4", ".mkv", ".mov",
             ".avi", ".webm", ".ts", ".flv"}

# 系统回环录音设备的常见名字:中文系统叫“立体声混音”,
# 英文系统叫 Stereo Mix,有些驱动叫 Wave Out Mix / What U Hear
LOOPBACK_HINTS = ("立体声混音", "stereo mix", "wave out mix", "what u hear",
                  "loopback")


# ---------------------------------------------------------------- 找板子 ----

def local_ip():
    """拿本机走默认路由的那个 IP(只是问路,不会真的发包)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def probe(ip, port, timeout=0.35):
    """端口通就返回这个 IP,否则返回 None"""
    try:
        c = socket.create_connection((ip, port), timeout=timeout)
        c.close()
        return ip
    except OSError:
        return None


# 记住上次找到的 IP。板子的 IP 是 DHCP 分的,但通常不会变,
# 缓存一下就能“秒连”,不用每次扫一遍
IP_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        ".last_board_ip")


def read_cache():
    try:
        with open(IP_CACHE, "r", encoding="utf-8") as f:
            return f.read().strip() or None
    except OSError:
        return None


def save_ip(ip):
    try:
        with open(IP_CACHE, "w", encoding="utf-8") as f:
            f.write(ip)
    except OSError:
        pass


def clear_cache():
    try:
        os.remove(IP_CACHE)
    except OSError:
        pass


def scan_once(port, timeout, workers=32):
    """扫一遍本机所在的 /24 网段"""
    me = local_ip()
    if not me:
        return None
    prefix = me.rsplit(".", 1)[0]
    targets = [f"{prefix}.{i}" for i in range(1, 255) if f"{prefix}.{i}" != me]

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        futures = {ex.submit(probe, ip, port, timeout): ip for ip in targets}
        for f in concurrent.futures.as_completed(futures):
            if f.result():
                return futures[f]
    return None


def find_board(port, tries=3):
    """优先用上次缓存的 IP,没有才扫网段。

    【这里千万别探测缓存里的 IP!】探测 = 和板子建一次真的 TCP 连接
    再关掉,板子那边会认认真真走一遍完整的"开始推流 -> 断开"流程
    (静音、改采样率、200ms 等待),紧接着的真连接就撞在它还没缓过来
    的时候 —— 现象是推 2~3 秒就断。
    所以缓存直接信,真连不上再由调用方清缓存重扫。
    """
    ip = read_cache()
    if ip:
        print(f"用上次的 IP: {ip}")
        return ip

    me = local_ip()
    if not me:
        print("[!] 拿不到本机 IP —— 电脑没连网?")
        return None

    for n in range(1, tries + 1):
        print(f"第 {n} 次扫描 {me.rsplit('.', 1)[0]}.1~254 ...", flush=True)
        ip = scan_once(port, timeout=0.4 + 0.3 * (n - 1))
        if ip:
            save_ip(ip)
            time.sleep(0.5)   # 扫描时探测过板子,给它半秒缓过来再正式连
            return ip
    return None


def list_dshow_audio():
    """让 ffmpeg 列一遍 DirectShow 设备,返回其中的音频设备名。
    ffmpeg 把设备清单打在 stderr 上,而且最后一定以“打不开 dummy”报错退出,
    所以这里不要检查返回码,只看输出。"""
    p = subprocess.run(
        ["ffmpeg", "-hide_banner", "-list_devices", "true",
         "-f", "dshow", "-i", "dummy"],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    names = []
    for line in ((p.stderr or "") + (p.stdout or "")).splitlines():
        line = line.strip()
        if "(audio)" in line and '"' in line:
            names.append(line.split('"')[1])
    return names


def pick_loopback():
    """从设备列表里挑一个最像“系统回环”的"""
    for n in list_dshow_audio():
        low = n.lower()
        if any(h in n or h in low for h in LOOPBACK_HINTS):
            return n
    return None


def collect(src):
    """传文件就返回它;传文件夹就返回里面所有音频(按名字排序)"""
    if os.path.isdir(src):
        names = [n for n in os.listdir(src)
                 if os.path.splitext(n)[1].lower() in AUDIO_EXT]
        return [os.path.join(src, n) for n in sorted(names)]
    return [src]


def stream_once(src, sock, capture=False):
    """把 src 推完为止。返回 False 表示板子断开连接了。

    capture=True 时 src 是 DirectShow 录音设备名,走实时采集:
      * 设备给的是 44.1k 还是 48k 都无所谓,交给 ffmpeg 重采样
      * -audio_buffer_size 把 dshow 的缓冲压到 50ms,降低延迟
      * -rtbufsize 给采集侧留足缓冲,电脑忙一下才不会丢成爆音
    """
    cmd = [
        "ffmpeg",
        "-hide_banner", "-loglevel", "error",
    ]
    if capture:
        cmd += ["-f", "dshow", "-rtbufsize", "128M",
                "-audio_buffer_size", "50", "-i", f"audio={src}"]
    else:
        cmd += ["-i", src]
    cmd += [
        "-f", "s16le",            # 裸 PCM,16bit 小端
        "-acodec", "pcm_s16le",
        "-ar", str(SAMPLE_RATE),  # 重采样到板子固定的 48kHz
        "-ac", str(CHANNELS),     # 强制立体声(单声道也会被 ffmpeg 复制成双声道)
        "-",                      # 输出到 stdout
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)

    sent = 0
    try:
        while True:
            data = proc.stdout.read(CHUNK)
            if not data:
                break
            try:
                sock.sendall(data)
            except (BrokenPipeError, ConnectionResetError, OSError):
                print("\n[!] 板子断开了连接")
                return False
            sent += len(data)

            # 进度:按 48k/2ch/16bit 折算成秒
            secs = sent / (SAMPLE_RATE * CHANNELS * 2)
            print(f"\r   {secs:6.1f} 秒", end="", flush=True)
    finally:
        proc.stdout.close()
        proc.wait()

    print()
    return True


def main():
    ap = argparse.ArgumentParser(description="把电脑上的音乐推给 ESP32(WiFi)")
    ap.add_argument("file", nargs="?",
                    help="音频文件(MP3 / FLAC / M4A / WAV / 视频都行)")
    ap.add_argument("host", nargs="?",
                    help="板子 IP,不填就在网段里自动找")
    ap.add_argument("--port", type=int, default=3333, help="默认 3333")
    ap.add_argument("--loop", action="store_true", help="放完自动重来")
    ap.add_argument("--shuffle", action="store_true", help="文件夹模式打乱顺序")
    ap.add_argument("--delay", type=float, default=0,
                    help="连接前先等几秒(板子刚上电时用)")
    ap.add_argument("--list-devices", action="store_true",
                    help="列出可用的录音设备后退出")
    ap.add_argument("--capture", nargs="?", const="auto", default=None,
                    metavar="设备名",
                    help="采集系统正在播放的声音推给板子(不填设备名就自动挑"
                         "“立体声混音”这类回环设备)")
    args = ap.parse_args()

    if args.list_devices:
        print("可用的录音设备(音频输入):")
        for n in list_dshow_audio():
            print(f"  {n}")
        print("\n其中带“立体声混音 / Stereo Mix”字样的就是系统回环设备。")
        return 0

    if args.delay > 0:
        print(f"等待 {args.delay:.0f} 秒(等板子启动) ...")
        time.sleep(args.delay)

    # ---- 采集系统声音模式:不需要文件名 ----
    capture_dev = None
    if args.capture is not None:
        capture_dev = args.capture
        if capture_dev == "auto":
            capture_dev = pick_loopback()
            if not capture_dev:
                print("[!] 没找到系统回环录音设备(通常叫“立体声混音”)。")
                print("    1) 先用 --list-devices 看看到底有哪些设备")
                print("    2) 都没法用的话:声音设置 -> 更多声音设置 -> 录制,")
                print("       在空白处右键选“显示禁用的设备”,把“立体声混音”启用")
                return 1
        print(f"音源 : 采集设备 [{capture_dev}]   (电脑放什么就推什么)")

    # 不带文件名 = 只想看看板子在不在(采集模式除外)
    if not args.file and capture_dev is None:
        host = args.host or find_board(args.port)
        if not host:
            print("[!] 没找到板子。确认板子开机了、和电脑在同一个 WiFi。")
            return 1
        print(f"找到板子: {host}")
        return 0

    files = None
    if capture_dev is None:
        files = collect(args.file)
        if not files:
            print(f"[!] {args.file} 里没有能放的音频")
            return 1
        if args.shuffle:
            import random
            random.shuffle(files)

        if len(files) == 1:
            print(f"音源 : {files[0]}")
        else:
            print(f"音源 : {args.file}  (共 {len(files)} 首)")

    host = args.host
    if host:
        print(f"目标 : {host}(手动指定)")
        save_ip(host)
    else:
        host = find_board(args.port)
        if not host:
            print("[!] 没找到板子。检查:板子开机了吗?和电脑在同一个 WiFi 吗?")
            print("    也可手动指定:python stream_audio.py 歌.mp3 192.168.1.182")
            return 1
        print(f"目标 : {host}(自动找到)")

    print("连接中 ...")
    try:
        sock = socket.create_connection((host, args.port), timeout=5)
    except OSError as e:
        print(f"[!] {host} 连不上({e}),重新扫描 ...")
        if args.host:
            print("    提示:去掉 IP 参数试试,让脚本自己在网段里找。")
            return 1
        clear_cache()
        host = find_board(args.port)
        if not host:
            print("[!] 没找到板子。检查:板子开机了吗?和电脑在同一个 WiFi 吗?")
            return 1
        print(f"目标 : {host}(重新找到)")
        try:
            sock = socket.create_connection((host, args.port), timeout=5)
        except OSError as e2:
            print(f"[!] 还是连不上 {host}: {e2}")
            return 1

    sock.settimeout(None)
    print("已连接,开始推流(Ctrl+C 停止)\n")

    total = 0 if files is None else len(files)
    try:
        if capture_dev is not None:
            print("\n开始采集。现在去放网易云(或任何声音)就行,Ctrl+C 停止")
            stream_once(capture_dev, sock, capture=True)
        else:
            while True:
                for i, fp in enumerate(files):
                    print(f"[{i + 1}/{total}] {os.path.basename(fp)}")
                    if not stream_once(fp, sock):
                        return 0
                if not args.loop:
                    break
                print("\n=== 放完了,再来一遍 ===")
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        sock.close()
        # 给板子一点时间发现连接断了,好让它切回 TF 卡播放
        time.sleep(0.3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
