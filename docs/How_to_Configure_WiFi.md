# WiFi Configuration Guide

The WiFi on this board is **not** configured with a phone app. There are two ways —
use the first one.

---

## 1. Recommended: put a `wifi.txt` on the TF card

**No recompiling, no re-flashing.** Edit the file, reboot, done — this is how you
change WiFi.

### Steps

1. Copy `wifi.txt` to the **root directory of the TF card** (not inside a subfolder).
2. Open it with Notepad, remove the leading `#` from these two lines, and fill in
   your own values:

   ```
   ssid=YourWiFiName
   pass=YourWiFiPassword
   ```

   A complete file looks like this (comment lines may stay):

   ```
   # my home network
   ssid=MyHomeWiFi
   pass=Abc12345678
   ```

3. **Save it.** Use UTF-8 encoding — that is the Notepad default on Windows 11,
   so you do not need to change anything.
4. Put the card back into the board and **power-cycle it**.

Once connected, **your WiFi name appears in the top-left corner** of the screen.
If it failed you will see `no wifi` instead.

### Format rules (the parser is deliberately forgiving)

| Case | Accepted? |
|---|---|
| `ssid=` / `SSID=` / `Ssid=` | ✅ case-insensitive |
| `pass=` / `password=` | ✅ both work |
| `ssid = MyNet` (spaces around `=`) | ✅ |
| Non-ASCII (e.g. Chinese) SSID | ✅ just type it, no escaping |
| Windows line endings (CRLF) | ✅ |
| UTF-8 BOM added by Notepad | ✅ |
| A whole line starting with `#` | treated as a comment |
| An overly long line | truncated to 32 bytes (the 802.11 hard limit) |

### How to confirm it was read

Watch the serial log at 115200 baud and look for lines starting with `clock:`:

| Log line | Meaning |
|---|---|
| `已从 TF 卡读到 WiFi 配置: "xxx"` | ✅ read successfully; now see if it connects |
| `TF 卡里没有 /sdcard/wifi.txt,继续用默认凭据 "xxx"` | the file is missing, or not in the root |
| `/sdcard/wifi.txt 里没找到可用的 ssid=,忽略` | the file exists but the `ssid=` line is commented out or empty |
| `没配过 WiFi:把 wifi.txt 放进 TF 卡根目录后重启` | no usable credentials at all (and none compiled in) |

> These messages are in Chinese in the firmware. The English meaning is given above.

---

## 2. Alternative: compile the credentials into the firmware

Only suitable for **your own** build (builds meant for other people ship with
empty credentials by default).

In `components/user_app/user_app.cpp` find these two macros:

```c
#define WIFI_SSID        "MIKU"
#define WIFI_PASSWORD    "GBN1213326948"
```

Change them, then rebuild and flash:

```powershell
& L:\ESP32\tools\build.ps1 -p COM5 build flash
```

> ⚠️ **A password compiled into the firmware sits in the .bin file in plain text**
> — anyone can find it with a hex editor. Before giving a build to somebody else,
> build the "empty credentials" variant:
> ```powershell
> & L:\ESP32\tools\build.ps1 -DWIFI_CREDENTIALS_EMPTY=ON build
> ```
> Then search the .bin for your password (both UTF-8 and UTF-16) and make sure it
> really is gone.

---

## 3. What happens if both exist?

**The `wifi.txt` on the TF card wins.**

Startup order:

1. The board first connects using the credentials **compiled into the firmware**
   (that is why your own build is online ~1.6 s after boot);
2. After the TF card is mounted, a background task reads `wifi.txt`;
3. If it finds one, it **overrides** the credentials and reconnects.

Two consequences:

- ✅ To change WiFi without re-flashing, just edit `wifi.txt` — it overrides even
  the compiled-in credentials.
- ⚠️ **A bad `wifi.txt` on the card will break working compiled-in credentials**,
  because it overrides them. If the board suddenly stops connecting after you
  insert a card, check that card first.

---

## 4. Static IP (only affects the author's own build)

The author's build can give the board a **fixed address**, so the PC streaming and
monitoring tools always know where to connect.

**Without a static IP the board uses DHCP**, which is also the default.
To find the address, look at the **top-left corner of the board's screen**
(format `IP:port`).

The boot log tells you which mode is active:

```
I (1527) clock: 静态 IP: 192.168.1.182  网关: 192.168.1.1
```
or
```
I (xxxx) clock: 网络: DHCP(没定义静态 IP)
```

### How to enable a fixed address

A static IP depends on the specific network, and everyone's subnet differs, so it
is **not** hard-coded in the source. It lives in the un-tracked `wifi_secrets.h`.

Open `components/user_app/wifi_secrets.h` and set your own values:

```c
#define STATIC_IP_0      192
#define STATIC_IP_1      168
#define STATIC_IP_2      1
#define STATIC_IP_3      182
#define STATIC_GW_0      192      // gateway
#define STATIC_GW_1      168
#define STATIC_GW_2      1
#define STATIC_GW_3      1
```

Then rebuild and re-flash.

> ⚠️ **Comment those lines out when you move to a different network.**
> A hard-coded `192.168.1.x` on somebody else's LAN produces the confusing symptom
> "the WiFi name shows up, but the time never becomes correct" — there is no
> reachable gateway, so DNS resolution fails. It may also collide with an existing
> device on their network.
>
> Also keep the address outside your router's DHCP pool, otherwise it can clash
> with a device that the router hands the same address to.

---

## 5. Troubleshooting

**Q: The screen keeps showing `no wifi`**

Check in this order:

1. Is `wifi.txt` in the **root** of the TF card (not inside a folder)?
2. Is the filename exactly `wifi.txt`? (`wifi.txt.txt` is the classic Windows
   trap — Explorer hides known extensions by default.)
3. Are the `#` characters removed from the `ssid=` / `pass=` lines?
4. Look at the `clock:` lines in the serial log — they say explicitly whether the
   file was missing or was read but failed to connect.

**Q: The WiFi name shows up, but the time is wrong**

Almost certainly the static IP issue — see section 4. Look for a
`静态 IP:` line in the boot log.

**Q: It keeps retrying and never connects**

- The board only supports **2.4 GHz**; it **cannot** connect to a 5 GHz network.
  If your router uses one combined SSID for both bands, try giving the 2.4 GHz
  band its own separate name.
- SSIDs are case-sensitive and leading/trailing spaces matter.
- Type the password as-is — no quotes needed.

**Q: It worked, then stopped after I inserted another card**

That card's root probably contains a `wifi.txt` which overrode the working
credentials (see section 3). Remove the card, or fix the file on it.
