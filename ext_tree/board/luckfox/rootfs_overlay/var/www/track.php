<?php
// track.php — текущий трек для приложения Fox Remote и усилителя DigiD D1.
//   GET          -> {"source":"qobuz","artist":"...","title":"...","album":"..."} ({} — нет данных)
//   GET ?amp=1   -> одна строка для экрана усилителя: "исполнитель\tназвание" в однобайтовой
//                   кодировке шрифта усилителя (ASCII + кириллица 0x80..0xBF), пусто — нет трека
// Источники: /tmp/nowplaying (пишет pfmeta для Qobuz, Spotify, AirPlay) и MPD (запрос здесь).
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
    if (($kv['state'] ?? '') !== 'play') return null;
    $title  = $kv['Title'] ?? '';
    $artist = $kv['Artist'] ?? ($kv['Name'] ?? '');     // радио: Name — станция, Title — что играет
    if ($title === '' && isset($kv['file'])) $title = preg_replace('/\.[^.\/]*$/', '', basename($kv['file']));
    return ['source' => 'mpd', 'artist' => $artist, 'title' => $title, 'album' => $kv['Album'] ?? ''];
}

function current_track($np_file) {
    $svc = active_service();
    if ($svc === 'mpd') return mpd_track();
    $l = @file($np_file, FILE_IGNORE_NEW_LINES);
    if (!$l || count($l) < 3) return null;
    if ($svc !== '' && $l[0] !== $svc) return null;    // устаревшее: плеер уже другой
    return ['source' => $l[0], 'artist' => $l[1], 'title' => $l[2], 'album' => $l[3] ?? ''];
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

$t = current_track($np_file);

if (isset($_GET['amp'])) {
    header('Content-Type: text/plain; charset=x-user-defined');
    header('Cache-Control: no-store');
    if ($t && ($t['title'] !== '' || $t['artist'] !== '')) echo amp_encode($t['artist'], 60) . "\t" . amp_encode($t['title'], 100);
    exit;
}

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');
echo json_encode($t ?: new stdClass(), JSON_UNESCAPED_UNICODE);
