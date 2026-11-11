<?php
// soloist.php — Spotify Soloist (официальный плеер Spotify): ключ разработчика и состояние.
//
//   GET                      {key:bool, version:"1.3.10…"|"", running:bool, active:"soloist"|"librespot"|""}
//   POST action=save key=…   сохранить ключ, скачать Soloist (если ещё нет), перезапустить Spotify
//   POST action=clear        убрать ключ — снова go-librespot
//
// Ключ: /etc/digifox/soloist.key (0600). Скачивание и обновление — /usr/bin/digifox-soloist.
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

const KEYF = '/etc/digifox/soloist.key';
const BIN  = '/var/lib/digifox/soloist/soloist';

function state() {
    $ver = '';
    if (is_executable(BIN)) {
        $v = trim((string)shell_exec(escapeshellarg(BIN) . ' --version 2>/dev/null | head -1'));
        if (preg_match('/soloist\s+([0-9][0-9.]*)/', $v, $m)) $ver = $m[1];
    }
    $active = '';
    if (trim((string)shell_exec('pidof soloist 2>/dev/null')) !== '') $active = 'soloist';
    elseif (trim((string)shell_exec('pidof librespot 2>/dev/null')) !== '') $active = 'librespot';
    return ['key' => is_file(KEYF) && filesize(KEYF) > 0, 'version' => $ver, 'active' => $active];
}

function spotify_restart() {
    // только если Spotify сейчас выбран: перезапустить его службу (подхватит ключ)
    $j = @json_decode((string)@file_get_contents('/tmp/system_status.json'), true);
    $svc = is_array($j) ? (string)($j['active_service'] ?? '') : '';
    if ($svc === 'spotify' || is_link('/etc/init.d/S95spotify') || is_file('/etc/init.d/S95spotify'))
        shell_exec('/etc/init.d/S95spotify restart >/dev/null 2>&1');
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') { echo json_encode(state()); exit; }

$a = (string)($_POST['action'] ?? '');
if ($a === 'save') {
    $k = trim((string)($_POST['key'] ?? ''));
    if ($k === '' || strlen($k) > 512 || !preg_match('/^[A-Za-z0-9._~+\/=-]+$/', $k)) {
        http_response_code(400); echo json_encode(['error' => 'Ключ не похож на ключ Soloist']); exit;
    }
    @mkdir('/etc/digifox', 0755, true);
    $t = KEYF . '.tmp';
    if (@file_put_contents($t, $k . "\n") === false) { http_response_code(500); echo json_encode(['error' => 'Не записать ключ']); exit; }
    @chmod($t, 0600);
    @rename($t, KEYF);
    // скачать плеер сразу (≈13 МБ), чтобы Spotify заработал без перезапуска
    if (!is_executable(BIN)) {
        exec('/usr/bin/digifox-soloist fetch 2>&1', $o, $rc);
        if ($rc !== 0 || !is_executable(BIN)) {
            $ev = @file('/tmp/digifox_events.log', FILE_IGNORE_NEW_LINES) ?: [];
            $last = '';
            foreach (array_reverse($ev) as $l) if (strpos($l, 'Soloist') !== false) { $last = $l; break; }
            http_response_code(502);
            echo json_encode(['error' => 'Ключ сохранён, но Soloist не скачался' . ($last ? ': ' . preg_replace('/^.*Soloist:?\s*/u', '', $last) : '')]);
            exit;
        }
    }
    spotify_restart();
    sync_fs();
    echo json_encode(state());
} elseif ($a === 'clear') {
    @unlink(KEYF);
    spotify_restart();
    sync_fs();
    echo json_encode(state());
} else {
    http_response_code(400); echo json_encode(['error' => 'bad action']);
}

function sync_fs() { shell_exec('sync'); }
