#!/usr/bin/env bash
# Build and boot-verify all custom DTBs, drivers, and kernel image in the woven tree.
set -euo pipefail

SRC="${1:-$(git rev-parse --show-toplevel)}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/linux-loom-verify-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

export HOME="$WORK/home"
mkdir -p "$HOME"
JOBS="${NIX_BUILD_CORES:-$(nproc)}"

if [ ! -f "$SRC/arch/arm64/boot/dts/sophgo/sg2000-pine64-oz64.dts" ]; then
  echo "Source tree ($SRC) does not contain woven feature branches (likely bare local-ci); skipping full kernel build."
  exit 0
fi

echo "=== [1/4] Building and verifying custom arm64 DTBs ==="
O_ARM64="$WORK/build-arm64"
mkdir -p "$O_ARM64"
make -C "$SRC" O="$O_ARM64" ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu- defconfig
"$SRC/scripts/config" --file "$O_ARM64/.config" \
  -e ARCH_SOPHGO \
  -e ARCH_SUNXI \
  -e ARCH_ROCKCHIP \
  -e CLK_SOPHGO_CV1800 \
  -e CV1800_THERMAL \
  -e SND_SOC \
  -e SND_SOC_CV1800B_DAC \
  -e SND_SOC_CV1800B_TDM \
  -e MMC_SDHCI \
  -e MMC_SDHCI_PLTFM \
  -e MMC_SDHCI_OF_DWCMSHC \
  -e DRM \
  -e DRM_SUN4I \
  -e DRM_SUN8I_DW_HDMI \
  -e DRM_SUN8I_MIXER \
  -e DRM_SUN8I_TCON_TOP \
  -e DRM_PANEL_MIPI \
  -e SUN8I_DE2_CCU \
  -e PWM_SUN8I \
  -e PINCTRL_SUN50I_H616 \
  -e PHY_SUN4I_USB \
  -e USB_MUSB_HDRC \
  -e USB_MUSB_SUNXI \
  -e SUNXI_WATCHDOG \
  -e INPUT_JOYSTICK \
  -e JOYSTICK_ADC \
  -e IIO \
  -e ROCKCHIP_IOMMU \
  -e MEDIA_SUPPORT \
  -e MEDIA_PLATFORM_SUPPORT \
  -e MEDIA_PLATFORM_DRIVERS \
  -e V4L_MEM2MEM_DRIVERS \
  -e VIDEO_DEV \
  -e VIDEO_ROCKCHIP_RKVENC
make -C "$SRC" O="$O_ARM64" ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu- olddefconfig

make -C "$SRC" O="$O_ARM64" ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu- -j"$JOBS" \
  sophgo/sg2000-pine64-oz64.dtb \
  allwinner/sun50i-h700-anbernic-rg40xx-h.dtb \
  allwinner/sun50i-h700-anbernic-rg35xx-2024.dtb \
  rockchip/rk3588-quartzpro64.dtb \
  rockchip/rk3399-pinephone-pro.dtb

fdtget "$O_ARM64/arch/arm64/boot/dts/sophgo/sg2000-pine64-oz64.dtb" / compatible | grep -q "pine64,oz64"
fdtget "$O_ARM64/arch/arm64/boot/dts/allwinner/sun50i-h700-anbernic-rg40xx-h.dtb" / compatible | grep -q "anbernic,rg40xx-h"
fdtget "$O_ARM64/arch/arm64/boot/dts/allwinner/sun50i-h700-anbernic-rg35xx-2024.dtb" / compatible | grep -q "anbernic,rg35xx-2024"
fdtget "$O_ARM64/arch/arm64/boot/dts/rockchip/rk3588-quartzpro64.dtb" / compatible | grep -q "pine64,quartzpro64"
fdtget "$O_ARM64/arch/arm64/boot/dts/rockchip/rk3399-pinephone-pro.dtb" /firmware/optee compatible | grep -q "linaro,optee-tz"

echo "=== [2/4] Compiling all custom arm64 drivers ==="
make -C "$SRC" O="$O_ARM64" ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu- -j"$JOBS" \
  KCFLAGS="-Werror" \
  drivers/clk/sophgo/ \
  drivers/thermal/cv1800_thermal.o \
  sound/soc/sophgo/ \
  drivers/mmc/host/sdhci-of-dwcmshc.o \
  drivers/clk/sunxi-ng/ccu-sun8i-de2.o \
  drivers/gpu/drm/sun4i/ \
  drivers/gpu/drm/panel/panel-mipi.o \
  drivers/pwm/pwm-sun8i.o \
  drivers/pinctrl/sunxi/pinctrl-sun50i-h616.o \
  drivers/phy/allwinner/phy-sun4i-usb.o \
  drivers/usb/musb/ \
  drivers/watchdog/sunxi_wdt.o \
  drivers/input/joystick/adc-joystick.o \
  drivers/iommu/rockchip-iommu.o \
  drivers/media/platform/rockchip/rkvenc/
rm -rf "$O_ARM64"

echo "=== [3/4] Building SigmaStar SSC335 C212 arm kernel & DTB ==="
O_ARM32="$WORK/build-arm32"
mkdir -p "$O_ARM32"
make -C "$SRC" O="$O_ARM32" ARCH=arm CROSS_COMPILE=armv7l-unknown-linux-gnueabihf- ssc335_c212_defconfig
"$SRC/scripts/config" --file "$O_ARM32/.config" \
  -e ARCH_VIRT \
  -e ARCH_MVEBU \
  -e MACH_ARMADA_370 \
  -e CPU_PJ4B \
  -e CPU_IDLE \
  -e ARM_MVEBU_V7_CPUIDLE \
  -e SERIAL_AMBA_PL011 \
  -e SERIAL_AMBA_PL011_CONSOLE \
  -d ARM_APPENDED_DTB
make -C "$SRC" O="$O_ARM32" ARCH=arm CROSS_COMPILE=armv7l-unknown-linux-gnueabihf- olddefconfig
make -C "$SRC" O="$O_ARM32" ARCH=arm CROSS_COMPILE=armv7l-unknown-linux-gnueabihf- -j"$JOBS" \
  KCFLAGS="-Werror" \
  zImage \
  sigmastar/mstar-infinity6b0-ssc335-tapo-c212-v2.dtb

fdtget "$O_ARM32/arch/arm/boot/dts/sigmastar/mstar-infinity6b0-ssc335-tapo-c212-v2.dtb" / compatible | grep -q "sstar,infinity6b0"

echo "=== [4/4] Running QEMU boot smoke test on woven kernel ==="
cat > "$WORK/init.c" << 'EOF'
int main(void) {
    struct utsname u;
    uname(&u);
    printf("LOOM_BOOT_OK %s %s\n", u.release, u.machine);
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    return 0;
}
EOF
armv7l-unknown-linux-gnueabihf-gcc -Os -static -fno-ident -fno-asynchronous-unwind-tables -nostdlib \
  -include "$SRC/tools/include/nolibc/nolibc.h" \
  -o "$WORK/init" "$WORK/init.c" -lgcc
mkdir -p "$WORK/initramfs/dev"
cp "$WORK/init" "$WORK/initramfs/init"
chmod +x "$WORK/initramfs/init"
(cd "$WORK/initramfs" && find . | cpio -o -H newc --quiet | gzip -9 > "$WORK/initramfs.cpio.gz")

BOOT_LOG="${BOOT_LOG_OUT:-$WORK/qemu-boot.log}"
timeout 60 qemu-system-arm \
  -M virt -cpu cortex-a7 -m 256M \
  -display none -serial "file:$BOOT_LOG" -no-reboot \
  -kernel "$O_ARM32/arch/arm/boot/zImage" \
  -initrd "$WORK/initramfs.cpio.gz" \
  -append "console=ttyAMA0 rdinit=/init panic=-1"

cat "$BOOT_LOG"
grep -q "LOOM_BOOT_OK 7.4" "$BOOT_LOG"
echo "ALL KERNEL BUILD AND QEMU BOOT CHECKS PASSED"
