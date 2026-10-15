<?php
// /opt/update.sh itself runs the update as a detached process (setsid) and
// only shows its log here, so the update finishes even if this page, PHP or
// lighttpd are replaced or killed while the files are being written.
header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');
set_time_limit(0);
ignore_user_abort(true);
while (ob_get_level()) ob_end_flush();

$p = popen('/usr/bin/sudo /opt/update.sh 2>&1', 'r');
if ($p) {
    while (($chunk = fread($p, 1024)) !== false && $chunk !== '') {
        echo $chunk;
        flush();
    }
    pclose($p);
}
