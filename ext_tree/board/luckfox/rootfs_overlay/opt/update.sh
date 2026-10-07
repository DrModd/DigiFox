#!/bin/sh
#
# DigiFox online update from GitHub Releases (DrModd/DigiFox, release "latest").
#
#   /opt/update.sh           update if a newer build is published
#   /opt/update.sh --force   reinstall even if the build is the same
#   /opt/update.sh --check   only print the installed and the latest build
#
# How it works (same idea as the PureFox updater, without its server):
#   1. download version.txt, SHA256SUMS, rootfs.squashfs and boot.img to /tmp (RAM)
#   2. check SHA-256
#   3. loop-mount rootfs.squashfs and rsync it over / (each file is replaced
#      atomically by rename; user settings are protected, see FILTERS)
#   4. flash the kernel part of boot.img only if it changed, then put back
#      the DTB of the current I2S mode (PLL / EXT 1024 / EXT 512)
#   5. reboot
# idblock, U-Boot and the U-Boot environment are never touched online:
# for those flash the full image with RV1106_Toolkit.

REPO="DrModd/DigiFox"
BASE="https://github.com/$REPO/releases/latest/download"
WORK=/tmp/digifox-update
NEW=$WORK/root
BOOT_MTD=/dev/mtd3
DTB_OFFSET=3932160          # 0x3C0000, as in post-build.sh and /opt/2*.sh
CUR_BUILD=$(cat /etc/digifox-release 2>/dev/null || echo unknown)

log()  { echo "$*"; }
fail() { echo "ОШИБКА: $*"; cleanup; exit 1; }

cleanup() {
    grep -q " $NEW " /proc/mounts 2>/dev/null && umount "$NEW"
    rm -rf "$WORK"
}

fetch() {   # fetch <name> <out>
    wget -q -T 60 -O "$2" "$BASE/$1" 2>/dev/null
}

trap cleanup INT TERM

FORCE=0
case "$1" in
    --force) FORCE=1 ;;
    --check)
        log "Установлена: $CUR_BUILD"
        log "Последняя:   $(wget -q -T 20 -O - "$BASE/version.txt" 2>/dev/null || echo 'нет связи с GitHub')"
        exit 0 ;;
esac

rm -rf "$WORK"; mkdir -p "$NEW" || fail "не создать $WORK"

log "Установлена сборка: $CUR_BUILD"
log "Проверяю GitHub ($REPO)..."
fetch version.txt "$WORK/version.txt" || fail "нет связи с GitHub"
NEW_BUILD=$(tr -d '\r\n' < "$WORK/version.txt")
log "Последняя сборка:   $NEW_BUILD"
if [ "$NEW_BUILD" = "$CUR_BUILD" ] && [ $FORCE -eq 0 ]; then
    log "Обновление не требуется."
    cleanup; exit 0
fi

fetch SHA256SUMS "$WORK/SHA256SUMS" || fail "нет SHA256SUMS"
need=$(awk '$2=="rootfs.squashfs"{print 1}' "$WORK/SHA256SUMS")
[ "$need" = 1 ] || fail "в релизе нет rootfs.squashfs"

# место в RAM: образ ~40-60 МБ + boot.img 4 МБ
avail=$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo)
log "Свободно памяти: ${avail} МБ"
[ "${avail:-0}" -ge 90 ] || fail "мало свободной памяти (нужно ~90 МБ), перезагрузите Фокс и повторите"

log "Скачиваю образ..."
fetch rootfs.squashfs "$WORK/rootfs.squashfs" || fail "не скачался rootfs.squashfs"
fetch boot.img "$WORK/boot.img" || fail "не скачался boot.img"
log "Проверяю контрольные суммы..."
( cd "$WORK" && grep -E ' (rootfs\.squashfs|boot\.img)$' SHA256SUMS | sha256sum -c - ) || fail "контрольная сумма не совпала"

mount -t squashfs -o loop,ro "$WORK/rootfs.squashfs" "$NEW" || fail "не смонтировать образ"
[ -x "$NEW/usr/bin/pfctl" ] && [ -f "$NEW/etc/digifox-release" ] || fail "образ не похож на DigiFox"

# ---- остановить плееры, сохранить настройки I2S ----
log "Останавливаю плееры..."
for s in /etc/init.d/S95*; do [ -x "$s" ] && "$s" stop >/dev/null 2>&1; done
cp /etc/i2s.conf "$WORK/i2s.conf" 2>/dev/null

# ---- файлы ----
log "Обновляю файлы системы..."
rsync -ac --delete-before \
    --exclude=/dev --exclude=/proc --exclude=/sys --exclude=/tmp --exclude=/run \
    --exclude=/mnt --exclude=/media --exclude=/root --exclude=/var/run --exclude=/var/log \
    --filter='protect /etc/i2s.conf' \
    --filter='protect /etc/asound.conf' \
    --filter='protect /etc/output' \
    --filter='protect /etc/usb_to_i2s.state' \
    --filter='protect /etc/init.d/S95*' \
    --filter='protect /etc/shadow' \
    --filter='protect /etc/digifox/' \
    --filter='protect /var/lib/digifox/' \
    --filter='protect /etc/resolv.conf' \
    --filter='protect /etc/dropbear/dropbear_*_host_key' \
    --filter='protect /data/ethaddr.txt' \
    --filter='protect /var/www/radio.json' \
    --filter='protect /usr/aplayer/*.dat' \
    --filter='protect /usr/aprenderer/*.dat' \
    "$NEW/" / || fail "rsync завершился с ошибкой — система могла обновиться частично, повторите обновление"

# настройки I2S: файл конфигурации сохранён, S94ioi2s пришёл новый (PLL) — привести в соответствие
[ -f "$WORK/i2s.conf" ] && cp "$WORK/i2s.conf" /etc/i2s.conf
MODE=$(sed -n 's/^MODE=//p' /etc/i2s.conf 2>/dev/null)
MCLK=$(sed -n 's/^MCLK=//p' /etc/i2s.conf 2>/dev/null)
[ "$MODE" = ext ] && sed -i 's/007c003c/007c001c/' /etc/init.d/S94ioi2s

umount "$NEW"
sync

# ---- ядро ----
dd if="$BOOT_MTD" of="$WORK/cur_kernel" bs=65536 count=$((DTB_OFFSET / 65536)) 2>/dev/null
dd if="$WORK/boot.img" of="$WORK/new_kernel" bs=65536 count=$((DTB_OFFSET / 65536)) 2>/dev/null
if cmp -s "$WORK/cur_kernel" "$WORK/new_kernel"; then
    log "Ядро не изменилось."
else
    log "Записываю новое ядро (boot.img)... Не выключайте питание!"
    size=$(wc -c < "$WORK/boot.img")
    flash_erase "$BOOT_MTD" 0 0 >/dev/null || fail "flash_erase boot — восстановите прошивку через RV1106_Toolkit"
    mtd_debug write "$BOOT_MTD" 0 "$size" "$WORK/boot.img" >/dev/null || fail "запись boot — восстановите прошивку через RV1106_Toolkit"
fi

# ---- DTB текущего режима I2S ----
case "$MODE:$MCLK" in
    ext:512) DTB=/data/boot/512_ext.dtb ;;
    ext:*)   DTB=/data/boot/1024_ext.dtb ;;
    *)       DTB=/data/boot/1024_pll.dtb ;;
esac
if [ -f "$DTB" ]; then
    n=$(wc -c < "$DTB")
    dd if="$BOOT_MTD" bs=65536 skip=$((DTB_OFFSET / 65536)) count=4 2>/dev/null | head -c "$n" > "$WORK/cur_dtb"
    if ! cmp -s "$WORK/cur_dtb" "$DTB"; then
        log "Записываю DTB для режима I2S ($MODE, MCLK $MCLK)..."
        flash_erase "$BOOT_MTD" $DTB_OFFSET 2 >/dev/null && \
            nandwrite -p -q "$BOOT_MTD" -s $DTB_OFFSET "$DTB" || fail "запись DTB"
    fi
fi
sync

log "Готово: $CUR_BUILD -> $NEW_BUILD"
log "Перезагрузка через 5 секунд..."
cleanup
( sleep 5; reboot ) >/dev/null 2>&1 &
exit 0
