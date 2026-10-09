#!/usr/bin/env bash
# Reproduz a toolchain usada pelo laboratório num Ubuntu 24.04, SEM downloads de
# releases do GitHub (bloqueados neste ambiente): tudo vem do apt ou de `git clone`.
#
#   - arm-none-eabi-gcc 13 (Ubuntu) com multilib thumb/v7-a+simd/hard = ABI do Vita
#   - vita-toolchain (vita-elf-create, vita-make-fself, vita-mksfoex, vita-pack-vpk…)
#   - libelf 0.8.12 (espelho git) no lugar da 0.8.13 exigida (API compatível)
#   - vita-headers + 496 bibliotecas de stubs geradas com vita-libs-gen-2
#   - mipsel-linux-gnu-gcc + qemu-user (oráculo de referência), arm-linux-gnueabihf-gcc
#
# usage: bootstrap-toolchain.sh [prefix=/home/user/deps]
set -euo pipefail
DEPS="${1:-/home/user/deps}"
VITASDK="$DEPS/vitasdk"
mkdir -p "$DEPS"
export DEBIAN_FRONTEND=noninteractive

apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi gcc-arm-linux-gnueabihf \
  libc6-dev-armhf-cross gcc-mipsel-linux-gnu libc6-dev-mipsel-cross qemu-user \
  zlib1g-dev libzip-dev libyaml-dev cmake ninja-build

cd "$DEPS"
[[ -d libelf-src ]] || git clone -q --depth 1 https://github.com/WolfgangSt/libelf libelf-src
[[ -d vita-toolchain ]] || git clone -q --depth 1 https://github.com/vitasdk/vita-toolchain
[[ -d vita-headers ]] || git clone -q --depth 1 https://github.com/vitasdk/vita-headers

(cd libelf-src && ./configure --prefix="$DEPS/deps_build" --disable-shared CFLAGS="-O2 -fPIC" >/dev/null &&
  make -j"$(nproc)" >/dev/null && make install instroot= >/dev/null)
ln -sfn . "$DEPS/deps_build/include/libelf/libelf"   # <libelf/sys_elf.h> include style

cd vita-toolchain
git submodule update --init --depth 1
sed -i 's/libelf=0.8.13/libelf>=0.8.12/' cmake/Modules/Findlibelf.cmake
PKG_CONFIG_PATH="$DEPS/deps_build/lib/pkgconfig" cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$VITASDK" -DTOOLCHAIN_DEPS_DIR="$DEPS/deps_build"
ninja -C build install >/dev/null

for t in gcc g++ as ar ld nm objcopy objdump readelf strip ranlib size; do
  ln -sf "$(command -v arm-none-eabi-$t)" "$VITASDK/bin/arm-vita-eabi-$t"
done

mkdir -p "$VITASDK/arm-vita-eabi/lib" "$VITASDK/arm-vita-eabi/include"
cd "$DEPS/vita-headers"
rm -rf stubs
PATH="$VITASDK/bin:$PATH" vita-libs-gen-2 -yml=./db -output=./stubs
(cd stubs && PATH="$VITASDK/bin:$PATH" VITASDK="$VITASDK" make -j"$(nproc)" install >/dev/null 2>&1)
cp -r include/* "$VITASDK/arm-vita-eabi/include/"
echo "toolchain pronta em $VITASDK ($(ls "$VITASDK/arm-vita-eabi/lib" | wc -l) bibliotecas de stubs)"
