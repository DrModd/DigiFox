<?php
// soloist-meta.php — трек и позиция Spotify Soloist для усилителя, веба и приложения.
// Читает события Soloist построчно ("<время_мс> <json>", soloist ctl trace — см. pfmeta
// spotify-soloist) и пишет, как остальные плееры:
//   /tmp/nowplaying  плеер, исполнитель, название, альбом, обложка, длительность (мс)
//   /tmp/np_prog     "<позиция_мс> <unix-время> play|pause"
$NP = '/tmp/nowplaying';
$PG = '/tmp/np_prog';

function wr($f, $s) {
    $t = $f . '.tmp.' . getmypid();
    if (@file_put_contents($t, $s) !== false) @rename($t, $f);
}
function clean($s) { return trim(str_replace(["\n", "\r"], ' ', (string)$s)); }

$last = ''; $status = ''; $pos = 0; $posAt = 0.0; $speed = 1.0;

function item($it) {
    global $NP, $last;
    if (!is_array($it)) return;
    $d = $it['decorations'] ?? [];
    $ti = clean($d['identity']['name'] ?? '');
    $ar = '';
    foreach ((array)($d['creators'] ?? []) as $c) {
        $n = clean($c['entity']['decorations']['identity']['name'] ?? '');
        if ($n !== '') { $ar = $n; break; }
    }
    $al = clean($d['parent']['entity']['decorations']['identity']['name'] ?? '');
    $cov = ''; $rank = -1;
    $pref = ['small' => 0, 'default' => 1, 'large' => 3, 'xlarge' => 2];   // large ≈ 640 px — хватит
    foreach ((array)($d['visual_identity']['cover'] ?? []) as $c) {
        $r = $pref[$c['size'] ?? ''] ?? 0;
        if (!empty($c['url']) && $r > $rank) { $rank = $r; $cov = (string)$c['url']; }
    }
    $dur = (int)($d['playback']['duration_ms'] ?? 0);
    $key = ($it['uri'] ?? '') . "|$ar|$ti|$al";
    if ($key === $last || ($ti === '' && $ar === '')) return;
    $last = $key;
    wr($NP, "spotify\n$ar\n$ti\n$al\n$cov\n$dur\n");
    setpos(0, 1.0);
}
function setpos($ms, $sp) {
    global $pos, $posAt, $speed;
    $pos = max(0, (int)$ms); $posAt = microtime(true); $speed = (float)$sp;
}
function position($p) {
    if (!is_array($p) || !isset($p['position_ms'])) return;
    $ms = (float)$p['position_ms'];
    $sp = (float)($p['speed'] ?? 1.0);
    // позиция — на момент timestamp_ms: досчитать до сейчас
    if (!empty($p['timestamp_ms']) && $sp > 0) $ms += max(0, microtime(true) * 1000 - (float)$p['timestamp_ms']) * $sp;
    setpos($ms, $sp);
}
function prog() {
    global $PG, $pos, $posAt, $speed, $status, $last;
    if ($last === '') return;
    $now = microtime(true);
    $ms = $pos + ($status === 'playing' && $speed > 0 ? ($now - $posAt) * 1000 * $speed : 0);
    wr($PG, (int)$ms . ' ' . (int)$now . ' ' . ($status === 'playing' ? 'play' : 'pause') . "\n");
}

$in = fopen('php://stdin', 'r');
while (($l = fgets($in)) !== false) {
    $sp = strpos($l, ' ');
    $j = json_decode($sp === false ? $l : substr($l, $sp + 1), true);
    if (!is_array($j)) continue;
    switch ($j['type'] ?? '') {
        case 'playback_state':
            if (isset($j['status'])) $status = $j['status'];
            if (!empty($j['item'])) item($j['item']);
            if (isset($j['position'])) position($j['position']);
            prog();
            break;
        case 'track_changed':
            item($j['item'] ?? null);
            prog();
            break;
        case 'playback_changed':
            if ($status === 'playing' || ($j['status'] ?? '') === 'playing') {
                // досчитать позицию до смены состояния, дальше — с новой скоростью
                $now = microtime(true);
                if ($status === 'playing') setpos($pos + ($now - $posAt) * 1000 * $speed, $speed);
            }
            $status = $j['status'] ?? $status;
            prog();
            break;
        case 'position_sync':
            position($j['position'] ?? null);
            prog();
            break;
    }
}
