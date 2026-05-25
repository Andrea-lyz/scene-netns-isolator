# Scene Netns Isolator

> 面向 Scene (`com.omarea.vtools`) 的 Android 网络命名空间隔离模块。

[English README](README_en.md)

---

## 项目简介

Scene Netns Isolator 是一个基于 Zygisk 的 Android 网络命名空间隔离模块，专门用于 Scene。

它的核心目标是：

把 Scene App 进程和它的 root daemon 一起放进同一个独立的 Linux network namespace，让它们在自己的 loopback 里通信，**对宿主以及其它 App 完全不可见**。

典型隔离的端口：

- `127.0.0.1:8788`
- `127.0.0.1:8765`
- Scene 9.x 之后用的 `127.0.0.1:14731` / `14754` 等

本项目不修改 Scene APK，也不写 iptables 全局规则给宿主看。所有改动都局限在 Scene 自己的 namespace 内。

---

## 工作原理

```
host netns                                  isolated netns (per-pinner)
─────────────                              ────────────────────────────
                                            scene UI ─┐
scene-netnsctl pin (root)  ──── unshare ──▶ daemon  ─┴─▶ 127.0.0.1:14754
                                            scn-i  10.99.99.2/30
                                                │
                                                │ veth pair
                                                ▼
scn-h  10.99.99.1/30   ──── iptables nat MASQUERADE ──▶ wlan0 / rmnet_data*
                                                ┆
ip rule iif scn-h  ─────▶ table 99
                                                ┆
                                                ▼
                          AF_NETLINK monitor (host netns) → refresh table 99 on route changes
```

模块由四块组成：

### 1. `scene-netnsctl`（native 控制器）

- `unshare(CLONE_NEWNET)` 创建 isolated netns，pin 住自己保持 ns 存活
- 跨 ns 拉起一对 veth：host 端 `scn-h`，isolated 端 `scn-i`
- 装好 `iptables -t nat MASQUERADE`、FORWARD ACCEPT、自定义路由表 99
- 后台 watchdog 用 AF_NETLINK 订阅路由事件，wifi/数据切换秒级响应

### 2. Zygisk 模块

监测到 Scene 进程被 fork 时：

1. 通过 root companion 拿到 isolated ns_fd
2. 在 specialize 早期 `setns(CLONE_NEWNET)` 切进去
3. 处理完毕后 `DLCLOSE_MODULE_LIBRARY`，自身从进程映射里消失

Scene 进程之后建的所有 socket 都在 isolated netns 里，host 看不到。

### 3. `su` wrapper

Scene 用 `su -c <cmd>` 拉 daemon。wrapper 把命令重写成：

```sh
scene-netnsctl enter -- /system/bin/sh -c '<原命令>'
```

保证 daemon 也进入同一 isolated ns，跟 UI 通过 loopback 通信。

### 4. veth + 自定义路由（v1.0.0 新加）

Android 把 default 路由分散在每张网卡的私有路由表里（`wlan0`、`rmnet_data0` 等），由 netd 的 fwmark 规则选择。我们自己 fork 出来的进程不受 netd 管，所以走默认 main 表找不到 default。

修法：用 `iif scn-h pref 11000 lookup 99` 把 isolated 出向流量导到自建表 99，watchdog 把当前真正的 default route 复制进去。netlink 监听到路由变化（wifi 上线、切流量）就重刷。

---

## 为什么需要它

普通 App 在宿主 netns 探测 `127.0.0.1:8788` 时会拿到 `ECONNREFUSED`，跟"Scene 没启动"的状态**完全不可区分**。

也就是说本模块在网络层提供的是一种"无信号检测面"——比 BPF 改写 / iptables 重定向那种带特征的方案干净得多。

---

## 兼容性矩阵

| 平台 | 状态 | 备注 |
|---|---|---|
| OnePlus 13 / ColorOS 16 + KernelSU + ZygiskNext | ✅ 已验证 | 主测设备，Scene 完整登录、内购可用 |
| AOSP / Pixel | 🟢 高置信 | 无 OEM 自定义路由策略，理论最干净 |
| 三星 One UI | 🟡 应该可用 | 接近 AOSP，未实测 |
| 小米 HyperOS / MIUI | ⚠️ 部分功能受影响 | Scene 本身可用，但反馈设备互联 / 小米互传 / 共享桌面会失败。详见下方副作用说明 |
| vivo OriginOS / iQOO | 🟡 应该可用 | 未实测 |
| 老 EMUI / Magic UI | 🟡 应该可用 | 仍是 Linux 内核，未实测 |
| HarmonyOS NEXT | ❌ 不工作 | 鸿蒙微内核，没有 Linux netns |
| 部分裁剪 NF_NAT 的游戏 ROM | ❌ 不工作 | iptables nat 表缺失 |

### Root 方案

- **KernelSU**：自身**不带 Zygisk**，必须装 [ZygiskNext](https://github.com/Dr-TSNG/ZygiskNext) 或 ReZygisk
- **Magisk**：自带 Zygisk，开关打开即可
- **APatch**：自带 zygisk 兼容层，理论可用

### Android 版本

- Android 12+：✅ 主目标
- Android 10/11：⚠️ 需要 Magisk 24+ 提供 Zygisk API v4
- Android 9 及以下：❌ 不推荐

### 内核要求（基本所有现代设备都满足）

- `CONFIG_NET_NS` + `CONFIG_VETH`
- `CONFIG_NF_NAT` + `CONFIG_NF_NAT_MASQUERADE`
- `CONFIG_IP_MULTIPLE_TABLES`
- `CONFIG_IP_NF_FILTER`（FORWARD 链）

---

## 仓库结构

```text
module/
  module.prop
  post-fs-data.sh
  service.sh
  customize.sh

src/native/
  scene_netnsctl.cpp     # netns pinner + veth/iptables/路由 watchdog
  su_wrapper.cpp         # 透明把 su -c 包成 enter --

src/zygisk/
  scene_netns_zygisk.cpp # 进程 setns 注入

scripts/
  build-native.sh
  build-native.ps1
  package.ps1
```

---

## 构建

推荐 Android NDK r27 或更新。

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

CI 上每次 push 到 main 会自动构建并发布到 Releases。

---

## 安装

1. 装 KernelSU（推荐 v1.0+）+ ZygiskNext，或 Magisk + Zygisk
2. 在 root 管理器里刷入 `scene-netns-isolator-*.zip`
3. 重启
4. 打开 Scene

---

## 验证

```sh
su

# 1. pinner 状态
/data/adb/modules/scene-netns-isolator/bin/scene-netnsctl status

# 2. Scene 是否进了 isolated ns（inode 跟 init 不同就是对的）
readlink /proc/$(pidof com.omarea.vtools)/ns/net
readlink /proc/1/ns/net

# 3. host 是否看不到 Scene 端口（应该没输出）
ss -ltnp | grep -E ":8788|:8765|:14754"

# 4. isolated ns 内部能不能联网
/data/adb/modules/scene-netns-isolator/bin/scene-netnsctl enter -- \
    /system/bin/sh -c 'curl -m 5 -sI http://1.1.1.1/'
```

---

## 排错

模块所有日志在 **`/dev/.15f1c4b9/pinner.log`**，root 可读。

| 错误 | 含义 / 排查 |
|---|---|
| `host helper exited with N` | veth/iptables 设置失败，N 是源码里 `_exit(N)` 的编号，对照源码看哪步 |
| `iptables: table nat does not exist` | 你的内核裁掉了 NF_NAT。换内核或换 ROM |
| `... table empty; treating as unstable` | 网络切换时 ConnectivityService 还没稳定，watchdog 在等。正常状态 |
| `RTNETLINK answers: ...` | 内核 / OEM 拒绝某条 ip 命令；具体内容贴 issue |
| `[route-watchdog] netlink bind failed` | SELinux 拒绝 AF_NETLINK 创建，watchdog 自动降级到 30s 轮询 |
| 一切看着正常但 Scene 不通 | 请贴 `iptables -L -n -v`、`iptables -t nat -L -n -v`、`ip rule`、`ip route show table all` 发 issue |

---

## 安全说明

- veth pair 只在 isolated netns 和 host netns 之间互通，没有 bridge，不接触其它 App
- iptables 改动局限在 nat POSTROUTING（仅匹配 10.99.99.0/30 源）+ FORWARD 两条 ACCEPT，不影响其它流量
- 不修改 Scene APK
- 不写全局 routing 表，只用自建的 table 99
- namespace fd 通过 unix socket + `SO_PEERCRED` 校验传递，非 root 客户端会被拒
- Zygisk 模块在 setns 完成后立即 dlclose，进程映射里没有自身残留

---

## 已知限制

- 包名固定为 `com.omarea.vtools`（Scene 9.x 系列）
- veth + MASQUERADE 后，`NetworkStatsManager` 看 Scene UID 的流量统计为 0（包经 SNAT 后丢了 sk_uid）
- Scene 自身依赖 IPv4，IPv6 上游目前不主动设置（OkHttp 会快速 fallback v4，无感知）
- 强对抗 anti-root 环境仍会检测到 Zygisk + 自定义网卡，但与本模块的设计目标（防普通 App 端口探测）无关

### 小米 HyperOS / MIUI 的副作用

已知会**影响以下系统功能**，原因是模块在 host netns 全局开启了 `net.ipv4.ip_forward = 1`，
小米 ROM 的网络优化模块（`MIUI NetworkBoost` 等）对此变化敏感：

- 设备互联（Mi Smart Hub）连不上
- 小米互传文件失败
- 共享桌面 / 多屏协同连接失败

Scene 本身的功能不受影响。如果你重度使用上述功能，请暂时不要安装本模块，或仅在需要 Scene 时短期启用。

后续版本计划改用接口级 `conf/<iface>/forwarding` 替代全局开关，理论上能消除这一影响。

---

## 致谢

- 原始项目：[NatumRagnag/scene-netns-isolator](https://github.com/NatumRagnag/scene-netns-isolator) 提供了 setns + Zygisk 框架
- v1.0.0 在此之上补完了出口连通性、路由策略和网络切换响应

---

## 许可证

GPL-3.0
