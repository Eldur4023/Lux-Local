#!/usr/bin/env bash
# Builds libluxlocal.so for the given ABIs and drops it into the Gradle project's jniLibs.
#   ANDROID_NDK=... tools/android-build.sh [abi ...]      (default: arm64-v8a x86_64)
set -euo pipefail
: "${ANDROID_NDK:?set ANDROID_NDK}"
cd "$(dirname "$0")/.."
cmake -S . -B build >/dev/null && cmake --build build --target respack >/dev/null
for ABI in "${@:-arm64-v8a x86_64}"; do for A in $ABI; do
  D=$PWD/build-android-deps/$A
  [ -f "$D/lib/libcurl.a" ] || tools/android-deps.sh "$A"
  PKG_CONFIG_LIBDIR=$D/lib/pkgconfig cmake -S . -B build-android-$A \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$A" -DANDROID_PLATFORM=android-28 -DLUX_HOST_RESPACK="$PWD/build/respack" \
    -DLUX_POSTGRES=OFF -DLUX_MYSQL=OFF -DLUX_PDF=OFF -DLUX_JEMALLOC=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-L$D/lib" -DCMAKE_EXE_LINKER_FLAGS="-L$D/lib" >/dev/null
  cmake --build build-android-$A --target luxlocal -j"$(nproc)"
  mkdir -p android/app/src/main/jniLibs/$A
  cp build-android-$A/libluxlocal.so android/app/src/main/jniLibs/$A/
done; done
