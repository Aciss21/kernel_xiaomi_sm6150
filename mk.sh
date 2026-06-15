#!/bin/bash
# ================================================================
#   Kernel Build Script - courbet
#   By: esteh @ TUF-FA5093
# ================================================================

# ── Telegram Config ──────────────────────────────────────────────
TG_TOKEN="8647652050:AAG0ZKtMuE4NhlOKx8EHz4VHfgPLlguMTqw"
TG_CHAT_ID="7540957411"

# ── Build Config ─────────────────────────────────────────────────
DEVICE="courbet"
KNAME="Deandless-Road"
VAR="KSU"
ZIPNAME="${KNAME}-${VAR}.zip"
CLANG_DIR="/mnt/d/pt/kernel/linux-x86/clang+llvm-14.0.0-x86_64-linux-gnu-ubuntu-18.04/bin"
ANYKERNEL_REPO="https://github.com/ZGSYet/AnyKernel3"
ANYKERNEL_BRANCH="master"

# ── Output Paths ─────────────────────────────────────────────────
OUT="out"
KERNEL="$OUT/arch/arm64/boot/Image.gz"
DTBO="$OUT/arch/arm64/boot/dtbo.img"
DTB="$OUT/arch/arm64/boot/dtb.img"

# ── Exports ──────────────────────────────────────────────────────
export ARCH=arm64
export KBUILD_BUILD_USER=PryL
export KBUILD_BUILD_HOST=TUF-FA5093
export PATH="$CLANG_DIR:$PATH"

SECONDS=0
DATE=$(date +"%d %B %Y")
TIME_START=$(date +"%H:%M:%S")

# ── Telegram Functions ───────────────────────────────────────────
tg_send_sticky() {
    local res
    res=$(curl -s -X POST "https://api.telegram.org/bot${TG_TOKEN}/sendMessage" \
        -d chat_id="$TG_CHAT_ID" \
        -d text="$1" \
        -d parse_mode="Markdown")
    MESSAGE_ID=$(echo "$res" | grep -oP '(?<="message_id":)\d+')
}

tg_update() {
    curl -s -X POST "https://api.telegram.org/bot${TG_TOKEN}/editMessageText" \
        -d chat_id="$TG_CHAT_ID" \
        -d message_id="$MESSAGE_ID" \
        -d text="$1" \
        -d parse_mode="Markdown" > /dev/null
}

tg_send_file() {
    curl -s -F document=@"$1" \
         -F chat_id="$TG_CHAT_ID" \
         -F caption="$2" \
         -F parse_mode="Markdown" \
         "https://api.telegram.org/bot${TG_TOKEN}/sendDocument" > /dev/null
}

tg_send_log() {
    curl -s -F document=@"$1" \
         -F chat_id="$TG_CHAT_ID" \
         -F caption="$2" \
         -F parse_mode="Markdown" \
         "https://api.telegram.org/bot${TG_TOKEN}/sendDocument" > /dev/null
}

tg_send_photo() {
    curl -s -X POST "https://api.telegram.org/bot${TG_TOKEN}/sendPhoto" \
        -d chat_id="$TG_CHAT_ID" \
        -d photo="$1" \
        -d caption="$2" \
        -d parse_mode="Markdown" > /dev/null
}
# ── Header ───────────────────────────────────────────────────────
echo "================================================"
echo "   KUDA ASELI NAIL KUDA BESI - Kernel Builder  "
echo "================================================"
echo " Device  : $DEVICE"
echo " Builder : $KBUILD_BUILD_USER@$KBUILD_BUILD_HOST"
echo " Output  : $ZIPNAME"
echo "================================================"

tg_send_sticky "
╔══════════════════════════╗
       🚀 *KERNEL BUILD STARTED* 🚀
╚══════════════════════════╝

📱 *Device*    : \`$DEVICE\`
🏷️ *Kernel*    : \`$KNAME\`
🔧 *Variant*   : \`$VAR\`
👤 *Builder*   : \`$KBUILD_BUILD_USER\`
🖥️ *Host*      : \`$KBUILD_BUILD_HOST\`
📅 *Date*      : \`$DATE\`
🕐 *Started*   : \`$TIME_START\`

⏳ *Status*    : Warming up engines..."

# ── Step 1: Clean ────────────────────────────────────────────────
if [[ $1 == "-c" || $1 == "--clean" ]]; then
    echo "[1/4] Cleaning output..."
    tg_update "
╔══════════════════════════╗
       🚀 *KERNEL BUILD* 🚀
╚══════════════════════════╝

📱 *Device*  : \`$DEVICE\`
🏷️ *Kernel*  : \`$KNAME\`
📅 *Date*    : \`$DATE\`

🧹 *Step 1/4* — Cleaning...
\`\`\`
Nuking out/ folder...
\`\`\`
⏳ sabar su mung $(proc --all)..."
    rm -rf "$OUT"
fi

echo "Setup clang"

# ── Step 2: Defconfig ────────────────────────────────────────────
echo "[2/4] Loading defconfig..."
tg_update "
╔══════════════════════════╗
       🚀 *KERNEL BUILD* 🚀
╚══════════════════════════╝

📱 *Device*  : \`$DEVICE\`
🏷️ *Kernel*  : \`$KNAME\`
📅 *Date*    : \`$DATE\`

⚙️ *Step 2/4* — Loading Defconfig...
\`\`\`
Applying ${DEVICE}_defconfig
\`\`\`
⏳ delok konfigurasi kernel..."

make O="$OUT" ARCH=arm64 "${DEVICE}_defconfig"

# ── Step 3: Compile ──────────────────────────────────────────────
echo "[3/4] Compiling kernel..."
tg_update "
╔══════════════════════════╗
       🚀 *KERNEL BUILD* 🚀
╚══════════════════════════╝

📱 *Device*  : \`$DEVICE\`
🏷️ *Kernel*  : \`$KNAME\`
📅 *Date*    : \`$DATE\`

⚡ *Step 3/4* — Compiling...
\`\`\`
Threads  : $(nproc --all) cores
Compiler : Clang + GCC
\`\`\`
🔥 Gas poll compilenya, tenang ae cok"

make -j$(nproc --all) \
    O="$OUT" \
    ARCH=arm64 \
    LLVM=1 \
    LLVM_IAS=1 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
    2>&1 | tee build.log

# ── Cek Hasil Compile ────────────────────────────────────────────
if [ ! -f "$KERNEL" ]; then
    echo "❌ Build GAGAL! Cek build.log"
    tg_update "
╔══════════════════════════╗
             *BUILD GAGAL!* 
╚══════════════════════════╝

📱 *Device*  : \`$DEVICE\`
🏷️ *Kernel*  : \`$KNAME\`
📅 *Date*    : \`$DATE\`

❌ *Error*   : Image\.gz not found!
⏱️ *Waktu*   : \`$((SECONDS / 60)) menit $((SECONDS % 60)) detik\`

😭 Compile gagal bestie, cek lognya yuk..."
    tg_send_log "build.log" "
💀 *BUILD LOG — GAGAL*
━━━━━━━━━━━━━━━━━━━━
📱 *Device* : \`$DEVICE\`
🏷️ *Kernel* : \`$KNAME\`
⏱️ *Durasi* : \`$((SECONDS / 60)) menit $((SECONDS % 60)) detik\`
━━━━━━━━━━━━━━━━━━━━
Semangat, pasti bisa fix! 💪"
    exit 1
fi

# ── Step 4: Packaging ────────────────────────────────────────────
echo "[4/4] Packaging AnyKernel3..."
tg_update "
╔══════════════════════════╗
       🚀 *KERNEL BUILD* 🚀
╚══════════════════════════╝

📱 *Device*  : \`$DEVICE\`
🏷️ *Kernel*  : \`$KNAME\`
📅 *Date*    : \`$DATE\`

📦 *Step 4/4* — Packaging...
\`\`\`
Cloning AnyKernel3...
Copying Image.gz + DTB...
Zipping flashable package...
\`\`\`
🎁 Hampir selesai, sabar dikit lagi!"

[ -d "AnyKernel3" ] && rm -rf AnyKernel3
git clone -q "$ANYKERNEL_REPO" -b "$ANYKERNEL_BRANCH" AnyKernel3

sed -i "s/device\.name1=.*/device.name1=${DEVICE}/"       AnyKernel3/anykernel.sh
sed -i "s/device\.name2=.*/device.name2=${DEVICE}in/"     AnyKernel3/anykernel.sh
sed -i "s/kernel\.string=.*/kernel.string=${KNAME}/"      AnyKernel3/anykernel.sh

cp "$KERNEL" AnyKernel3/
[ -f "$DTBO" ] && cp "$DTBO" AnyKernel3/
[ -f "$DTB"  ] && cp "$DTB"  AnyKernel3/

cd AnyKernel3
zip -r9 "../$ZIPNAME" * -x "*.git*" -x "*.github*"
cd ..
rm -rf AnyKernel3

# ── Step 5: Upload ───────────────────────────────────────────────
DURATION="$((SECONDS / 60)) menit $((SECONDS % 60)) detik"
TIME_END=$(date +"%H:%M:%S")
echo "✅ dadi $DURATION"

tg_update "
╔══════════════════════════╗
       🚀 *KERNEL BUILD* 🚀
╚══════════════════════════╝

📱 *Device*  : \`$DEVICE\`
🏷️ *Kernel*  : \`$KNAME\`

✅ *Sukses!* Lagi upload zip...
📤 Mohon tunggu sebentar..."

tg_send_file "$ZIPNAME" "
╔══════════════════════════╗
    ✅ *KARI MBADOK* ✅
╚══════════════════════════╝

📱 *Device*   : \`$DEVICE\`
🏷️ *Kernel*   : \`$KNAME\`
🔧 *Variant*  : \`$VAR\`
📦 *File*     : \`$ZIPNAME\`

🕐 *Start*    : \`$TIME_START\`
🕑 *Finish*   : \`$TIME_END\`
⏱️ *Durasi*   : \`$DURATION\`

👤 *Builder*  : \`$KBUILD_BUILD_USER\`
🖥️ *Host*     : \`$KBUILD_BUILD_HOST\`
📅 *Date*     : \`$DATE\`

_Alhamdulillah, action di perbudak_ 🙏"

tg_update "
tg_send_photo "
╔══════════════════════════╗
    ✅ *BUILD SELESAI!* ✅
╚══════════════════════════╝

📦 *File*    : \`$ZIPNAME\`
⏱️ *Durasi*  : \`$DURATION\`
📅 *Date*    : \`$DATE\`

echo "================================================"
echo " Selesai dalam $DURATION"
echo "================================================"