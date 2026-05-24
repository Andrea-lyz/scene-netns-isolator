#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [[ -z "${NDK}" ]]; then
  echo "ANDROID_NDK_HOME or ANDROID_NDK_ROOT is required" >&2
  exit 1
fi

CLANG_BIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [[ ! -d "${CLANG_BIN}" ]]; then
  CLANG_BIN="${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/bin"
fi
if [[ ! -d "${CLANG_BIN}" ]]; then
  CLANG_BIN="${NDK}/toolchains/llvm/prebuilt/windows-x86_64/bin"
fi
if [[ ! -d "${CLANG_BIN}" ]]; then
  echo "Unable to find LLVM toolchain under ${NDK}" >&2
  exit 1
fi

OUT_BIN="${ROOT}/module/bin"
OUT_ZYGISK="${ROOT}/module/zygisk"
ZYGISK_INCLUDE="${ROOT}/third_party/zygisk"
mkdir -p "${OUT_BIN}" "${OUT_ZYGISK}"

build_arch() {
  local name="$1"
  local compiler="$2"
  local build_dir="${ROOT}/build/${name}"
  local cxx="${CLANG_BIN}/${compiler}"
  local strip="${CLANG_BIN}/llvm-strip"

  if [[ ! -x "${cxx}" ]]; then
    echo "Missing compiler: ${cxx}" >&2
    exit 1
  fi

  mkdir -p "${build_dir}"

  "${cxx}" -std=c++17 -fPIE -Wall -Wextra -Os \
    "${ROOT}/src/native/scene_netnsctl.cpp" \
    -o "${build_dir}/scene-netnsctl" -static-libstdc++

  "${cxx}" -std=c++17 -fPIE -Wall -Wextra -Os \
    "${ROOT}/src/native/su_wrapper.cpp" \
    -o "${build_dir}/su" -static-libstdc++

  "${cxx}" -std=c++17 -fPIC -shared -Wall -Wextra -Os \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
    -I"${ZYGISK_INCLUDE}" \
    "${ROOT}/src/zygisk/scene_netns_zygisk.cpp" \
    -llog \
    -nostdlib++ \
    -o "${build_dir}/zygisk.so"

  if [[ -x "${strip}" ]]; then
    "${strip}" "${build_dir}/scene-netnsctl" "${build_dir}/su" "${build_dir}/zygisk.so"
  fi

  if [[ "${name}" == "arm64-v8a" ]]; then
    cp "${build_dir}/scene-netnsctl" "${OUT_BIN}/scene-netnsctl"
    cp "${build_dir}/su" "${OUT_BIN}/su"
  fi
  cp "${build_dir}/zygisk.so" "${OUT_ZYGISK}/${name}.so"
}

build_arch "arm64-v8a" "aarch64-linux-android23-clang++"
build_arch "armeabi-v7a" "armv7a-linux-androideabi23-clang++"

echo "Native artifacts written to ${OUT_BIN} and ${OUT_ZYGISK}"
