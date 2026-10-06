<?php
// track.php — текущий трек для приложения Fox Remote и усилителя DigiD D1.
//   GET          -> {"source":"qobuz","artist":"...","title":"...","album":"...",
//                    "cover":"https://..."|"", "dur":245000, "pos":61000, "play":true} ({} — нет данных)
//                   dur/pos — мс; dur 0 — длительность неизвестна (радио)
//   GET ?prog=1  -> для усилителя: "позиция_с длительность_с 1|0" (пусто — нет трека)
//   GET ?amp=1   -> одна строка для экрана усилителя: "исполнитель\tназвание" в однобайтовой
//                   кодировке шрифта усилителя (ASCII + кириллица 0x80..0xBF), пусто — нет трека
// Источники: /tmp/nowplaying и /tmp/np_prog (пишет pfmeta для Qobuz, Spotify, AirPlay) и MPD
// (запрос здесь). Обложки и длительности, которых плеер не дал, ищутся в iTunes (pfmeta art).
// Положить на Фокс в /var/www/track.php.

$np_file = '/tmp/nowplaying';

function active_service() {
    $j = @json_decode(@file_get_contents('/tmp/system_status.json'), true);
    return is_array($j) && isset($j['active_service']) ? $j['active_service'] : '';
}

function mpd_track() {
    $s = @fsockopen('127.0.0.1', 6600, $en, $es, 1.0);
    if (!$s) return null;
    stream_set_timeout($s, 1);
    fgets($s);                                         // OK MPD x.y.z
    fwrite($s, "status\ncurrentsong\nclose\n");
    $kv = [];
    while (($l = fgets($s)) !== false) {
        $l = rtrim($l, "\n");
        if ($l === 'OK') continue;
        $p = strpos($l, ': ');
        if ($p !== false && !isset($kv[substr($l, 0, $p)])) $kv[substr($l, 0, $p)] = substr($l, $p + 2);
    }
    fclose($s);
    $state = $kv['state'] ?? '';
    if ($state !== 'play' && $state !== 'pause') return null;
    $title  = $kv['Title'] ?? '';
    $artist = $kv['Artist'] ?? ($kv['Name'] ?? '');     // радио: Name — станция, Title — что играет
    if ($title === '' && isset($kv['file'])) $title = preg_replace('/\.[^.\/]*$/', '', basename($kv['file']));
    $dur = (int)round(1000 * (float)($kv['duration'] ?? ($kv['Time'] ?? 0)));
    return ['source' => 'mpd', 'artist' => $artist, 'title' => $title, 'album' => $kv['Album'] ?? '',
            'cover' => '', 'dur' => $dur, 'pos' => (int)round(1000 * (float)($kv['elapsed'] ?? 0)),
            'play' => $state === 'play', 'radio' => $dur === 0];
}

function current_track($np_file) {
    $svc = active_service();
    if ($svc === 'mpd') return mpd_track();
    $l = @file($np_file, FILE_IGNORE_NEW_LINES);
    if (!$l || count($l) < 3) return null;
    if ($svc !== '' && $l[0] !== $svc) return null;    // устаревшее: плеер уже другой
    $t = ['source' => $l[0], 'artist' => $l[1], 'title' => $l[2], 'album' => $l[3] ?? '',
          'cover' => $l[4] ?? '', 'dur' => (int)($l[5] ?? 0), 'pos' => 0, 'play' => true, 'radio' => false];
    // позиция: "мс время_с play|pause" — где трек был в момент time; старше трека — не считается
    $p = preg_split('/\s+/', trim((string)@file_get_contents('/tmp/np_prog')));
    if (count($p) >= 3 && @filemtime('/tmp/np_prog') >= @filemtime($np_file) - 1) {
        $t['play'] = $p[2] === 'play';
        $t['pos'] = max(0, (int)$p[0] + ($t['play'] ? (time() - (int)$p[1]) * 1000 : 0));
    } else {
        $t['pos'] = max(0, (time() - (int)@filemtime($np_file)) * 1000);
    }
    return $t;
}

// UTF-8 -> кодировка шрифта усилителя
function amp_encode($s, $max) {
    static $map = null;
    if ($map === null) {
        $map = [];
        $cyr = 'АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмнопрстуфхцчшщъыьэюя';
        foreach (preg_split('//u', $cyr, -1, PREG_SPLIT_NO_EMPTY) as $i => $ch) $map[$ch] = chr(0x80 + $i);
        $alias = ['Ё' => 'Е', 'ё' => 'е', 'Є' => 'Е', 'є' => 'е', 'Ґ' => 'Г', 'ґ' => 'г', 'Ў' => 'У', 'ў' => 'у'];
        foreach ($alias as $k => $v) $map[$k] = $map[$v];
        $lat = ['І'=>'I','і'=>'i','Ї'=>'I','ї'=>'i','—'=>'-','–'=>'-','‐'=>'-','−'=>'-','«'=>'"','»'=>'"',
                '“'=>'"','”'=>'"','„'=>'"','‘'=>"'",'’'=>"'",'`'=>"'",'…'=>'...','×'=>'x','·'=>'-','•'=>'-',
                'À'=>'A','Á'=>'A','Â'=>'A','Ã'=>'A','Ä'=>'A','Å'=>'A','Æ'=>'AE','Ç'=>'C','È'=>'E','É'=>'E',
                'Ê'=>'E','Ë'=>'E','Ì'=>'I','Í'=>'I','Î'=>'I','Ï'=>'I','Ñ'=>'N','Ò'=>'O','Ó'=>'O','Ô'=>'O',
                'Õ'=>'O','Ö'=>'O','Ø'=>'O','Ù'=>'U','Ú'=>'U','Û'=>'U','Ü'=>'U','Ý'=>'Y','ß'=>'ss',
                'à'=>'a','á'=>'a','â'=>'a','ã'=>'a','ä'=>'a','å'=>'a','æ'=>'ae','ç'=>'c','è'=>'e','é'=>'e',
                'ê'=>'e','ë'=>'e','ì'=>'i','í'=>'i','î'=>'i','ï'=>'i','ñ'=>'n','ò'=>'o','ó'=>'o','ô'=>'o',
                'õ'=>'o','ö'=>'o','ø'=>'o','ù'=>'u','ú'=>'u','û'=>'u','ü'=>'u','ý'=>'y','ÿ'=>'y',
                'Ł'=>'L','ł'=>'l','Š'=>'S','š'=>'s','Ž'=>'Z','ž'=>'z','Č'=>'C','č'=>'c','Ř'=>'R','ř'=>'r',
                'Ő'=>'O','ő'=>'o','Ű'=>'U','ű'=>'u','Ş'=>'S','ş'=>'s','İ'=>'I','ı'=>'i','Ğ'=>'G','ğ'=>'g'];
        $map += $lat;
    }
    $out = '';
    foreach (preg_split('//u', (string)$s, -1, PREG_SPLIT_NO_EMPTY) ?: [] as $ch) {
        $o = ord($ch[0]);
        if (strlen($ch) === 1) $out .= ($o >= 0x20 && $o < 0x7F) ? $ch : ' ';
        else $out .= $map[$ch] ?? '?';
        if (strlen($out) >= $max) break;
    }
    return trim(substr($out, 0, $max));
}

// обложка и длительность, которых плеер не дал: iTunes по исполнителю и названию (в фоне,
// результат — в /tmp/np_art: ключ, обложка, мс; запрос — /tmp/np_art.req)
function fill_art(&$t) {
    if (!$t || $t['title'] === '' || ($t['cover'] !== '' && ($t['dur'] > 0 || $t['radio']))) return;
    $key = md5($t['artist'] . "\n" . $t['title']);
    $a = @file('/tmp/np_art', FILE_IGNORE_NEW_LINES);
    if ($a && $a[0] === $key) {
        if ($t['cover'] === '' && ($a[1] ?? '') !== '') $t['cover'] = $a[1];
        // длительность из поиска — только когда плеер её не знает (Qobuz); у радио её нет
        if ($t['dur'] <= 0 && !$t['radio'] && (int)($a[2] ?? 0) > 0) $t['dur'] = (int)$a[2];
        return;
    }
    if (trim((string)@file_get_contents('/tmp/np_art.req')) === $key) return;   // уже ищется
    @file_put_contents('/tmp/np_art.req', $key);
    $term = trim($t['artist'] . ' ' . $t['title']);
    $url = 'https://itunes.apple.com/search?media=music&entity=song&limit=1&term=' . rawurlencode($term);
    exec('/usr/bin/pfmeta art ' . escapeshellarg($key) . ' ' . escapeshellarg($url) . ' >/dev/null 2>&1 &');
}

$t = current_track($np_file);
if ($t) {
    fill_art($t);
    if ($t['dur'] > 0 && $t['pos'] > $t['dur']) $t['pos'] = $t['dur'];
}

if (isset($_GET['prog'])) {
    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: no-store');
    if ($t && $t['title'] !== '' && $t['dur'] > 0)
        echo intdiv($t['pos'], 1000) . ' ' . intdiv($t['dur'], 1000) . ' ' . ($t['play'] ? 1 : 0);
    exit;
}

if (isset($_GET['amp'])) {
    header('Content-Type: text/plain; charset=x-user-defined');
    header('Cache-Control: no-store');
    if ($t && ($t['title'] !== '' || $t['artist'] !== '')) echo amp_encode($t['artist'], 60) . "\t" . amp_encode($t['title'], 100);
    exit;
}

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');
echo json_encode($t ?: new stdClass(), JSON_UNESCAPED_UNICODE);
