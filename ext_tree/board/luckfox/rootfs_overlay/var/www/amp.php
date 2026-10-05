<?php
// amp.php — громкость усилителя DigiD D1 для приложения Fox Remote.
// Связь с усилителем идёт через pfctl serve (консольный UART -> STM32).
//   GET               -> {"present":true,"pos":40,"max":80,"mute":false,"db":true,"power":true}
//   POST action=vol&pos=N  -> запрос громкости (pos 0..max)
//   POST action=mute       -> переключить mute усилителя
//   POST action=power      -> включить / выключить усилитель (дежурный режим)
// DigiFox:
//   GET ?full=1  -> ещё cfg{filter,delay,dsdgain,standby,autoon,mode}, ver, now,
//                   tz, sleep_left (с), alarm{on,time,days,src,vol}
//   POST action=set&key=K&val=V    -> настройка усилителя (@ASET)
//   POST action=sleep&min=N        -> таймер сна, 0 — отменить
//   POST action=alarm&on=&time=&days=&src=&vol=  -> будильник
//   POST action=tz&tz=MSK-3        -> часовой пояс (POSIX TZ)
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

$state_file = '/tmp/amp_state';   // пишет pfctl: "pos max mute db power time"
$req_file   = '/tmp/amp_req';     // читает pfctl

function read_state($f) {
    $t = @file_get_contents($f);
    if ($t === false) return null;
    $p = preg_split('/\s+/', trim($t));
    if (count($p) < 4) return null;
    return ['pos' => (int)$p[0], 'max' => (int)$p[1], 'mute' => $p[2] === '1', 'db' => $p[3] === '1',
            'power' => count($p) < 6 || $p[4] === '1'];
}

function put_req($f, $text) {
    $tmp = $f . '.tmp';
    file_put_contents($tmp, $text);
    rename($tmp, $f);
}

$st = read_state($state_file);

$cfg_dir    = '/etc/digifox';
$alarm_file = $cfg_dir . '/alarm.conf';
$tz_file    = $cfg_dir . '/tz';
$sleep_file = '/tmp/sleep_at';
$cfg_keys   = ['filter', 'delay', 'dsdgain', 'standby', 'autoon', 'mode'];

function tz_get($f) {
    $t = trim((string)@file_get_contents($f));
    return preg_match('/^[A-Za-z<>+\-0-9:]{3,20}$/', $t) ? $t : 'MSK-3';
}

function alarm_get($f) {
    $a = ['on' => false, 'time' => '07:00', 'days' => '12345', 'src' => 'keep', 'vol' => -30];
    foreach (@file($f, FILE_IGNORE_NEW_LINES) ?: [] as $l) {
        if (!preg_match('/^ALARM_(\w+)=(.*)$/', $l, $m)) continue;
        $v = trim($m[2], "\"' ");
        switch ($m[1]) {
            case 'ON':   $a['on'] = $v === '1'; break;
            case 'TIME': $a['time'] = $v; break;
            case 'DAYS': $a['days'] = $v; break;
            case 'SRC':  $a['src'] = $v; break;
            case 'VOL':  $a['vol'] = (int)$v; break;
        }
    }
    return $a;
}

function save_file($f, $text) {
    $tmp = $f . '.tmp';
    if (@file_put_contents($tmp, $text) === false || !@rename($tmp, $f)) {
        http_response_code(500);
        echo json_encode(['error' => 'cannot write ' . $f]);
        exit;
    }
}

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = $_POST['action'] ?? '';
    if ($action === 'sleep') {
        $min = (int)($_POST['min'] ?? 0);
        if ($min <= 0) @unlink($sleep_file);
        else save_file($sleep_file, (string)(time() + min($min, 600) * 60));
        echo json_encode(['ok' => true]);
        exit;
    }
    if ($action === 'alarm') {
        $time = preg_match('/^([01]\d|2[0-3]):[0-5]\d$/', $_POST['time'] ?? '') ? $_POST['time'] : '07:00';
        $days = preg_replace('/[^1-7]/', '', $_POST['days'] ?? '');
        $src  = preg_match('/^[a-z0-9]{2,16}$/', $_POST['src'] ?? '') ? $_POST['src'] : 'keep';
        $vol  = max(-80, min(0, (int)($_POST['vol'] ?? -30)));
        $on   = ($_POST['on'] ?? '0') === '1' && $days !== '' ? '1' : '0';
        save_file($alarm_file, "ALARM_ON=$on\nALARM_TIME=$time\nALARM_DAYS=$days\nALARM_SRC=$src\nALARM_VOL=$vol\n");
        echo json_encode(['ok' => true]);
        exit;
    }
    if ($action === 'tz') {
        $tz = $_POST['tz'] ?? '';
        if (!preg_match('/^[A-Za-z<>+\-0-9:]{3,20}$/', $tz)) { http_response_code(400); echo json_encode(['error' => 'bad tz']); exit; }
        save_file($tz_file, $tz . "\n");
        echo json_encode(['ok' => true]);
        exit;
    }
    if ($action === 'set') {
        $key = $_POST['key'] ?? '';
        $val = (int)($_POST['val'] ?? -1);
        if (!in_array($key, $cfg_keys, true) || $val < 0 || $val > ($key === 'standby' ? 2 : 1)) {
            http_response_code(400); echo json_encode(['error' => 'bad setting']); exit;
        }
        if ($st === null) { http_response_code(503); echo json_encode(['error' => 'amplifier not connected']); exit; }
        put_req($req_file, "set $key $val");
        echo json_encode(['ok' => true]);
        exit;
    }
}

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    if ($st === null) {
        http_response_code(503);
        echo json_encode(['error' => 'amplifier not connected']);
        exit;
    }
    $action = $_POST['action'] ?? '';
    if ($action === 'vol') {
        $pos = (int)($_POST['pos'] ?? -1);
        if ($pos < 0 || $pos > $st['max']) {
            http_response_code(400);
            echo json_encode(['error' => 'bad pos']);
            exit;
        }
        put_req($req_file, "vol $pos");
        // сразу показать новое значение, усилитель подтвердит своим отчётом
        @file_put_contents($state_file, "$pos {$st['max']} 0 " . ($st['db'] ? '1' : '0') . ' ' .
                                        ($st['power'] ? '1' : '0') . ' ' . time());
        echo json_encode(['ok' => true, 'pos' => $pos]);
    } elseif ($action === 'mute') {
        put_req($req_file, 'mute');
        echo json_encode(['ok' => true]);
    } elseif ($action === 'power') {
        put_req($req_file, 'power');
        echo json_encode(['ok' => true]);
    } else {
        http_response_code(400);
        echo json_encode(['error' => 'bad action']);
    }
    exit;
}

$out = $st === null ? ['present' => false] : ['present' => true] + $st;
if (isset($_GET['full'])) {
    $c = preg_split('/\s+/', trim((string)@file_get_contents('/tmp/amp_cfg')));
    if (count($c) >= 6) {
        foreach ($cfg_keys as $i => $k) $out['cfg'][$k] = (int)$c[$i];
    }
    $v = trim((string)@file_get_contents('/tmp/amp_ver'));
    $out['ver'] = $v !== '' ? $v : null;
    $tz = tz_get($tz_file);
    $out['tz'] = $tz;
    $out['now'] = trim((string)shell_exec('TZ=' . escapeshellarg($tz) . ' date +%H:%M'));
    $out['time_ok'] = (int)date('Y') >= 2025;
    $sa = (int)@file_get_contents($sleep_file);
    $out['sleep_left'] = $sa > time() ? $sa - time() : 0;
    $out['alarm'] = alarm_get($alarm_file);
}
echo json_encode($out);
