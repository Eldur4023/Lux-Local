#!/usr/bin/env bash
# Cross-builds the native deps Lux's http/mail/imap modules need on Android: static OpenSSL + libcurl.
#   ANDROID_NDK=~/Android/android-ndk-r26d tools/android-deps.sh [abi] [prefix]
# Leaves <prefix>/lib/pkgconfig/libcurl.pc, which Lux's CMake finds through PKG_CONFIG_PATH
# (see README "Android"). Sources are downloaded to a scratch dir, nothing is vendored.
set -euo pipefail
ABI=${1:-arm64-v8a}; API=28
PREFIX=${2:-$PWD/build-android-deps/$ABI}
OPENSSL=3.5.9; CURL=${CURL_VERSION:-8.18.0}   # not 8.19+: custom IMAP commands (UID FETCH ...) return an empty body there (checked 8.19.0-8.22.0)
: "${ANDROID_NDK:?set ANDROID_NDK}"
case $ABI in arm64-v8a) SSLT=android-arm64;; x86_64) SSLT=android-x86_64;; armeabi-v7a) SSLT=android-arm;; *) echo "abi?"; exit 1;; esac
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
TC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64
mkdir -p "$PREFIX"

cd "$WORK"
if [ ! -f "$PREFIX/lib/libssl.a" ]; then
curl -fsSL "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL/openssl-$OPENSSL.tar.gz" | tar xz
( cd openssl-$OPENSSL
  PATH=$TC/bin:$PATH ANDROID_NDK_ROOT=$ANDROID_NDK \
    ./Configure $SSLT -D__ANDROID_API__=$API no-shared no-tests no-apps --prefix="$PREFIX" --libdir=lib
  PATH=$TC/bin:$PATH make -j"$(nproc)" build_libs
  PATH=$TC/bin:$PATH make install_dev )
fi

curl -fsSL "https://curl.se/download/curl-$CURL.tar.xz" | tar xJ
# No compiled-in CA location: curl then uses OpenSSL's default paths, and src/android.cpp points
# SSL_CERT_FILE at a PEM bundle it builds from the system CAs (their hashed names do not resolve with OpenSSL 3).
cmake -S curl-$CURL -B curl-build -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCURL_USE_OPENSSL=ON -DOPENSSL_INCLUDE_DIR="$PREFIX/include" -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.a" \
  -DCURL_CA_PATH=none -DCURL_CA_BUNDLE=none -DCURL_CA_FALLBACK=ON \
  -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
  -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DUSE_LIBIDN2=OFF
cmake --build curl-build -j"$(nproc)"
cmake --install curl-build
echo "done: $PREFIX/lib/pkgconfig/libcurl.pc"
