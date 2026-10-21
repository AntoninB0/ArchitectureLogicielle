#!/usr/bin/env bash
# Compile / flashe une application Zephyr pour la Blue Pill (STM32F103C8).
#
#   ./build.sh <appli>                compile
#   ./build.sh <appli> clean          recompile depuis zéro
#   ./build.sh <appli> flash [port]   flashe TOUTES les cartes branchées (ou seulement [port])
#   ./build.sh monitor [log.txt]      affiche ce que toutes les cartes envoient (+ enregistre),
#                                     et envoie des commandes (voir tools/chaine.py)
#
# <appli> = dossier contenant prj.conf et src/*.c (+ include/*.h optionnel).
# Le CMakeLists.txt est créé automatiquement : tous les .c de src/ sont compilés.
#
# Flash : avant, mettre BOOT0 = 1 et appuyer sur RESET sur chaque carte à flasher.
#         Après : BOOT0 = 0 + RESET pour que le programme redémarre seul.
set -uo pipefail

NCS_VERSION=v3.4.1
NCS_DIR="$HOME/ncs/$NCS_VERSION"
BOARD=stm32_min_dev@blue
BAUD=${BAUD:-57600}   # vitesse du bootloader UART (115200 trop fragile sur breadboard)
EXTRA_MODULES="$HOME/zephyr-modules/hal_stm32"
ROOT=$(dirname "$(realpath "$0")")

usage() { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

[ $# -ge 1 ] || usage
if [ "$1" = monitor ]; then exec python3 "$ROOT/tools/chaine.py" ${2:+--log "$2"}; fi

APP=$(realpath "$1")
ACTION=${2:-build}
BUILD_DIR="$APP/build"
BIN="$BUILD_DIR/zephyr/zephyr.bin"

[ -d "$APP/src" ] || { echo "Erreur : pas de dossier src/ dans $APP"; exit 1; }

# CMakeLists.txt généré : l'utilisateur n'a jamais à y toucher
ensure_cmakelists() {
    local f="$APP/CMakeLists.txt"
    grep -q "tools/app.cmake" "$f" 2>/dev/null && return
    [ -s "$f" ] && { echo "Note : $f remplacé par la version générée (ancien -> CMakeLists.txt.old)"; mv "$f" "$f.old"; }
    cat > "$f" <<CM
# Généré par build.sh - ne pas modifier (voir tools/app.cmake)
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS \$ENV{ZEPHYR_BASE})
project($(basename "$APP"))
include($(realpath --relative-to="$APP" "$ROOT")/tools/app.cmake)
CM
}

build() {
    ensure_cmakelists
    touch "$APP/prj.conf"
    (cd "$NCS_DIR" && nrfutil sdk-manager toolchain launch --ncs-version "$NCS_VERSION" -- \
        west build --no-sysbuild -p "$1" -b "$BOARD" -d "$BUILD_DIR" "$APP" \
        -- -DEXTRA_ZEPHYR_MODULES="$EXTRA_MODULES")
}

# Flashe une carte : jusqu'à 3 essais (les adaptateurs PL2303 ratent parfois la synchro)
flash_one() {
    local port=$1 out
    for try in 1 2 3; do
        out=$(timeout 90 STM32_Programmer_CLI -c port="$port" br="$BAUD" \
              -w "$BIN" 0x08000000 -s 0x08000000 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
        if grep -q "Start operation achieved" <<<"$out"; then
            echo "  $port : OK, programme lancé"
            return 0
        fi
        if grep -q "Activating device: KO" <<<"$out"; then
            echo "  $port : la carte ne répond pas (BOOT0 = 1 ? RESET appuyé ?)"
            grep -E "Error|error" <<<"$out" | head -2 | sed 's/^/      /'
            return 1
        fi
        echo "  $port : essai $try raté, nouvel essai..."
        grep -E "Error|error" <<<"$out" | head -2 | sed 's/^/      /'
    done
    echo "  $port : ÉCHEC après 3 essais. Appuie sur RESET (BOOT0 = 1) et relance."
    grep -E "Error" <<<"$out" | head -3 | sed 's/^/      /'
    return 1
}

case "$ACTION" in
    build) build auto ;;
    clean) build always ;;
    flash)
        build auto || exit 1
        ports=("${@:3}")
        [ ${#ports[@]} -gt 0 ] || ports=(/dev/ttyUSB*)
        [ -e "${ports[0]}" ] || { echo "Aucun adaptateur USB-série branché"; exit 1; }
        echo "Flash de $(basename "$APP") sur : ${ports[*]}"
        rc=0
        for p in "${ports[@]}"; do flash_one "$p" || rc=1; done
        [ $rc = 0 ] && echo "Terminé. Pour garder le programme après un reset : BOOT0 = 0 puis RESET."
        exit $rc
        ;;
    *) usage ;;
esac
