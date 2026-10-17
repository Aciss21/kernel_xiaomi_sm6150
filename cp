#!/bin/bash
# ================================================================
#   Kernel Build Script - courbet
#   By: Aciss
# ================================================================

# ── Project Directory ────────────────────────────────────────────
KERNEL_DIR="$(pwd)"

# ── Telegram Config ──────────────────────────────────────────────
TG_ENV_FILE="${TG_ENV_FILE:-usr/.tg.env}"

if [ -f "$TG_ENV_FILE" ]; then
    # shellcheck disable=SC1090
    source "$TG_ENV_FILE"
fi

if [ -z "$TG_TOKEN" ] || [ -z "$TG_CHAT_ID" ]; then
    echo "TG_TOKEN / TG_CHAT_ID belum di-set."
    echo
    echo "Buat file: $TG_ENV_FILE"
    echo
    echo 'TG_TOKEN="123456:ABC-your-bot-token"'
    echo 'TG_CHAT_ID="123456789"'
    exit 1
fi

# ── Build Config ─────────────────────────────────────────────────
DEVICE="courbet"
KNAME="shirayuki"
VAR="oem"

ZIPNAME="${KNAME}-${VAR}.zip"

ANYKERNEL_REPO="https://github.com/Aciss21/AnyKernel3.git"
ANYKERNEL_BRANCH="master"

# ── Output Paths ─────────────────────────────────────────────────
OUT="$KERNEL_DIR/out"

KERNEL="$OUT/arch/arm64/boot/Image.gz"
DTBO="$OUT/arch/arm64/boot/dtbo.img"
DTB="$OUT/arch/arm64/boot/dtb.img"

BUILD_LOG="$KERNEL_DIR/build.log"

# ── Exports ──────────────────────────────────────────────────────
export ARCH=arm64

export KBUILD_BUILD_USER="Aciss"
export KBUILD_BUILD_HOST="TUF-FA5093"

SECONDS=0

DATE=$(date +"%d %B %Y")
TIME_START=$(date +"%H:%M:%S")

# ── Telegram Functions ───────────────────────────────────────────
#
# Semua pesan Telegram menggunakan <pre>
# agar tampil monospace.
#

tg_send_sticky() {
    local res

    res=$(curl -s -X POST \
        "https://api.telegram.org/bot${TG_TOKEN}/sendMessage" \
        -d "chat_id=${TG_CHAT_ID}" \
        --data-urlencode "text=<pre>$1</pre>" \
        -d "parse_mode=HTML")

    MESSAGE_ID=$(echo "$res" | grep -oP '(?<="message_id":)\d+' | head -1)

    if [ -z "$MESSAGE_ID" ]; then
        echo "Gagal mendapatkan Telegram message ID."
    fi
}

tg_update() {
    if [ -z "$MESSAGE_ID" ]; then
        return
    fi

    curl -s -X POST \
        "https://api.telegram.org/bot${TG_TOKEN}/editMessageText" \
        -d "chat_id=${TG_CHAT_ID}" \
        -d "message_id=${MESSAGE_ID}" \
        --data-urlencode "text=<pre>$1</pre>" \
        -d "parse_mode=HTML" \
        > /dev/null
}

tg_send_file() {
    curl -s \
        -F "document=@$1" \
        -F "chat_id=$TG_CHAT_ID" \
        --form-string "caption=<pre>$2</pre>" \
        -F "parse_mode=HTML" \
        "https://api.telegram.org/bot${TG_TOKEN}/sendDocument" \
        > /dev/null
}

tg_send_log() {
    curl -s \
        -F "document=@$1" \
        -F "chat_id=$TG_CHAT_ID" \
        --form-string "caption=<pre>$2</pre>" \
        -F "parse_mode=HTML" \
        "https://api.telegram.org/bot${TG_TOKEN}/sendDocument" \
        > /dev/null
}

# ── Header ───────────────────────────────────────────────────────

echo "================================================"
echo "       KUDA ASELI NAIL KUDA BESI"
echo "================================================"
echo " Device  : $DEVICE"
echo " Kernel  : $KNAME"
echo " Variant : $VAR"
echo " Builder : $KBUILD_BUILD_USER@$KBUILD_BUILD_HOST"
echo " Output  : $ZIPNAME"
echo "================================================"

# ── Telegram Build Start ─────────────────────────────────────────

tg_send_sticky "Kernel Build Started

Device  : $DEVICE
Kernel  : $KNAME
Variant : $VAR

Builder : $KBUILD_BUILD_USER
Date    : $DATE
Start   : $TIME_START

Status  : Starting"

# Pastikan selalu kembali ke root kernel
cd "$KERNEL_DIR" || exit 1

# ── Toolchain: Neutron Clang + GCC ───────────────────────────────
#
# Neutron Clang 10032024
# GCC64: greenforce gcc-arm64
# GCC32: greenforce gcc-arm
#
CLANG_DIR="$KERNEL_DIR/clang"
GCC64_DIR="$KERNEL_DIR/gcc64"
GCC32_DIR="$KERNEL_DIR/gcc32"

NEUTRON_TARBALL="neutron-clang-10032024.tar.zst"
NEUTRON_URL="https://github.com/Neutron-Toolchains/clang-build-catalogue/releases/download/10032024/neutron-clang-10032024.tar.zst"

GCC64_REPO="https://github.com/greenforce-project/gcc-arm64"
GCC32_REPO="https://github.com/greenforce-project/gcc-arm"

# ── Download Neutron Clang ───────────────────────────────────────

if [ ! -x "$CLANG_DIR/bin/clang" ]; then

    echo "[0/4] Setting up Neutron Clang..."

    tg_update "Kernel Build

Device : $DEVICE
Kernel : $KNAME

[0/4] Toolchain

Clang  : Neutron 10032024
Status : Downloading"

    rm -rf "$CLANG_DIR"
    mkdir -p "$CLANG_DIR"

    curl -fL -o "$KERNEL_DIR/$NEUTRON_TARBALL" "$NEUTRON_URL"

    if [ $? -eq 0 ]; then
        tar --use-compress-program=zstd -xf "$KERNEL_DIR/$NEUTRON_TARBALL" -C "$CLANG_DIR"
        rm -f "$KERNEL_DIR/$NEUTRON_TARBALL"
    fi

    if [ ! -x "$CLANG_DIR/bin/clang" ]; then

        DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"

        echo
        echo "Neutron Clang setup failed."

        tg_update "Kernel Build Failed

Device : $DEVICE
Kernel : $KNAME

Error  : Neutron Clang setup failed
Time   : $DURATION"

        exit 1
    fi
fi

# ── Clone GCC64 ──────────────────────────────────────────────────

if [ ! -d "$GCC64_DIR" ]; then

    echo "[0/4] Cloning GCC64..."

    tg_update "Kernel Build

Device : $DEVICE
Kernel : $KNAME

[0/4] Toolchain

GCC64  : greenforce gcc-arm64
Status : Cloning"

    git clone --depth=1 "$GCC64_REPO" "$GCC64_DIR"

    if [ ! -d "$GCC64_DIR" ]; then

        DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"

        echo
        echo "GCC64 clone failed."

        tg_update "Kernel Build Failed

Device : $DEVICE
Kernel : $KNAME

Error  : GCC64 clone failed
Time   : $DURATION"

        exit 1
    fi
fi

# ── Clone GCC32 ──────────────────────────────────────────────────

if [ ! -d "$GCC32_DIR" ]; then

    echo "[0/4] Cloning GCC32..."

    tg_update "Kernel Build

Device : $DEVICE
Kernel : $KNAME

[0/4] Toolchain

GCC32  : greenforce gcc-arm
Status : Cloning"

    git clone --depth=1 "$GCC32_REPO" "$GCC32_DIR"

    if [ ! -d "$GCC32_DIR" ]; then

        DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"

        echo
        echo "GCC32 clone failed."

        tg_update "Kernel Build Failed

Device : $DEVICE
Kernel : $KNAME

Error  : GCC32 clone failed
Time   : $DURATION"

        exit 1
    fi
fi

# ── Set Toolchain PATH ───────────────────────────────────────────

export CLANG_PATH="$CLANG_DIR"
export PATH="$CLANG_DIR/bin:$GCC64_DIR/bin:$GCC32_DIR/bin:$PATH"

echo "Clang  : $("$CLANG_DIR/bin/clang" --version | head -n1)"
echo "GCC64  : $(aarch64-elf-gcc --version | head -n1)"
echo "GCC32  : $(arm-eabi-gcc --version | head -n1)"

# ── Step 1: Clean ────────────────────────────────────────────────

if [[ "$1" == "-c" || "$1" == "--clean" ]]; then

    echo "[1/4] Cleaning output..."

    tg_update "Kernel Build

Device : $DEVICE
Kernel : $KNAME

[1/4] Cleaning output

Status  : Removing out"

    rm -rf "$OUT"

fi

# ── Step 2: Defconfig ────────────────────────────────────────────

echo "[2/4] Loading defconfig..."

echo
echo "Working directory:"
pwd

echo
echo "Defconfig:"
echo "arch/arm64/configs/vendor/courbet_defconfig"
echo

tg_update "Kernel Build

Device : $DEVICE
Kernel : $KNAME

[2/4] Defconfig

Config  : vendor/courbet_defconfig
Status  : Loading"

make O="$OUT" \
    ARCH=arm64 \
    CC=clang \
    CROSS_COMPILE=aarch64-linux-gnu- \
    vendor/courbet_defconfig

DEFCONFIG_STATUS=$?

if [ "$DEFCONFIG_STATUS" -ne 0 ]; then

    DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"

    echo
    echo "Defconfig failed."
    echo

    tg_update "Kernel Build Failed

Device : $DEVICE
Kernel : $KNAME

Error  : Defconfig failed
Time   : $DURATION"

    exit 1
fi

echo "Defconfig loaded successfully."

# ── Step 3: Compile ──────────────────────────────────────────────

echo "[3/4] Compiling kernel..."

THREADS=$(nproc --all)

tg_update "Kernel Build

Device : $DEVICE
Kernel : $KNAME

[3/4] Compile

Threads : $THREADS
Compiler: Neutron Clang + GCC
IAS     : LLVM Integrated Assembler

Status  : Building"

rm -f "$BUILD_LOG"

make -j"$THREADS" \
    O="$OUT" \
    ARCH=arm64 \
    CC=clang \
    CLANG_TRIPLE=aarch64-linux-gnu- \
    CROSS_COMPILE=aarch64-elf- \
    CROSS_COMPILE_ARM32=arm-eabi- \
    AR=llvm-ar \
    NM=llvm-nm \
    OBJCOPY=llvm-objcopy \
    OBJDUMP=llvm-objdump \
    STRIP=llvm-strip \
    READELF=llvm-readelf \
    OBJSIZE=llvm-size \
    HOSTCC=clang \
    HOSTCXX=clang++ \
    HOSTAR=llvm-ar \
    HOSTLD=ld.lld \
    LD=ld.lld \
    2>&1 | tee "$BUILD_LOG"

BUILD_STATUS=${PIPESTATUS[0]}

# ── Check Build ──────────────────────────────────────────────────

if [ "$BUILD_STATUS" -ne 0 ] || [ ! -f "$KERNEL" ]; then

    DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"

    echo
    echo "Build failed."
    echo "Check: $BUILD_LOG"

    tg_update "Kernel Build Failed

Device : $DEVICE
Kernel : $KNAME

Error  : Kernel compilation failed
Time   : $DURATION

Status : Sending build log"

    if [ -f "$BUILD_LOG" ]; then

        tg_send_log "$BUILD_LOG" \
"Kernel Build Log

Device   : $DEVICE
Kernel   : $KNAME
Variant  : $VAR
Duration : $DURATION"

    fi

    exit 1
fi

echo
echo "Kernel compiled successfully."
echo "Image: $KERNEL"

# ── Step 4: Packaging ────────────────────────────────────────────

echo "[4/4] Packaging AnyKernel3..."

tg_update "Kernel Build

Device : $DEVICE
Kernel : $KNAME

[4/4] Packaging

Status  : Creating ZIP"

# ── Remove Old AnyKernel ─────────────────────────────────────────

if [ -d "$KERNEL_DIR/AnyKernel3" ]; then
    rm -rf "$KERNEL_DIR/AnyKernel3"
fi

# ── Clone AnyKernel3 ─────────────────────────────────────────────

git clone -q \
    "$ANYKERNEL_REPO" \
    -b "$ANYKERNEL_BRANCH" \
    "$KERNEL_DIR/AnyKernel3"

AK_STATUS=$?

if [ "$AK_STATUS" -ne 0 ]; then

    DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"

    tg_update "Kernel Build Failed

Device : $DEVICE
Kernel : $KNAME

Error  : AnyKernel3 clone failed
Time   : $DURATION"

    exit 1
fi

# ── Configure AnyKernel3 ─────────────────────────────────────────

sed -i \
    "s/device\.name1=.*/device.name1=courbet/" \
    "$KERNEL_DIR/AnyKernel3/anykernel.sh"

sed -i \
    "s/device\.name2=.*/device.name2=courbetin/" \
    "$KERNEL_DIR/AnyKernel3/anykernel.sh"

# ── Copy Kernel ──────────────────────────────────────────────────

cp "$KERNEL" \
    "$KERNEL_DIR/AnyKernel3/"

# ── Copy DTBO ────────────────────────────────────────────────────

if [ -f "$DTBO" ]; then
    cp "$DTBO" "$KERNEL_DIR/AnyKernel3/"
fi

# ── Copy DTB ─────────────────────────────────────────────────────

if [ -f "$DTB" ]; then
    cp "$DTB" "$KERNEL_DIR/AnyKernel3/"
fi

# ── Create ZIP ───────────────────────────────────────────────────

cd "$KERNEL_DIR/AnyKernel3" || exit 1

zip -r9 \
    "$KERNEL_DIR/$ZIPNAME" \
    * \
    -x "*.git*" \
    -x "*.github*" \
    > /dev/null

ZIP_STATUS=$?

cd "$KERNEL_DIR" || exit 1

rm -rf "$KERNEL_DIR/AnyKernel3"

if [ "$ZIP_STATUS" -ne 0 ] || [ ! -f "$KERNEL_DIR/$ZIPNAME" ]; then

    DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"

    tg_update "Kernel Build Failed

Device : $DEVICE
Kernel : $KNAME

Error  : ZIP creation failed
Time   : $DURATION"

    exit 1
fi

# ── Upload ────────────────────────────────────────────────────────

DURATION="$((SECONDS / 60))m $((SECONDS % 60))s"
TIME_END=$(date +"%H:%M:%S")

echo
echo "Build completed."
echo "Duration: $DURATION"

tg_update "Build Successful

Device : $DEVICE
Kernel : $KNAME
Variant : $VAR

File    : $ZIPNAME
Duration: $DURATION

Status  : Uploading"

# ── Send ZIP ─────────────────────────────────────────────────────

tg_send_file "$KERNEL_DIR/$ZIPNAME" \
"Kernel Build Complete

Device : $DEVICE
Kernel : $KNAME
Variant: $VAR

File     : $ZIPNAME
Start    : $TIME_START
Finish   : $TIME_END
Duration : $DURATION

Builder  : $KBUILD_BUILD_USER
Host     : $KBUILD_BUILD_HOST"

# ── Done ─────────────────────────────────────────────────────────

echo
echo "================================================"
echo " Build Complete"
echo "================================================"
echo " Device  : $DEVICE"
echo " Kernel  : $KNAME"
echo " Variant : $VAR"
echo " Output  : $ZIPNAME"
echo " Duration: $DURATION"
echo "================================================"

