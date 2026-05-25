# Scene Netns Isolator

> Per-app network namespace isolation for Scene (`com.omarea.vtools`) on rooted Android.

[中文 README](README.md)

---

## Overview

Scene Netns Isolator is a Zygisk-based Android module that puts the Scene app
process and its root daemon into a private Linux network namespace. They keep
talking to each other over loopback as before, but the host system and every
other app on the device sees nothing. Probes from other apps that try to
detect Scene by scanning `127.0.0.1:8788` (or any of the other ports Scene
uses) get a clean `ECONNREFUSED` that is **indistinguishable from "Scene is
not running"**.

The module never modifies the Scene APK and never writes any
host-visible iptables rules. All side effects are confined to Scene's own
namespace.

---

## How it works

```
host netns                                 isolated netns (per-pinner)
─────────────                             ────────────────────────────
                                           scene UI ─┐
scene-netnsctl pin (root) ──── unshare ──▶ daemon  ─┴─▶ 127.0.0.1:14754
                                           scn-i  10.99.99.2/30
                                               │
                                               │ veth pair
                                               ▼
scn-h  10.99.99.1/30 ──── iptables nat MASQUERADE ──▶ wlan0 / rmnet_data*
                                               ┆
ip rule iif scn-h ─────▶ table 99
                                               ┆
                                               ▼
                          AF_NETLINK monitor (host netns) → refresh table 99 on route changes
```

Four moving parts:

### 1. `scene-netnsctl` (native controller)

- `unshare(CLONE_NEWNET)` to create the isolated netns and pin it.
- Spawns a veth pair across the boundary: `scn-h` on host, `scn-i` on
  isolated.
- Installs `iptables -t nat MASQUERADE`, FORWARD ACCEPT rules, and a private
  routing table 99.
- A background watchdog subscribes to AF_NETLINK route events and refreshes
  table 99 with sub-second latency on wifi <-> mobile-data handovers.

### 2. Zygisk module

When the target process is identified as Scene:

1. Connects to the root companion to fetch the pinned netns FD.
2. Calls `setns(CLONE_NEWNET)` early in `preAppSpecialize`.
3. Asks Zygisk for `DLCLOSE_MODULE_LIBRARY` so the module's library is no
   longer mapped in the target process.

Every socket Scene creates afterwards lives entirely inside the isolated
netns.

### 3. `su` wrapper

Scene starts privileged daemons via `su -c <cmd>`. The wrapper rewrites the
invocation to:

```sh
scene-netnsctl enter -- /system/bin/sh -c '<original command>'
```

so the daemon enters the same isolated namespace as the UI and the two halves
keep finding each other over loopback.

### 4. veth + custom routing (new in v1.0.0)

Android stores default routes in per-network tables (`wlan0`,
`rmnet_data0`, …) selected by netd-managed fwmark rules. Forwarded packets
arriving from the isolated netns carry no fwmark and no matching uid, so the
kernel walks `main` and finds nothing — packets get dropped before they ever
hit the FORWARD chain.

The fix: install `ip rule iif scn-h pref 11000 lookup 99` and let the
watchdog mirror whichever default the device currently uses into table 99.
Netlink notifies us on every route change, so wifi disconnects, cellular
handovers, and ConnectivityService transitions are all picked up
automatically.

---

## Why bother

Other apps trying to detect Scene by probing `127.0.0.1:8788` get
`ECONNREFUSED`, which is the **same response they would get from a clean
device that has never run Scene**. Compared to BPF rewriting or iptables
redirection, this is a much quieter detection surface — there is no signal
to detect.

---

## Compatibility matrix

| Platform | Status | Notes |
|---|---|---|
| OnePlus 13 / ColorOS 16 + KernelSU + ZygiskNext | ✅ Verified | Reference device. Full Scene login + in-app billing works. |
| AOSP / Pixel | 🟢 High confidence | No OEM-specific routing quirks expected. |
| Samsung One UI | 🟡 Should work | AOSP-derived. Untested. |
| Xiaomi HyperOS / MIUI | 🟡 Should work after v1.1.0 | v1.0.0 had side effects (see below); v1.1.0 uses per-interface forwarding. |
| vivo OriginOS / iQOO | 🟡 Should work | Untested. |
| Older EMUI / Magic UI | 🟡 Should work | Still Linux kernel. Untested. |
| HarmonyOS NEXT | ❌ Won't work | Microkernel; no Linux netns. |
| Game-ROMs that strip NF_NAT | ❌ Won't work | iptables nat table missing. |

### Root managers

- **KernelSU** does **not** ship Zygisk. You need
  [ZygiskNext](https://github.com/Dr-TSNG/ZygiskNext) or ReZygisk.
- **Magisk** has built-in Zygisk; just enable it.
- **APatch** ships a Zygisk shim and should also work.

### Android version

- Android 12+: ✅ primary target
- Android 10/11: ⚠️ needs Magisk 24+ for Zygisk API v4
- Android 9 and below: ❌ not recommended

### Kernel requirements (every modern Android device satisfies these)

- `CONFIG_NET_NS` and `CONFIG_VETH`
- `CONFIG_NF_NAT` and `CONFIG_NF_NAT_MASQUERADE`
- `CONFIG_IP_MULTIPLE_TABLES`
- `CONFIG_IP_NF_FILTER` (FORWARD chain)

---

## Repository layout

```text
module/
  module.prop
  post-fs-data.sh
  service.sh
  customize.sh

src/native/
  scene_netnsctl.cpp     # netns pinner + veth/iptables + routing watchdog
  su_wrapper.cpp         # transparently turns `su -c` into `enter --`

src/zygisk/
  scene_netns_zygisk.cpp # process setns injection

scripts/
  build-native.sh
  build-native.ps1
  package.ps1
```

---

## Build

Android NDK r27 or newer recommended.

### Linux

```sh
ANDROID_NDK_HOME=/path/to/ndk bash scripts/build-native.sh
(cd module && zip -r ../scene-netns-isolator.zip .)
```

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

CI builds and publishes a release zip on every push to `main`.

---

## Installation

1. Install KernelSU (1.0+) + ZygiskNext, or Magisk + Zygisk.
2. Flash `scene-netns-isolator-*.zip` from the root manager.
3. Reboot.
4. Launch Scene.

---

## Verification

```sh
su

# 1. Pinner status
/data/adb/modules/scene-netns-isolator/bin/scene-netnsctl status

# 2. Is Scene actually in the isolated namespace?
#    The two inodes should differ.
readlink /proc/$(pidof com.omarea.vtools)/ns/net
readlink /proc/1/ns/net

# 3. Are Scene's ports invisible from host?  Should print nothing.
ss -ltnp | grep -E ":8788|:8765|:14754"

# 4. Can the isolated ns reach the internet?
/data/adb/modules/scene-netns-isolator/bin/scene-netnsctl enter -- \
    /system/bin/sh -c 'curl -m 5 -sI http://1.1.1.1/'
```

---

## Troubleshooting

The pinner writes everything to **`/dev/.15f1c4b9/pinner.log`**.

| Error | Meaning / Action |
|---|---|
| `host helper exited with N` | veth/iptables setup failed; `N` is the `_exit(N)` line in source. |
| `iptables: table nat does not exist` | Kernel was built without NF_NAT. Switch ROMs/kernels. |
| `... table empty; treating as unstable` | Network handover in progress, watchdog is waiting. Normal. |
| `RTNETLINK answers: ...` | Kernel/OEM rejected an `ip` command; share contents in an issue. |
| `[route-watchdog] netlink bind failed` | SELinux blocked AF_NETLINK; watchdog falls back to 30s polling automatically. |
| Everything looks fine but Scene can't connect | Open an issue with `iptables -L -n -v`, `iptables -t nat -L -n -v`, `ip rule`, `ip route show table all`. |

---

## Security notes

- veth only connects the isolated and host namespaces; no bridge, no other
  app traffic touches it.
- iptables changes are limited to one nat POSTROUTING rule (matches only
  the 10.99.99.0/30 source) and two FORWARD ACCEPT rules. Other traffic
  unaffected.
- No modifications to the Scene APK.
- No writes to the global `main` routing table; only the private table 99.
- Namespace FD transfer uses unix domain sockets gated by `SO_PEERCRED`;
  non-root clients are rejected.
- The Zygisk module dlclose-s itself once setns is done, so the target
  process keeps no module library mapped.

---

## Known limitations

- Package name is hard-coded to `com.omarea.vtools` (Scene 9.x).
- After veth + MASQUERADE, Android's `NetworkStatsManager` will report the
  Scene UID as having zero traffic on wifi/mobile (sk_uid is lost during
  SNAT). Functional impact: none. Optical impact: visible in some apps.
- IPv6 upstream is not configured. Scene is IPv4-only and OkHttp falls back
  to v4 in milliseconds, so this is invisible.
- Aggressive anti-root environments will still notice Zygisk and the custom
  veth interface, but that is orthogonal to this module's threat model
  (preventing other apps from probing Scene's ports).

### Xiaomi HyperOS / MIUI side effects

v1.0.0 set `net.ipv4.ip_forward = 1` globally in the host netns. Xiaomi's
network optimisation modules interpret this as "router mode" and break
Wi-Fi Direct, Mi Share, device interconnect, and screencast.

**Fixed in v1.1.0**: the module now uses per-interface
`conf/scn-h/forwarding` and `conf/<upstream>/forwarding` without touching
the global switch. If you still experience issues, open an issue with
`cat /proc/sys/net/ipv4/ip_forward` and pinner.log.

---

## Credits

- Original project: [NatumRagnag/scene-netns-isolator](https://github.com/NatumRagnag/scene-netns-isolator)
  established the setns + Zygisk skeleton.
- v1.0.0 added outbound connectivity, custom routing, and live route-event
  handling.

---

## License

GPL-3.0
