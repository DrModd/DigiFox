<?php
// Starts /opt/update.sh detached from the web server and streams its log.
// The update keeps running even if this page, PHP or lighttpd is replaced
// or killed while the files are being updated.
header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');
header('X-Accel-Buffering: no');
set_time_limit(0);
ignore_user_abort(true);
while (ob_get_level()) ob_end_flush();

$log  = '/tmp/digifox-update.log';
$pidf = '/tmp/digifox-update.pid';
$done = '@@UPDATE-END@@';

function running($pidf) {
    $pid = (int)@file_get_contents($pidf);
    return $pid > 0 && file_exists("/proc/$pid");
}

if (!running($pidf)) {
    @unlink($log);
    touch($log);
    $cmd = "( /opt/update.sh; echo '$done' ) > $log 2>&1 < /dev/null";
    exec("setsid sh -c " . escapeshellarg($cmd) . " > /dev/null 2>&1 < /dev/null & echo \$! > $pidf");
    usleep(300000);
}

$fh = fopen($log, 'r');
$t0 = time();
$idle = 0;
while ($fh && time() - $t0 < 1800) {
    $chunk = fread($fh, 8192);
    if ($chunk !== '' && $chunk !== false) {
        $end = strpos($chunk, $done);
        echo $end === false ? $chunk : substr($chunk, 0, $end);
        flush();
        if ($end !== false) break;
        $idle = 0;
        continue;
    }
    if (connection_aborted()) break;
    if (!running($pidf) && ++$idle > 4) break;
    usleep(500000);
}
if ($fh) fclose($fh);
