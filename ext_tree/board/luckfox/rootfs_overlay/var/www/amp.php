<?php
// amp.php — громкость усилителя DigiD D1 для приложения Fox Remote.
// Связь с усилителем идёт через pfctl serve (консольный UART -> STM32).
//   GET               -> {"present":true,"pos":40,"max":80,"mute":false,"db":true,"power":true}
//   POST action=vol&pos=N  -> запрос громкости (pos 0..max)
//   POST action=mute       -> переключить mute усилителя
//   POST action=power      -> включить / выключить усилитель (дежурный режим)
// Положить на Фокс в /var/www/amp.php.
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

echo json_encode($st === null ? ['present' => false] : ['present' => true] + $st);
