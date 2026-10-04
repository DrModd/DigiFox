<?php
// rate.php — частота и формат текущего потока для приложения Fox Remote.
// Положить на Фокс в /var/www/rate.php (необязательно: без него приложение
// просто не показывает частоту).
header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');

foreach (glob('/proc/asound/card*/pcm*p/sub0/hw_params') as $f) {
    $t = @file_get_contents($f);
    if ($t === false || strpos($t, 'closed') === 0) {
        continue;
    }
    if (preg_match('/^format:\s*(\S+)/m', $t, $fm) && preg_match('/^rate:\s*(\d+)/m', $t, $rm)) {
        $fmt = $fm[1];
        $hz  = (int)$rm[1];
        foreach (['DSD_U32' => 32, 'DSD_U16' => 16, 'DSD_U8' => 8] as $p => $bits) {
            if (strpos($fmt, $p) === 0) {
                echo 'DSD' . intdiv($hz * $bits, 44100);
                exit;
            }
        }
        echo "$hz $fmt";
        exit;
    }
}
echo 'STOP';
