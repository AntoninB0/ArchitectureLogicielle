#!/usr/bin/env bash
# Compile / flashe une application Zephyr pour la Blue Pill (STM32F103C8).
#
#   ./build.sh <appli>                  compile
#   ./build.sh <appli> clean            recompile depuis zéro
#   ./build.sh <appli> flash [port]     flashe par le bootloader série
#                                       (BOOT0 = 1 puis RESET avant ; port par défaut /dev/ttyUSB0)
#   ./build.sh <appli> monitor [port]   affiche ce que la carte envoie sur A9 (Ctrl-A Ctrl-X pour quitter)
#
# <appli> = dossier contenant CMakeLists.txt, prj.conf, src/...
set -euo pipefail

NCS_VERSION=v3.4.1
NCS_DIR="$HOME/ncs/$NCS_VERSION"
BOARD=stm32_min_dev@blue
EXTRA_MODULES="$HOME/zephyr-modules/hal_stm32"

usage() { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

[ $# -ge 1 ] || usage
APP=$(realpath "$1")
ACTION=${2:-build}
PORT=${3:-/dev/ttyUSB0}
BUILD_DIR="$APP/build"
BIN="$BUILD_DIR/zephyr/zephyr.bin"

[ -f "$APP/CMakeLists.txt" ] || { echo "Erreur : pas de CMakeLists.txt dans $APP"; exit 1; }

build() {
    local pristine=$1
    cd "$NCS_DIR"
    nrfutil sdk-manager toolchain launch --ncs-version "$NCS_VERSION" -- \
        west build --no-sysbuild -p "$pristine" -b "$BOARD" -d "$BUILD_DIR" "$APP" \
        -- -DEXTRA_ZEPHYR_MODULES="$EXTRA_MODULES"
}

case "$ACTION" in
    build)   build auto ;;
    clean)   build always ;;
    flash)
        [ -f "$BIN" ] || build auto
        STM32_Programmer_CLI -c port="$PORT" br=115200 -w "$BIN" 0x08000000 -v -s 0x08000000
        ;;
    monitor) picocom -b 115200 "$PORT" ;;
    *)       usage ;;
esac
