<?php
header('Content-Type: text/event-stream');
header('Cache-Control: no-cache');
header('X-Accel-Buffering: no');

@set_time_limit(0);
while (ob_get_level() > 0) {
    ob_end_flush();
}

$socket = @stream_socket_client('unix:///tmp/status_monitor_events.sock', $errno, $errstr, 1);
if ($socket === false) {
    http_response_code(503);
    echo "event: unavailable\ndata: {}\n\n";
    flush();
    exit;
}

stream_set_blocking($socket, true);
while (!connection_aborted() && ($status = fgets($socket)) !== false) {
    // DigiFox: output is always 100% (volume lives in the amplifier)
    $st = json_decode(trim($status), true);
    if (is_array($st)) {
        $st['volume'] = '100%';
        $st['muted'] = false;
        $st['volume_control_available'] = false;
        $st['mute_control_available'] = false;
        $status = json_encode($st);
    }
    echo "event: status\ndata: " . trim($status) . "\n\n";
    flush();
}

fclose($socket);
