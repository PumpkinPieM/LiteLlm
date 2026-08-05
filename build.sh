#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
arch="host"
build_type="Release"
build_dir=""
sdk_root="${DEVECO_SDK_HOME:-}"
mock_runtime="ON"
build_tests="ON"

usage() {
  echo "Usage: $0 [--arch host|arm64-v8a|x86_64] [--build-type Debug|Release]"
  echo "          [--build-dir PATH] [--sdk-root PATH] [--real-runtime] [--without-tests]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch) arch="$2"; shift 2 ;;
    --build-type) build_type="$2"; shift 2 ;;
    --build-dir) build_dir="$2"; shift 2 ;;
    --sdk-root) sdk_root="$2"; shift 2 ;;
    --real-runtime) mock_runtime="OFF"; shift ;;
    --without-tests) build_tests="OFF"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ "$arch" != "host" && "$arch" != "arm64-v8a" && "$arch" != "x86_64" ]]; then
  echo "Unsupported architecture: $arch" >&2
  exit 2
fi
if [[ "$build_type" != "Debug" && "$build_type" != "Release" ]]; then
  echo "Unsupported build type: $build_type" >&2
  exit 2
fi
if [[ -z "$build_dir" ]]; then
  build_dir="$project_root/build-$arch"
fi

cmake_args=(
  -S "$project_root"
  -B "$build_dir"
  "-DCMAKE_BUILD_TYPE=$build_type"
  "-DLITE_LLM_USE_MOCK_RUNTIME=$mock_runtime"
  -DLITE_LLM_BUILD_SERVER=ON
  "-DLITE_LLM_BUILD_TESTS=$build_tests"
)

if [[ "$arch" != "host" ]]; then
  if [[ -n "${OHOS_NDK:-}" ]]; then
    ohos_native="$OHOS_NDK"
    hms_native="$OHOS_NDK"
  else
    if [[ -z "$sdk_root" && -d /Applications/DevEco-Studio.app/Contents/sdk ]]; then
      sdk_root=/Applications/DevEco-Studio.app/Contents/sdk
    fi
    if [[ -z "$sdk_root" || ! -d "$sdk_root" ]]; then
      echo "Unable to locate the DevEco SDK. Pass --sdk-root, set DEVECO_SDK_HOME, or set OHOS_NDK." >&2
      exit 2
    fi
    ohos_native="$sdk_root/default/openharmony/native"
    hms_native="$sdk_root/default/hms/native"
  fi
  cmake_bin="$ohos_native/build-tools/cmake/bin/cmake"
  ninja_bin="$ohos_native/build-tools/cmake/bin/ninja"
  toolchain="$hms_native/build/cmake/hmos.toolchain.bisheng.cmake"
  for required in "$cmake_bin" "$ninja_bin" "$toolchain"; do
    if [[ ! -f "$required" ]]; then
      echo "Required SDK file not found: $required" >&2
      exit 2
    fi
  done
  cmake_args+=(
    -G Ninja
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    "-DCMAKE_MAKE_PROGRAM=$ninja_bin"
    "-DHMOS_SDK_NATIVE=$hms_native"
    "-DOHOS_SDK_NATIVE=$ohos_native"
    -DCMAKE_SYSTEM_NAME=OHOS
    "-DOHOS_ARCH=$arch"
    "-DCMAKE_OHOS_ARCH_ABI=$arch"
  )
else
  cmake_bin=cmake
fi

"$cmake_bin" "${cmake_args[@]}"
"$cmake_bin" --build "$build_dir"

echo "Built liblite_llm.so and lite-server in $build_dir"
