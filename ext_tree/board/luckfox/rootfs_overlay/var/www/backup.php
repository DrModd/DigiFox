<?php
// backup.php — резервная копия настроек DigiFox одним файлом (JSON).
//   GET              -> скачать digifox-settings-YYYYMMDD.json
//   POST file=<json> -> восстановить; затем нужна перезагрузка (reboot.php)
// В копию входит: настройки I2S (тактирование, MCLK, перестановки), активный плеер,
// /etc/digifox/* (пересчёт, фильтр, тонкомпенсация, будильник, часовой пояс),
// список веб-радио, настройки веб-радио и UPnP-рендерера.
// Не входит: пароль, ключи SSH, MAC-адрес, режим USB → I2S.
require_once 'config.php';

const PLAYERS = ['qobuz' => 'S95qobuz', 'naa' => 'S95naa', 'raat' => 'S95roonready', 'mpd' => 'S95mpd',
                 'aprenderer' => 'S95aprenderer', 'aplayer' => 'S95aplayer', 'apscream' => 'S95apscream',
                 'shairport' => 'S95shairport', 'lms' => 'S95squeezelite', 'spotify' => 'S95spotify'];
const I2S_KEYS = ['MODE' => ['pll', 'ext'], 'MCLK' => ['512', '1024'], 'PCM_SWAP' => ['0', '1'],
                  'DSD_SWAP' => ['0', '1'], 'FREQ_SWAP' => ['0', '1']];

function i2s_read() {
    $r = [];
    foreach (@file('/etc/i2s.conf', FILE_IGNORE_NEW_LINES) ?: [] as $l)
        if (preg_match('/^([A-Z_]+)=(\S+)/', $l, $m) && isset(I2S_KEYS[$m[1]])) $r[$m[1]] = $m[2];
    return $r;
}

function text_files() {
    $f = glob('/etc/digifox/*') ?: [];
    $f[] = '/var/www/radio.json';
    return array_values(array_filter($f, function ($p) {
        return is_file($p) && filesize($p) < 65536 && !preg_match('/\.tmp$/', $p);
    }));
}

function bin_files() {
    return array_values(array_filter(array_merge(glob('/usr/aplayer/*.dat') ?: [], glob('/usr/aprenderer/*.dat') ?: []),
        function ($p) { return is_file($p) && filesize($p) < 262144; }));
}

// ---------------------------------------------------------------- save
if ($_SERVER['REQUEST_METHOD'] === 'GET') {
    $player = null;
    $link = @readlink('/etc/init.d/S95player');
    if ($link) $player = array_search(basename($link), PLAYERS, true) ?: null;
    $out = [
        'digifox_backup' => 1,
        'version' => VERSION,
        'date' => date('c'),
        'i2s' => i2s_read(),
        'player' => $player,
        'files' => [],
        'bin' => [],
    ];
    foreach (text_files() as $p) $out['files'][$p] = (string)file_get_contents($p);
    foreach (bin_files() as $p) $out['bin'][$p] = base64_encode((string)file_get_contents($p));
    header('Content-Type: application/json; charset=utf-8');
    header('Content-Disposition: attachment; filename="digifox-settings-' . date('Ymd') . '.json"');
    header('Cache-Control: no-store');
    echo json_encode($out, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);
    exit;
}

// ---------------------------------------------------------------- restore
header('Content-Type: application/json; charset=utf-8');
function fail($msg, $code = 400) { http_response_code($code); echo json_encode(['error' => $msg], JSON_UNESCAPED_UNICODE); exit; }

$raw = isset($_FILES['file']) && $_FILES['file']['error'] === UPLOAD_ERR_OK
     ? (string)file_get_contents($_FILES['file']['tmp_name']) : (string)($_POST['json'] ?? '');
if ($raw === '' || strlen($raw) > 4 << 20) fail('Файл не передан или слишком большой');
$b = json_decode($raw, true);
if (!is_array($b) || ($b['digifox_backup'] ?? 0) !== 1) fail('Это не резервная копия DigiFox');

$done = [];
// files: only paths that this page itself saves
$allowed_text = function ($p) { return preg_match('#^/etc/digifox/[A-Za-z0-9._-]+$#', $p) || $p === '/var/www/radio.json'; };
$allowed_bin  = function ($p) { return preg_match('#^/usr/(aplayer|aprenderer)/[A-Za-z0-9._-]+\.dat$#', $p); };
@mkdir('/etc/digifox', 0777, true);
foreach (($b['files'] ?? []) as $p => $c) {
    if (!is_string($p) || !is_string($c) || !$allowed_text($p) || strpos($p, '..') !== false) continue;
    if (file_put_contents($p . '.tmp', $c) !== false && rename($p . '.tmp', $p)) $done[] = $p;
}
foreach (($b['bin'] ?? []) as $p => $c) {
    if (!is_string($p) || !is_string($c) || !$allowed_bin($p) || strpos($p, '..') !== false) continue;
    $d = base64_decode($c, true);
    if ($d !== false && is_dir(dirname($p)) && file_put_contents($p, $d) !== false) $done[] = $p;
}

// player: the boot scripts start /etc/init.d/S95player
$pl = $b['player'] ?? null;
if (is_string($pl) && isset(PLAYERS[$pl]) && is_file('/etc/rc.pure/' . PLAYERS[$pl])) {
    shell_exec('/usr/bin/sudo /bin/sh -c ' . escapeshellarg('rm -f /etc/init.d/S95player && ln -s /etc/rc.pure/' . PLAYERS[$pl] . ' /etc/init.d/S95player'));
    $done[] = 'player ' . $pl;
}

// I2S: values into i2s.conf, then the DTB of the clock mode (as /opt/2*.sh do)
$want = i2s_read();
$cur = $want;
foreach (($b['i2s'] ?? []) as $k => $v) {
    if (isset(I2S_KEYS[$k]) && in_array((string)$v, I2S_KEYS[$k], true)) $want[$k] = (string)$v;
}
$conf = (string)@file_get_contents('/etc/i2s.conf');
foreach ($want as $k => $v) {
    if (preg_match('/^' . $k . '=.*$/m', $conf)) $conf = preg_replace('/^' . $k . '=.*$/m', $k . '=' . $v, $conf);
    else $conf .= "\n$k=$v\n";
}
file_put_contents('/etc/i2s.conf', $conf);
$done[] = '/etc/i2s.conf';
$mode = $want['MODE'] ?? 'pll';
$mclk = $want['MCLK'] ?? '1024';
if (($cur['MODE'] ?? '') !== $mode || ($mode === 'ext' && ($cur['MCLK'] ?? '') !== $mclk)) {
    $dtb = $mode === 'ext' ? ($mclk === '512' ? '512_ext.dtb' : '1024_ext.dtb') : '1024_pll.dtb';
    $reg = $mode === 'ext' ? 's/007c003c/007c001c/' : 's/007c001c/007c003c/';
    shell_exec('/usr/bin/sudo /bin/sh -c ' . escapeshellarg(
        "sed -i '$reg' /etc/init.d/S94ioi2s; flash_erase /dev/mtd3 0x003C0000 0x2 && sleep 1 && " .
        "nandwrite -p /dev/mtd3 -s 0x003C0000 /data/boot/$dtb; sync") . ' 2>&1');
    $done[] = "DTB $dtb";
}

// sample-rate conversion mode -> asound.std
if (is_file('/etc/digifox/srcmode')) shell_exec('/usr/bin/sudo /usr/bin/digifox-srcmode apply 2>&1');

shell_exec('sync');
// the page (or the app) reboots the Fox afterwards via reboot.php
echo json_encode(['ok' => true, 'restored' => $done, 'from' => $b['version'] ?? null, 'reboot' => true],
                 JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
