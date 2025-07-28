#!/bin/bash

# ===============================
# Tobrut Exotic - AOSP Build Script
# ===============================

set -e

# Set Timezone
sudo rm -f /etc/localtime
sudo ln -sf /usr/share/zoneinfo/Asia/Jakarta /etc/localtime

# Install Dependencies
sudo apt update -y
sudo apt install -y bc cpio flex bison aptitude git python-is-python3 tar aria2 perl wget curl lz4 libssl-dev zip unzip

# Setup Environment Variables
export BUILD_TIME=$(date "+%d%m%Y-%H%M")
export CLANG_PATH="$PWD/clang"
export GCC64_PATH="$PWD/gcc64"
export GCC32_PATH="$PWD/gcc32"
export ARCH=arm64
export KBUILD_BUILD_USER="user-tobrut"
export KBUILD_BUILD_HOST="exotic-host"
export KBUILD_COMPILER_STRING="${CLANG_PATH}/clang"
export PATH="$CLANG_PATH/bin:$GCC64_PATH/bin:$GCC32_PATH/bin:$PATH"

# Clone Toolchains
mkdir -p clang && cd clang
curl -LO "https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman"
chmod +x antman
./antman -S
./antman --patch=glibc
cd ..

git clone https://github.com/greenforce-project/gcc-arm64 -b main --depth=1 gcc64
git clone https://github.com/greenforce-project/gcc-arm -b main --depth=1 gcc32

# Clone KernelSU-Next (susfs)
curl -LSs "https://raw.githubusercontent.com/Mr-Morat/KernelSU-Next/susfs/kernel/setup.sh" | bash -s susfs

# Start Build
make O=out ARCH=arm64 sweet_defconfig

make -j$(nproc --all) O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang \
  CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=$GCC64_PATH/bin/aarch64-elf- \
  CROSS_COMPILE_ARM32=$GCC32_PATH/bin/arm-eabi-

# Save config for reference
mv out/.config out/sweet_defconfig.txt

# Clone AnyKernel3 & Build Flashable ZIP
git clone --depth=1 https://github.com/Mr-Morat/anykernel3 -b main AnyKernel3
cp out/arch/arm64/boot/Image.gz AnyKernel3/Image.gz
cp out/arch/arm64/boot/dtbo.img AnyKernel3/dtbo.img
cp out/arch/arm64/boot/dtb.img AnyKernel3/dtb.img
cd AnyKernel3
zip -r "../TobrutExotic-AOSP-${BUILD_TIME}.zip" *
cd ..

echo -e "\n✅ Build completed: TobrutExotic-AOSP-${BUILD_TIME}.zip"

# OPTIONAL: Send to Telegram (if token & chat_id available in environment)
if [[ -n "$TELEGRAM_BOT_TOKEN" && -n "$TELEGRAM_CHAT_ID" ]]; then
  curl -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendDocument" \
    -F chat_id="${TELEGRAM_CHAT_ID}" \
    -F message_thread_id=120979 \
    -F document=@"TobrutExotic-AOSP-${BUILD_TIME}.zip" \
    -F caption=" Last Version is Cumming "
fi