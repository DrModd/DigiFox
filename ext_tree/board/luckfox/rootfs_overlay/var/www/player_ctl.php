<?php
// player_ctl.php — кнопки плеера (пульт усилителя, pfctl "ctl ..."): тот плеер, что сейчас выбран.
//
//   GET ?cmd=toggle|next|prev   -> "OK <плеер>" | "ERR <причина>"
//
//   mpd       (DLNA/upmpdcli, Яндекс Музыка) — протокол MPD на :6600
//   aplayer   (веб-радио и файлы APlayer)    — его API на :7778 (Pause/Next/Prev; радио: стоп/продолжить)
//   raat      (Roon Ready)                   — MPRIS по D-Bus: транспорт hifiberry в raat_app (/etc/raat.conf)
//   qobuz     (Qobuz Connect)                — FIFO /tmp/qobuz_ctl, его читает libassert_hook.so внутри плеера
// Остальные (NAA, AirPlay, Spotify, LMS, APScream, aprenderer) — "ERR unsupported".
header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');

$cmd = (string)($_GET['cmd'] ?? '');
if (!in_array($cmd, ['toggle', 'next', 'prev'], true)) { echo 'ERR bad_cmd'; exit; }

function svc() {
    $j = @json_decode((string)@file_get_contents('/tmp/system_status.json'), true);
    $s = is_array($j) ? (string)($j['active_service'] ?? '') : '';
    if ($s === '') $s = trim((string)@file_get_contents('/tmp/active_service'));
    return $s;
}

// --- MPD ------------------------------------------------------------------
function mpd($lines) {
    $s = @fsockopen('127.0.0.1', 6600, $en, $es, 1.0);
    if (!$s) return null;
    stream_set_timeout($s, 3);
    fgets($s);
    $out = [];
    foreach ((array)$lines as $c) {
        fwrite($s, $c . "\n");
        while (($l = fgets($s)) !== false) {
            $l = rtrim($l, "\n");
            if ($l === 'OK') break;
            if (strpos($l, 'ACK') === 0) { fclose($s); return false; }
            $p = strpos($l, ': ');
            if ($p !== false) $out[substr($l, 0, $p)] = substr($l, $p + 2);
        }
    }
    fwrite($s, "close\n");
    fclose($s);
    return $out;
}
function ctl_mpd($cmd) {
    if ($cmd === 'toggle') {
        $st = mpd('status');
        if ($st === null) return 'ERR mpd_not_running';
        $r = mpd(($st['state'] ?? '') === 'play' ? 'pause 1' : 'play');
    } else {
        $r = mpd($cmd === 'next' ? 'next' : 'previous');
    }
    return $r === null ? 'ERR mpd_not_running' : ($r === false ? 'ERR mpd_refused' : 'OK mpd');
}

// --- APlayer --------------------------------------------------------------
function ctl_aplayer($cmd) {
    require_once __DIR__ . '/radio_lib.php';
    $st = ap('State', 3);
    if ($st === null) return 'ERR aplayer_not_running';
    $radio = !empty($st['RadioMode']);
    if ($cmd === 'toggle') {
        if (!empty($st['Playing'])) { ap($radio ? 'Stop' : 'Pause', 5); return 'OK aplayer'; }
        if ($radio) { $r = resume(); return strpos($r, 'OK') === 0 ? 'OK aplayer' : $r; }
        ap('Pause', 5);                                   // файлы: снять с паузы
        return 'OK aplayer';
    }
    $r = ap($cmd === 'next' ? 'Next' : 'Prev', 10);
    return $r === null ? 'ERR aplayer_not_running' : 'OK aplayer';
}

// --- Roon Ready (raat_app, MPRIS org.mpris.MediaPlayer2.raat) -----------------
function ctl_raat($cmd) {
    $m = ['toggle' => 'PlayPause', 'next' => 'Next', 'prev' => 'Previous'][$cmd];
    exec('timeout 3 dbus-send --system --print-reply --dest=org.mpris.MediaPlayer2.raat ' .
         '/org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.' . $m . ' 2>&1', $o, $rc);
    return $rc === 0 ? 'OK raat' : 'ERR raat_no_control';
}

// --- Qobuz Connect (FIFO внутри плеера) -------------------------------------
function ctl_qobuz($cmd) {
    $f = '/tmp/qobuz_ctl';
    if (@filetype($f) !== 'fifo') return 'ERR qobuz_no_control';
    // писать с тайм-аутом: если плеер умер, у FIFO нет читателя и запись повисла бы
    exec('timeout 2 sh -c ' . escapeshellarg('echo ' . $cmd . ' > ' . $f) . ' 2>&1', $o, $rc);
    return $rc === 0 ? 'OK qobuz' : 'ERR qobuz_not_running';
}

$s = svc();
if (file_exists('/etc/usb_to_i2s.state')) { echo 'ERR usb_mode'; exit; }
switch ($s) {
    case 'mpd':     echo ctl_mpd($cmd); break;
    case 'aplayer': echo ctl_aplayer($cmd); break;
    case 'raat':    echo ctl_raat($cmd); break;
    case 'qobuz':   echo ctl_qobuz($cmd); break;
    default:        echo 'ERR unsupported ' . ($s === '' ? '-' : $s);
}
