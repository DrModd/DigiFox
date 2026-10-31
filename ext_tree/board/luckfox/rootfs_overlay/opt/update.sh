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
# With read-only rootfs + userdata (S00overlay) the image is written to the rootfs
# itself (remounted rw, reached by a plain bind mount under the overlays), and at the
# next boot S00overlay drops from the change layer everything but user files (keep.rules).
# idblock and U-Boot are never touched online (the environment only once, by
# /etc/init.d/S00overlay: ",-(userdata)" appended to mtdparts);
# for those flash the full image with RV1106_Toolkit.

REPO="DrModd/DigiFox"
BASE="https://github.com/$REPO/releases/latest/download"
WORK=/tmp/digifox-update
NEW=$WORK/root
BOOT_MTD=/dev/mtd3
DTB_OFFSET=3932160          # 0x3C0000, as in post-build.sh and /opt/2*.sh
CUR_BUILD=$(cat /etc/digifox-release 2>/dev/null || echo unknown)

log()  { echo "$*"; }
fail() { echo "ОШИБКА: $*"; rm -f /tmp/fox_upd; cleanup; exit 1; }
# шаг обновления для усилителя (pfctl -> @UPD): звук приглушён, на экране «DIGIFOX UPDATE»
amp_step() { echo "$1" > /tmp/fox_upd.tmp && mv /tmp/fox_upd.tmp /tmp/fox_upd; }

cleanup() {
    grep -q " $NEW " /proc/mounts 2>/dev/null && umount "$NEW"
    grep -q " $WORK/rom " /proc/mounts 2>/dev/null && umount "$WORK/rom"
    rm -rf "$WORK"
}

fetch() {   # fetch <name> <out>
    wget -q -T 60 -O "$2" "$BASE/$1" 2>/dev/null
}

trap cleanup INT TERM

# Обновление всегда идёт отдельным процессом (setsid), с журналом в $ULOG.
# Тот, кто его запустил (веб-страница, ssh), только показывает журнал.
# Если веб-сервер, PHP или ssh-сессия оборвутся посреди записи файлов,
# обновление всё равно дойдёт до конца — иначе файлы остаются недописанными
# (так php-cgi оказался пустым, 0 байт, и веб отвечал 500).
ULOG=/tmp/digifox-update.log
UPID=/tmp/digifox-update.pid
END_MARK='@@UPDATE-END@@'
if [ "$1" != "--check" ] && [ -z "$DIGIFOX_UPD_CHILD" ]; then
    pid=$(cat "$UPID" 2>/dev/null)
    if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
        echo "Обновление уже идёт, показываю журнал:"
    else
        : > "$ULOG"
        DIGIFOX_UPD_CHILD=1 setsid sh -c "\"$0\" $*; echo '$END_MARK'" >> "$ULOG" 2>&1 < /dev/null &
        echo $! > "$UPID"
    fi
    # показать журнал до конца обновления (обрыв просмотра не мешает обновлению)
    pid=$(cat "$UPID" 2>/dev/null); shown=0; gone=0
    while :; do
        total=$(wc -l < "$ULOG" 2>/dev/null || echo 0)
        if [ "$total" -gt "$shown" ]; then
            sed -n "$((shown + 1)),${total}p" "$ULOG" | grep -v "^$END_MARK\$"
            shown=$total
        fi
        grep -q "^$END_MARK\$" "$ULOG" 2>/dev/null && break
        if [ ! -d "/proc/$pid" ]; then gone=$((gone + 1)); [ $gone -gt 2 ] && break; fi
        sleep 1
    done
    exit 0
fi
trap '' HUP PIPE

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

amp_step "1 DOWNLOADING"
log "Скачиваю образ..."
fetch rootfs.squashfs "$WORK/rootfs.squashfs" || fail "не скачался rootfs.squashfs"
fetch boot.img "$WORK/boot.img" || fail "не скачался boot.img"
log "Проверяю контрольные суммы..."
( cd "$WORK" && grep -E ' (rootfs\.squashfs|boot\.img)$' SHA256SUMS | sha256sum -c - ) || fail "контрольная сумма не совпала"

mount -t squashfs -o loop,ro "$WORK/rootfs.squashfs" "$NEW" || fail "не смонтировать образ"
[ -x "$NEW/usr/bin/pfctl" ] && [ -f "$NEW/etc/digifox-release" ] || fail "образ не похож на DigiFox"

# ---- куда писать систему ----
# Система только для чтения (S00overlay): /etc /usr ... — overlay поверх rootfs, изменения
# на разделе userdata. Новая система пишется в сам rootfs: перемонтировать его на запись
# и взять «чистым» через простой bind (без overlay-каталогов поверх).
ROOTW=""
if grep -q '^ov-etc /etc overlay ' /proc/mounts && grep -q ' /overlay ubifs ' /proc/mounts; then
    log "Система только для чтения: пишу обновление в rootfs, настройки остаются на userdata"
    mount -o remount,rw / || fail "rootfs не перемонтировался на запись"
    mkdir -p "$WORK/rom" && mount -o bind / "$WORK/rom" || fail "не получился bind rootfs"
    ROOTW="$WORK/rom"
fi

# пользовательские файлы, которые обновление не трогает: список из нового образа
# (тот же читает S00overlay); в старых образах его не было — тот же список здесь
RULES="$WORK/keep.rules"
if [ -f "$NEW/usr/share/digifox/keep.rules" ]; then
    cp "$NEW/usr/share/digifox/keep.rules" "$RULES"
else
    cat > "$RULES" <<'EOR'
protect /etc/i2s.conf
protect /etc/asound.conf
protect /etc/output
protect /etc/usb_to_i2s.state
protect /etc/init.d/S95*
protect /etc/shadow
protect /etc/digifox/
protect /etc/digifox/**
protect /var/lib/digifox/
protect /var/lib/digifox/**
protect /etc/resolv.conf
protect /etc/hostname
protect /etc/hosts
protect /etc/dropbear/dropbear_*_host_key
protect /data/ethaddr.txt
protect /var/www/radio.json
protect /usr/aplayer/*.dat
protect /usr/aplayer/Radio/**
protect /usr/aprenderer/*.dat
protect /usr/apscream/config.txt
EOR
fi

# ---- остановить плееры, сохранить настройки I2S ----
log "Останавливаю плееры..."
for s in /etc/init.d/S95*; do [ -x "$s" ] && "$s" stop >/dev/null 2>&1; done
cp /etc/i2s.conf "$WORK/i2s.conf" 2>/dev/null

# ---- файлы ----
# метка «обновление не закончено»: если Фокс перезагрузится посреди записи файлов,
# при следующей загрузке S98digifox-guard сам запустит переустановку (--force)
mkdir -p /etc/digifox && echo "$NEW_BUILD" > /etc/digifox/update-pending && sync
# копия пользовательских настроек: что бы ни сделал rsync, вернуть их после
cp -a /etc/digifox "$WORK/etc-digifox" 2>/dev/null
amp_step "2 INSTALLING"
log "Обновляю файлы системы..."
# rsync и его библиотеки берём из нового образа (загрузчик и библиотеки образа),
# а не из системы: если прошлое обновление прервалось, системные библиотеки
# (libcrypto) могут быть недописаны, и системный rsync даже не запустится.
# --fsync: каждый файл записан на флеш до замены старого — при перезагрузке или
# пропадании питания посреди обновления не остаётся пустых файлов (0 байт).
RSYNC="$NEW/lib/ld-linux-armhf.so.3 --library-path $NEW/lib:$NEW/usr/lib $NEW/usr/bin/rsync"
$RSYNC --version >/dev/null 2>&1 || RSYNC=rsync
FSYNC=""; $RSYNC --help 2>/dev/null | grep -q -- '--fsync' && FSYNC=--fsync
# станции веб-радио правили на странице «Радиостанции» — список целиком пользовательский:
# не добавлять обратно удалённые станции из образа и не трогать папку вообще
RADIO_EXCL=""; [ -f /etc/digifox/radio_edited ] && RADIO_EXCL="--exclude=/usr/aplayer/Radio/"
sync_root() {
    $RSYNC -ac $FSYNC --delete-before $RADIO_EXCL \
    --exclude=/dev --exclude=/proc --exclude=/sys --exclude=/tmp --exclude=/run \
    --exclude=/mnt --exclude=/media --exclude=/root --exclude=/var/run --exclude=/var/log \
    --exclude=/etc/digifox-release \
    --exclude=/overlay \
    --filter="merge $RULES" \
    "$NEW/" "$ROOTW/"
}
sync_root || fail "rsync завершился с ошибкой — система могла обновиться частично, повторите обновление"
sync

# второй проход: rsync -c заново сверяет каждый файл с образом и переписывает
# те, что не совпали (так однажды повредился php-cgi и пропал веб-интерфейс)
# сбросить кэш, чтобы сверка читала файлы с флеша, а не из памяти
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
log "Проверяю записанные файлы..."
sync_root || fail "повторная проверка файлов не прошла — повторите обновление"
# пользовательские настройки — назад, если rsync что-то из них убрал
if [ -d "$WORK/etc-digifox" ]; then
    mkdir -p /etc/digifox && chmod 0777 /etc/digifox
    for f in "$WORK/etc-digifox"/* "$WORK/etc-digifox"/.[!.]*; do
        [ -e "$f" ] && [ ! -e "/etc/digifox/${f##*/}" ] && cp -a "$f" /etc/digifox/
    done
    sync
fi
# номер версии — последним, когда все файлы уже на месте: если обновление
# прервётся, Фокс останется на старом номере и предложит обновиться снова
cp "$NEW/etc/digifox-release" "$ROOTW/etc/digifox-release.new" && \
    mv "$ROOTW/etc/digifox-release.new" "$ROOTW/etc/digifox-release" && sync

# настройки I2S: файл конфигурации сохранён, S94ioi2s пришёл новый (PLL) — привести в соответствие
[ -f "$WORK/i2s.conf" ] && cp "$WORK/i2s.conf" /etc/i2s.conf
MODE=$(sed -n 's/^MODE=//p' /etc/i2s.conf 2>/dev/null)
MCLK=$(sed -n 's/^MCLK=//p' /etc/i2s.conf 2>/dev/null)
[ "$MODE" = ext ] && sed -i 's/007c003c/007c001c/' "$ROOTW/etc/init.d/S94ioi2s"

umount "$NEW"
sync

# ---- ядро ----
dd if="$BOOT_MTD" of="$WORK/cur_kernel" bs=65536 count=$((DTB_OFFSET / 65536)) 2>/dev/null
dd if="$WORK/boot.img" of="$WORK/new_kernel" bs=65536 count=$((DTB_OFFSET / 65536)) 2>/dev/null
if cmp -s "$WORK/cur_kernel" "$WORK/new_kernel"; then
    log "Ядро не изменилось."
else
    amp_step "3 WRITING KERNEL"
    log "Записываю новое ядро (boot.img)... Не выключайте питание!"
    size=$(wc -c < "$WORK/boot.img")
    flash_erase "$BOOT_MTD" 0 0 >/dev/null || fail "flash_erase boot — восстановите прошивку через RV1106_Toolkit"
    mtd_debug write "$BOOT_MTD" 0 "$size" "$WORK/boot.img" >/dev/null || fail "запись boot — восстановите прошивку через RV1106_Toolkit"
fi

# ---- DTB текущего режима I2S ----
case "$MODE:$MCLK" in
    ext:512) DTB="$ROOTW/data/boot/512_ext.dtb" ;;
    ext:*)   DTB="$ROOTW/data/boot/1024_ext.dtb" ;;
    *)       DTB="$ROOTW/data/boot/1024_pll.dtb" ;;
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

# слой изменений: при следующей загрузке оставить в нём только пользовательские файлы,
# иначе старые копии системных файлов (их правили во время работы) закрыли бы новые
if [ -n "$ROOTW" ]; then
    touch /overlay/.update-cleanup
    sync
    umount "$WORK/rom" 2>/dev/null
fi
rm -f /etc/digifox/update-pending; sync
amp_step "4 RESTARTING"
log "Готово: $CUR_BUILD -> $NEW_BUILD"
log "Перезагрузка через 5 секунд..."
cleanup
sync
( sleep 5; sync; reboot ) >/dev/null 2>&1 &
exit 0
