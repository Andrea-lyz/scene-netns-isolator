# Scene Netns Isolator

> 面向 Scene (`com.omarea.vtools`) 的 Android 网络命名空间隔离模块。

---

## 项目简介

Scene Netns Isolator 是一个基于 Magisk 与 Zygisk 的 Android 网络命名空间隔离模块，专门用于 Scene。

它的核心目标是：

将 Scene App 进程与其 root daemon 放入同一个独立的 Linux network namespace 中，使其能够在私有 loopback 网络环境中通信，而不会与宿主系统或其他应用占用的端口发生冲突。

典型端口包括：

- `127.0.0.1:8788`
- `127.0.0.1:8765`

本项目不会修改 Scene 本体代码，而是通过 Linux namespace 机制在系统外侧提供隔离能力。

---

## 功能特性

- 针对单个应用的 network namespace 隔离
- 不影响全局系统网络栈
- Scene App 与 root daemon 共用同一 namespace
- 支持 `arm64-v8a` 与 `armeabi-v7a`
- 基于 Zygisk specialization 工作
- 保留 loopback (`lo`) 通信能力
- 不修改 Scene APK

---

## 工作原理

模块主要由三个部分组成：

### 1. `scene-netnsctl`

负责 network namespace 生命周期管理的原生控制器。

启动阶段：

```text
service.sh
    ↓
scene-netnsctl pin
    ↓
unshare(CLONE_NEWNET)
    ↓
bring up lo
    ↓
keep namespace alive with persistent pinner
```

控制器会：

- 创建独立 network namespace
- 启用 loopback 接口
- 使用常驻 pinner 保持 namespace 存活
- 通过 Unix domain socket 分发 namespace FD

---

### 2. Zygisk 模块

当目标进程属于 `com.omarea.vtools` 时：

1. Zygisk 连接 root companion
2. companion 连接 namespace pinner
3. companion 通过 `SCM_RIGHTS` 将 namespace FD 转发给目标进程
4. 目标进程调用 `setns(CLONE_NEWNET)`

最终使 Scene 进程直接运行在独立网络命名空间中。

模块在完成 specialization 处理后会请求 Zygisk 卸载模块库，减少目标进程生命周期里的模块映射残留。

---

### 3. `su` wrapper

Scene 会通过 `su -c` 拉起 root daemon。

为了避免 namespace 分裂，模块会将调用改写为：

```sh
scene-netnsctl enter -- /system/bin/sh -c '<original command>'
```

这样可以保证：

- Scene App
- root shell
- daemon

始终位于同一个 network namespace 中。

---

## 为什么需要它

默认情况下，Android 用户态应用共享宿主 network namespace。

当多个应用：

- 使用固定 localhost 端口
- 假定 loopback 独占
- 启动 root daemon

时，就容易发生冲突。

Linux network namespace 可以在不修改应用逻辑的前提下，提供独立网络环境。

当然，有可能有其他更有趣的原因。

---

## 仓库结构

```text
module/
  module.prop
  post-fs-data.sh
  service.sh
  customize.sh

src/native/
  scene_netnsctl.cpp
  su_wrapper.cpp

src/zygisk/
  scene_netns_zygisk.cpp

scripts/
  build-native.sh
  build-native.ps1
  package.ps1
```

---

## 构建

推荐使用 Android NDK r27 或更新版本。

### Linux

```sh
ANDROID_NDK_HOME=/path/to/ndk \
bash scripts/build-native.sh
```

打包模块：

```sh
(cd module && zip -r ../scene-netns-isolator.zip .)
```

---

### Windows

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\scripts\build-native.ps1
```

打包模块：

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\scripts\package.ps1
```

---

## 安装方法

1. 在 Magisk 中刷入生成的 zip
2. 启用 Zygisk
3. 重启设备
4. 启动 Scene

---

## 验证运行状态

检查 namespace 状态：

```sh
su -c /data/adb/modules/scene-netns-isolator/bin/scene-netnsctl status
```

检查 Scene namespace：

```sh
su -c 'readlink /proc/$(pidof com.omarea.vtools)/ns/net'
```

检查监听端口：

```sh
su -c 'ss -ltnp | grep -E ":8788|:8765"'
```

正常情况下：

- Scene 与 daemon 会共享同一个 namespace
- 宿主系统进程位于 namespace 外部

---

## 兼容性

需要：

- Root 权限
- Magisk
- Zygisk
- 内核支持 `CLONE_NEWNET`

---

## 安全说明

本项目设计目标之一是尽量减少全局系统可见改动。

当前实现不会：

- 修改宿主 routing table
- 改写全局 socket
- Patch Scene APK
- 替换 Android framework 组件

namespace FD 通过 Unix domain socket + `SO_PEERCRED` 校验进行传递，以降低未授权访问风险。

Zygisk 侧不直接读取 root 私有 endpoint，而是通过 Zygisk root companion 完成特权访问，再把 namespace FD 传回目标进程。

---

## 已知限制

- 当前包名固定为 `com.omarea.vtools`
- 如果 Scene 修改启动链，匹配逻辑可能需要更新
- 某些强对抗 anti-root 环境仍可能检测到 Zygisk

---

## 许可证

基于 GNU GPL v3 开源。
