<?php
// src.php — кто пересчитывает частоту: AK4137 в усилителе или Фокс (digifox-srcmode).
//   GET            -> {"mode":"ak4137"|"fox", "ak":true|false|null (есть ли AK4137)}
//   POST mode=...  -> переключить (перезапускает активный плеер), ответ как GET
//   POST phase=lin|int|min / rolloff=std|steep|slow / gain=0|-3
//                  -> фильтр пересчёта (/etc/digifox/srcfilter), слышно через ~1 с
//   POST loudness=on|off -> тонкомпенсация (/etc/digifox/loudness), только в режиме ФОКС;
//                  подъём НЧ/ВЧ растёт по мере уменьшения громкости усилителя
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

function src_mode() {
    $m = trim((string)@file_get_contents('/etc/digifox/srcmode'));
    return $m === 'fox' ? 'fox' : 'ak4137';
}

$filter_file = '/etc/digifox/srcfilter';
$loud_file = '/etc/digifox/loudness';
function loud_get() {
    $t = trim((string)@file_get_contents('/etc/digifox/loudness'));
    return $t !== '' && ($t[0] === '1' || strncmp($t, 'on', 2) === 0) ? 'on' : 'off';
}
function filter_get($f) {
    $r = ['phase' => 'lin', 'rolloff' => 'std', 'gain' => '0'];
    foreach (@file($f, FILE_IGNORE_NEW_LINES) ?: [] as $l) {
        if (preg_match('/^(phase|rolloff|gain)=(\S+)$/', trim($l), $m)) $r[$m[1]] = $m[2];
    }
    $r['loudness'] = loud_get();
    return $r;
}
$allowed = ['phase' => ['lin', 'int', 'min'], 'rolloff' => ['std', 'steep', 'slow'], 'gain' => ['0', '-3']];
if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['loudness'])) {
    $v = $_POST['loudness'];
    if (!in_array($v, ['on', 'off'], true)) { http_response_code(400); echo json_encode(['error' => 'loudness']); exit; }
    @mkdir('/etc/digifox', 0777, true);
    // keep a "ref=N" line if someone tuned it by hand
    $keep = preg_grep('/^ref=\d+$/', array_map('trim', @file($loud_file) ?: []));
    $tmp = $loud_file . '.tmp';
    if (@file_put_contents($tmp, $v . "\n" . implode("\n", $keep) . ($keep ? "\n" : '')) === false || !@rename($tmp, $loud_file)) {
        http_response_code(500); echo json_encode(['error' => 'cannot write']); exit;
    }
} elseif ($_SERVER['REQUEST_METHOD'] === 'POST' && !isset($_POST['mode'])) {
    $f = filter_get($filter_file);
    foreach ($allowed as $k => $vals) {
        if (isset($_POST[$k])) {
            if (!in_array($_POST[$k], $vals, true)) { http_response_code(400); echo json_encode(['error' => $k]); exit; }
            $f[$k] = $_POST[$k];
        }
    }
    $tmp = $filter_file . '.tmp';
    @mkdir('/etc/digifox', 0777, true);
    if (@file_put_contents($tmp, "phase={$f['phase']}\nrolloff={$f['rolloff']}\ngain={$f['gain']}\n") === false || !@rename($tmp, $filter_file)) {
        http_response_code(500); echo json_encode(['error' => 'cannot write']); exit;
    }
}

if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['mode'])) {
    $mode = $_POST['mode'] ?? '';
    if (!in_array($mode, ['ak4137', 'fox'], true)) {
        http_response_code(400);
        echo json_encode(['error' => 'mode']);
        exit;
    }
    if ($mode !== src_mode()) {
        exec('/usr/bin/sudo /usr/bin/digifox-srcmode ' . escapeshellarg($mode) . ' 2>&1', $out, $rc);
        if ($rc !== 0) {
            http_response_code(500);
            echo json_encode(['error' => implode("\n", $out)], JSON_UNESCAPED_UNICODE);
            exit;
        }
    }
}
// усилитель сообщает, есть ли AK4137 (aak, прошивка 1.2+): null — неизвестно
$ak = @file_get_contents('/tmp/amp_ak');
$ak = $ak === false ? null : (trim($ak) === '1');
echo json_encode(['mode' => src_mode(), 'ak' => $ak, 'filter' => filter_get($filter_file)]);
