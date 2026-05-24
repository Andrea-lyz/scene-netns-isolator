# Scene Netns Isolator

> Per-app network namespace isolation for Scene (`com.omarea.vtools`) on rooted Android devices.

---

## Overview

Scene Netns Isolator is a Magisk + Zygisk based network namespace isolation module designed specifically for Scene.

Its primary goal is to place the Scene app process and its root daemon into the same isolated Linux network namespace, so they can communicate through their own private loopback environment without conflicting with ports already occupied by the host system or other applications.

Typical ports affected include:

- `127.0.0.1:8788`
- `127.0.0.1:8765`

The project does not modify Scene itself. Instead, it provides isolation externally at the process / namespace level.

---

## Features

- Per-app network namespace isolation
- Isolates Scene without affecting the global system network stack
- Keeps Scene app and root daemon inside the same namespace
- Supports both `arm64-v8a` and `armeabi-v7a`
- Works through Zygisk process specialization
- Preserves localhost communication via isolated `lo`
- Does not require modifying Scene APK

---

## Architecture

The module consists of three major components:

### 1. `scene-netnsctl`

Native controller responsible for namespace lifecycle management.

Boot sequence:

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

The controller:

- creates a dedicated network namespace
- enables loopback (`lo`)
- pins the namespace with a persistent process
- exposes namespace FD transfer through Unix domain sockets

---

### 2. Zygisk module

When the target process belongs to `com.omarea.vtools`, the module:

1. connects to the Zygisk root companion
2. lets the companion connect to the namespace pinner
3. receives the forwarded namespace FD through `SCM_RIGHTS`
4. calls `setns(CLONE_NEWNET)`

This causes the Scene process to start directly inside the isolated namespace.

After specialization handling is finished, the module asks Zygisk to unload the module library to reduce module mapping residue in the target process lifetime.

---

### 3. `su` wrapper

Scene frequently launches privileged daemons through `su -c`.

To prevent namespace splitting, the module replaces the original invocation with:

```sh
scene-netnsctl enter -- /system/bin/sh -c '<original command>'
```

This guarantees:

- Scene app
- root shell
- daemon process

all remain inside the same namespace.

---

## Why This Exists

Android applications normally share the host network namespace.

This becomes problematic when:

- multiple tools bind fixed localhost ports
- daemons assume exclusive ownership of loopback sockets
- root helper services conflict with other apps

Using Linux network namespaces avoids modifying application logic while still providing isolated networking environments.

Naturally, it is possible that there are other, more compelling reasons.

---

## Repository Layout

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

## Build

Android NDK r27 or newer is recommended.

### Linux

```sh
ANDROID_NDK_HOME=/path/to/ndk \
bash scripts/build-native.sh
```

Package module:

```sh
(cd module && zip -r ../scene-netns-isolator.zip .)
```

---

### Windows

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\scripts\build-native.ps1
```

Package module:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\scripts\package.ps1
```

---

## Installation

1. Flash the generated zip in Magisk
2. Enable Zygisk
3. Reboot the device
4. Launch Scene

---

## Verification

Check namespace status:

```sh
su -c /data/adb/modules/scene-netns-isolator/bin/scene-netnsctl status
```

Check Scene namespace:

```sh
su -c 'readlink /proc/$(pidof com.omarea.vtools)/ns/net'
```

Check listening ports:

```sh
su -c 'ss -ltnp | grep -E ":8788|:8765"'
```

If isolation works correctly:

- Scene and its daemon should share the same namespace
- Host processes should remain outside that namespace

---

## Compatibility

Requirements:

- Root access
- Magisk
- Zygisk
- Kernel support for `CLONE_NEWNET`

---

## Security Notes

The project is designed to minimize observable global system changes.

Current implementation avoids:

- modifying global routing tables
- altering host namespace sockets
- patching Scene APK
- replacing Android framework components

Namespace transfer is performed through Unix domain sockets with credential validation (`SO_PEERCRED`) to reduce unauthorized access risk.

The Zygisk process does not read the root-private endpoint directly. Privileged access is handled by the Zygisk root companion, which forwards the namespace FD back to the target process.

---

## Limitations

- Package name is currently hardcoded to `com.omarea.vtools`
- Changes in Scene startup behavior may require matcher updates
- Some aggressive anti-hook / anti-root environments may still detect Zygisk presence

---

## License

Licensed under GNU GPL v3.
