"""把 PC 的实时资源占用推给 ESP32-RLCD(资源监视器)。

数据源
    CPU 占用 / 内存 / 网络   psutil —— 实测 0.04~3.9 ms/次,基本不占 CPU
    GPU 占用 / 温度 / 显存   NVML(pynvml);没装就走 nvidia-smi 子进程(37 ms,慢)
    CPU 温度                 ⚠ Windows 上【拿不到】,必须借助第三方工具。
                             按顺序自动尝试:
                               1. LibreHardwareMonitor 的 WMI(推荐,免费开源)
                               2. HWiNFO 的「写入注册表」
                             都没有就这项发 -1,板子上显示 "--"

用法
    python pc_monitor.py                     # 自动找板子
    python pc_monitor.py 192.168.1.182       # 手动指定
    python pc_monitor.py --port 3334 --rate 1

推荐先装这两个(可选,但能大幅降低开销/解锁 CPU 温度):
    pip install nvidia-ml-py pywin32

按 Ctrl+C 停止。
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import psutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import pynvml
    HAVE_NVML = True
except ImportError:
    HAVE_NVML = False

try:
    import win32com.client
    HAVE_WMI = True
except ImportError:
    HAVE_WMI = False

try:
    import winreg
    HAVE_REG = True
except ImportError:
    HAVE_REG = False

DEFAULT_PORT = 3334


# ---------------------------------------------------------------- GPU ----

class Gpu:
    """优先 NVML(微秒级);退回 nvidia-smi 子进程(37ms,所以降频采样)。"""

    def __init__(self):
        self.ok = False
        self.util = self.temp = -1
        self.vram_u = self.vram_t = -1
        self._last = 0.0
        if HAVE_NVML:
            try:
                pynvml.nvmlInit()
                self._h = pynvml.nvmlDeviceGetHandleByIndex(0)
                self.ok = True
                name = pynvml.nvmlDeviceGetName(self._h)
                if isinstance(name, bytes):
                    name = name.decode()
                print(f"GPU  : {name}  (NVML 直读)")
            except Exception as e:                      # noqa: BLE001
                print(f"GPU  : NVML 初始化失败({e}),改用 nvidia-smi")
        if not self.ok:
            self._smi = None
            try:
                out = subprocess.run(
                    ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
                    capture_output=True, text=True, timeout=5)
                if out.returncode == 0:
                    self._smi = out.stdout.strip()
                    self.ok = True
                    print(f"GPU  : {self._smi}  (nvidia-smi 子进程,较慢)")
            except Exception:                           # noqa: BLE001
                pass
        if not self.ok:
            print("GPU  : 没有可用的 NVIDIA 显卡")

    def sample(self, interval=1.0, slow_period=2.0):
        if not self.ok:
            return
        if HAVE_NVML and self._h is not None:
            try:
                u = pynvml.nvmlDeviceGetUtilizationRates(self._h)
                m = pynvml.nvmlDeviceGetMemoryInfo(self._h)
                self.util = u.gpu
                self.temp = pynvml.nvmlDeviceGetTemperature(
                    self._h, pynvml.NVML_TEMPERATURE_GPU)
                self.vram_u = int(m.used / 1048576)
                self.vram_t = int(m.total / 1048576)
            except Exception:                           # noqa: BLE001
                pass
            return
        # nvidia-smi 慢,隔几秒才采一次
        now = time.monotonic()
        if now - self._last < slow_period:
            return
        self._last = now
        try:
            out = subprocess.run(
                ["nvidia-smi",
                 "--query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=5)
            p = [x.strip() for x in out.stdout.strip().split(",")]
            self.util, self.temp = int(p[0]), int(p[1])
            self.vram_u, self.vram_t = int(p[2]), int(p[3])
        except Exception:                               # noqa: BLE001
            pass


# ------------------------------------------------------------ CPU 温度 ----

class CpuTemp:
    """Windows 上 CPU 温度必须靠第三方工具,这里按可用性依次尝试。

    1. LibreHardwareMonitor / OpenHardwareMonitor 的 WMI(需 pywin32)
    2. HWiNFO 写入 HKCU\\Software\\HWiNFO64\\VSB 的值(无需额外模块)
    3. 都没有 -> None,板子上会显示 "--"
    """

    def __init__(self):
        self.kind = "none"
        self._last = 0.0
        self.value = None
        if HAVE_WMI:
            for ns in ("LibreHardwareMonitor", "OpenHardwareMonitor"):
                try:
                    win32com.client.GetObject(
                        f"winmgmts:\\\\.\\root\\{ns}")
                    self.kind = f"wmi:{ns}"
                    break
                except Exception:                       # noqa: BLE001
                    continue
        if self.kind == "none" and HAVE_REG:
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                    r"Software\HWiNFO64\VSB"):
                    self.kind = "hwinfo-reg"
            except OSError:
                pass
        print({"none": "CPU 温度: 没有可用的数据源 -> 显示 --\n"
                       "         想要的话:装 LibreHardwareMonitor(免费开源),"
                       "或者开 HWiNFO 的「写入注册表」",
               }.get(self.kind, f"CPU 温度: {self.kind}"))

    def _from_wmi(self):
        ns = self.kind.split(":", 1)[1]
        wmi = win32com.client.GetObject(f"winmgmts:\\\\.\\root\\{ns}")
        best = None
        for s in wmi.ExecQuery("SELECT * FROM Sensor WHERE SensorType='Temperature'"):
            n = str(s.Name)
            if n.startswith("CPU") and ("Package" in n or "Core" in n or "Total" in n):
                return float(s.Value)
            if best is None and n.startswith("CPU"):
                best = float(s.Value)
        return best

    def _from_reg(self):
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\HWiNFO64\VSB") as k:
            labels, values = {}, {}
            i = 0
            while True:
                try:
                    name, data, _ = winreg.EnumValue(k, i)
                except OSError:
                    break
                if name.startswith("Label"):
                    labels[name[5:]] = str(data)
                elif name.startswith("Value"):
                    values[name[5:]] = data
                i += 1
            for idx, lab in labels.items():
                if "CPU" in lab and ("Package" in lab or "温度" in lab) \
                        and idx in values:
                    try:
                        v = float(values[idx])
                    except (TypeError, ValueError):
                        continue
                    if 0 < v < 120:                     # 合理性检查
                        return v
        return None

    def sample(self, slow_period=2.0):
        if self.kind == "none":
            return
        now = time.monotonic()
        if now - self._last < slow_period:
            return
        self._last = now
        try:
            v = self._from_wmi() if self.kind.startswith("wmi") else self._from_reg()
            if v is not None and 0 < v < 120:
                self.value = v
        except Exception:                               # noqa: BLE001
            pass


# ---------------------------------------------------------------- 主流程 ----

def main():
    ap = argparse.ArgumentParser(description="把 PC 资源占用推给 ESP32")
    ap.add_argument("host", nargs="?", help="板子 IP,不填就自动找")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--rate", type=float, default=1.0, help="每秒推几次,默认 1")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="采样周期秒,默认 1")
    ap.add_argument("--echo", action="store_true", help="把每条数据也打到屏幕上")
    ap.add_argument("--dry-run", action="store_true",
                    help="只采样打印,不连板子(用来验证数据源)")
    ap.add_argument("--count", type=int, default=0,
                    help="采够这么多条就退出,0 = 一直跑")
    args = ap.parse_args()

    host = args.host
    if not args.dry_run and not host:
        try:
            from stream_audio import find_board       # 复用现成的发现逻辑
            host = find_board(args.port)
        except Exception:                             # noqa: BLE001
            host = None
        if not host:
            print("[!] 没找到板子,请手动指定 IP")
            return 1

    gpu = Gpu()
    cpu_temp = CpuTemp()
    hostname = "".join(c for c in socket.gethostname() if c.isalnum() or c in "-_")[:24]
    print(f"主机 : {hostname}")
    if args.dry_run:
        print(f"模式 : 自测(不连板子)   采样 {args.interval}s")
    else:
        print(f"目标 : {host}:{args.port}   采样 {args.interval}s")

    psutil.cpu_percent(interval=None)                 # 预热,第一次必返回 0.0
    prev_net = psutil.net_io_counters()
    prev_t = time.monotonic()

    sock = None
    n = 0
    while True:
        # ---- 采样 ----
        cpu = psutil.cpu_percent(interval=None)
        vm = psutil.virtual_memory()
        net = psutil.net_io_counters()
        now = time.monotonic()
        dt = max(now - prev_t, 1e-3)
        rx = (net.bytes_recv - prev_net.bytes_recv) / dt / 1024.0    # KB/s
        tx = (net.bytes_sent - prev_net.bytes_sent) / dt / 1024.0
        prev_net, prev_t = net, now

        gpu.sample(slow_period=2.0)
        cpu_temp.sample(slow_period=2.0)
        ct = int(cpu_temp.value) if cpu_temp.value else -1

        line = (f"HOST={hostname} CPU={cpu:.1f} RAM={vm.percent:.1f} "
                f"RAMU={vm.used // 1048576} RAMT={vm.total // 1048576} "
                f"CPUT={ct} GPU={gpu.util} GPUT={gpu.temp} "
                f"VRAMU={gpu.vram_u} VRAMT={gpu.vram_t} "
                f"NETRX={rx:.0f} NETTX={tx:.0f}\n")

        # ---- 发送(断了就重连)----
        n += 1
        if args.echo or args.dry_run:
            print(line.rstrip())
        if args.dry_run:
            if args.count and n >= args.count:
                return 0
            time.sleep(args.interval)
            continue
        if sock is None:
            try:
                sock = socket.create_connection((host, args.port), timeout=3)
                sock.settimeout(None)
                print(f"已连接 {host}:{args.port}")
            except OSError:
                sock = None
                time.sleep(2)
                continue
        try:
            sock.sendall(line.encode())
        except OSError:
            print("连接断开,重连中 ...")
            sock.close()
            sock = None

        if args.count and n >= args.count:
            print(f"已推送 {n} 条,退出")
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已停止")
