<?php
// src.php — кто пересчитывает частоту: AK4137 в усилителе или Фокс (digifox-srcmode).
//   GET            -> {"mode":"ak4137"|"fox"}
//   POST mode=...  -> переключить (перезапускает активный плеер), ответ как GET
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

function src_mode() {
    $m = trim((string)@file_get_contents('/etc/digifox/srcmode'));
    return $m === 'fox' ? 'fox' : 'ak4137';
}

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
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
echo json_encode(['mode' => src_mode()]);
