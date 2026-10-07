<?php
// diag.php — диагностика Фокса и усилителя одной страницей.
//   GET          -> HTML-страница
//   GET ?json=1  -> {"sections":[{"title":..,"rows":[[k,v],..]}],"logs":[{"title":..,"text":..}],"text":"отчёт"}
require_once 'config.php';

function rd($f) { $t = @file_get_contents($f); return $t === false ? '' : trim($t); }
function sh($c) { return trim((string)@shell_exec('( ' . $c . ' ) 2>/dev/null')); }
function tail_file($f, $n) {
    if (!is_file($f)) return '';
    $l = @file($f, FILE_IGNORE_NEW_LINES) ?: [];
    return implode("\n", array_slice($l, -$n));
}
function dur($s) {
    $s = (int)$s; $d = intdiv($s, 86400); $h = intdiv($s % 86400, 3600); $m = intdiv($s % 3600, 60);
    return ($d ? $d . ' д ' : '') . sprintf('%02d:%02d', $h, $m);
}
function mb($kb) { return round($kb / 1024) . ' МБ'; }

$players = ['S95qobuz' => 'Qobuz Connect', 'S95naa' => 'HQPlayer NAA', 'S95roonready' => 'Roon Ready', 'S95mpd' => 'MPD / UPnP',
            'S95aprenderer' => 'Audirvana / UPnP-рендерер', 'S95aplayer' => 'aplayer', 'S95apscream' => 'apscream',
            'S95shairport' => 'AirPlay', 'S95squeezelite' => 'LMS / Squeezelite', 'S95spotify' => 'Spotify Connect'];

// ---------------------------------------------------------------- system
$up = (float)explode(' ', rd('/proc/uptime'))[0];
$la = array_slice(explode(' ', rd('/proc/loadavg')), 0, 3);
$temp = rd('/sys/class/thermal/thermal_zone0/temp');
$mem = [];
foreach (@file('/proc/meminfo') ?: [] as $l) if (preg_match('/^(\w+):\s+(\d+)/', $l, $m)) $mem[$m[1]] = (int)$m[2];
$fs = [];
foreach (['/' => 'система', '/userdata' => 'данные'] as $p => $n) {
    $tot = @disk_total_space($p); $free = @disk_free_space($p);
    if ($tot) $fs[] = $n . ' ' . round($free / 1048576) . ' из ' . round($tot / 1048576) . ' МБ свободно';
}
$ip = sh("ip -4 -o addr show eth0 | awk '{print \$4}'");
$speed = rd('/sys/class/net/eth0/speed');
$carrier = rd('/sys/class/net/eth0/carrier');
$rx = (int)rd('/sys/class/net/eth0/statistics/rx_errors') + (int)rd('/sys/class/net/eth0/statistics/rx_dropped');
$sys = [
    ['DigiFox', VERSION . ' (PureFox ' . PUREFOX_VERSION . ')'],
    ['Ядро', sh('uname -r')],
    ['Работает', dur($up)],
    ['Нагрузка', implode(' / ', $la)],
    ['Температура ЦП', $temp !== '' ? round($temp / 1000, 1) . ' °C' : '—'],
    ['Память', isset($mem['MemTotal']) ? mb($mem['MemAvailable'] ?? $mem['MemFree']) . ' свободно из ' . mb($mem['MemTotal']) : '—'],
    ['Хранилище', $fs ? implode(', ', $fs) : '—'],
    ['Сеть', ($carrier === '1' ? 'есть, ' . ($speed > 0 ? $speed . ' Мбит/с' : '?') : 'нет связи') . ($ip ? ', ' . $ip : '')
             . ($rx ? ', ошибок приёма ' . $rx : '')],
    ['Время', date('d.m.Y H:i:s T')],
];

// ---------------------------------------------------------------- amplifier
$st = preg_split('/\s+/', rd('/tmp/amp_state'));
$age = isset($st[5]) && $st[5] !== '' ? time() - (int)$st[5] : null;
$ampver = rd('/tmp/amp_ver');
$ak = rd('/tmp/amp_ak');
$amp = [
    ['Связь', is_file('/tmp/amp_state') ? 'есть' . ($age !== null ? ', последний ответ ' . dur(max(0, $age)) . ' назад' : '') : 'нет ответа от усилителя'],
    ['Прошивка', $ampver !== '' ? $ampver : '—'],
    ['Встроенная в DigiFox', rd('/usr/share/digifox/amp-fw.ver') ?: '—'],
    ['Питание', count($st) >= 5 ? ($st[4] === '1' ? 'включён' : 'дежурный режим') : '—'],
    ['Громкость', count($st) >= 2 ? $st[0] . ' из ' . $st[1] . ($st[2] === '1' ? ', MUTE' : '') : '—'],
    ['AK4137', $ak === '1' ? 'установлен' : ($ak === '0' ? 'нет' : '—')],
];

// ---------------------------------------------------------------- audio
$link = @readlink('/etc/init.d/S95player');
$srcmode = rd('/etc/digifox/srcmode') === 'fox' ? 'ФОКС' : 'AK4137';
$filt = str_replace("\n", ', ', rd('/etc/digifox/srcfilter'));
$loud = rd('/etc/digifox/loudness');
$i2s = str_replace("\n", ', ', sh("grep -E '^(MODE|MCLK|PCM_SWAP|DSD_SWAP|FREQ_SWAP)=' /etc/i2s.conf"));
$hw = []; $stat = [];
foreach (glob('/proc/asound/card*/pcm*p/sub0/hw_params') ?: [] as $f) {
    $t = rd($f);
    $hw[] = basename(dirname(dirname(dirname($f)))) . ': ' . ($t === 'closed' ? 'закрыто'
          : str_replace("\n", ', ', preg_replace('/^(access|subformat|period_size|buffer_size).*\n?/m', '', $t)));
}
$aud = [
    ['Плеер', $link ? ($players[basename($link)] ?? basename($link)) : 'не выбран'],
    ['Пересчёт частоты', $srcmode . ($srcmode === 'ФОКС' && $filt !== '' ? ' (' . $filt . ')' : '')],
    ['Тонкомпенсация', ($loud !== '' && ($loud[0] === '1' || strncmp($loud, 'on', 2) === 0)) ? 'вкл.' : 'выкл.'],
    ['Вход', rd('/tmp/digifox_in') ?: '—'],
    ['I2S', $i2s ?: '—'],
    ['Выход ALSA', $hw ? implode('; ', $hw) : '—'],
    ['Режим USB → I2S', sh('pidof uac2_router') ? 'включён' : 'выкл.'],
];

$sections = [['title' => 'СИСТЕМА', 'rows' => $sys], ['title' => 'УСИЛИТЕЛЬ', 'rows' => $amp], ['title' => 'ЗВУК', 'rows' => $aud]];

// ---------------------------------------------------------------- logs
$dmesg = sh('/usr/bin/sudo -n dmesg | tail -n 40');
if ($dmesg === '') $dmesg = sh('dmesg | tail -n 40');
$logs = [];
foreach ([['Ядро (dmesg)', $dmesg],
          ['События пересчёта (digifox_events.log)', tail_file('/tmp/digifox_events.log', 30)],
          ['Пересчёт (digifox_src.log)', tail_file('/tmp/digifox_src.log', 30)],
          ['Qobuz (qobuz-meta.log)', tail_file('/tmp/qobuz-meta.log', 20)]] as [$t, $x]) {
    if ($x !== '') $logs[] = ['title' => $t, 'text' => $x];
}

$text = "DigiFox — диагностика\n";
foreach ($sections as $s) {
    $text .= "\n[" . $s['title'] . "]\n";
    foreach ($s['rows'] as [$k, $v]) $text .= $k . ': ' . $v . "\n";
}
foreach ($logs as $l) $text .= "\n[" . $l['title'] . "]\n" . $l['text'] . "\n";

if (isset($_GET['json'])) {
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo json_encode(['sections' => $sections, 'logs' => $logs, 'text' => $text], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}
if (isset($_GET['txt'])) {
    header('Content-Type: text/plain; charset=utf-8');
    header('Content-Disposition: attachment; filename="digifox-diag-' . date('Ymd-His') . '.txt"');
    echo $text;
    exit;
}
$h = function ($s) { return htmlspecialchars((string)$s, ENT_QUOTES); };
?>
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#101113" media="(prefers-color-scheme: dark)">
    <meta name="theme-color" content="#eef0f2" media="(prefers-color-scheme: light)">
    <title>DigiFox — диагностика</title>
    <link rel="icon" href="favicon.svg" type="image/svg+xml">
    <link rel="icon" href="favicon.ico" sizes="any">
    <link rel="stylesheet" href="assets/css/digifox.css?v=<?php echo VERSION; ?>">
    <style>
        .kv { display: flex; gap: 12px; padding: 7px 0; border-top: 1px solid var(--line, rgba(128,128,128,.18)); font-size: 14px; }
        .kv:first-of-type { border-top: 0; }
        .kv .k { color: var(--dim); flex: 0 0 42%; }
        .kv .v { flex: 1; min-width: 0; overflow-wrap: anywhere; font-variant-numeric: tabular-nums; }
        .diag-log { white-space: pre-wrap; overflow-wrap: anywhere; font-family: var(--mono); font-size: 11px; line-height: 1.4;
                    max-height: 320px; overflow: auto; margin-top: 8px; }
    </style>
</head>
<body>
<main class="wrap">
    <header class="head">
        <a class="back" href="index.php" aria-label="Назад">
            <svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path d="M15 5l-7 7 7 7" stroke="currentColor" stroke-width="1.6" fill="none"/></svg>
        </a>
        <div class="brand">ДИАГНОСТИКА</div>
        <span class="dot"></span>
    </header>

<?php foreach ($sections as $s): ?>
    <section class="panel">
        <div class="cap"><?php echo $h($s['title']); ?></div>
<?php foreach ($s['rows'] as [$k, $v]): ?>
        <div class="kv"><div class="k"><?php echo $h($k); ?></div><div class="v"><?php echo $h($v); ?></div></div>
<?php endforeach; ?>
    </section>
<?php endforeach; ?>

<?php foreach ($logs as $l): ?>
    <section class="panel">
        <div class="cap"><?php echo $h($l['title']); ?></div>
        <div class="diag-log"><?php echo $h($l['text']); ?></div>
    </section>
<?php endforeach; ?>

    <section class="panel">
        <div class="row2">
            <button class="hifi" onclick="location.reload()">ОБНОВИТЬ</button>
            <a class="hifi" href="diag.php?txt=1" style="text-align:center;text-decoration:none">СКАЧАТЬ ОТЧЁТ</a>
        </div>
        <div class="hint">Отчёт — текстовый файл со всем, что на этой странице. Его можно приложить к вопросу о неисправности.</div>
    </section>

    <footer class="foot">DigiFox v<?php echo $h(VERSION); ?></footer>
</main>
</body>
</html>
