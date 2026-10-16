<?php
// radio_lib.php — web radio (APlayer) helpers shared by radio.php (amp menu) and
// radio_edit.php (station editor page). Stations live in /usr/aplayer/Radio:
// folder = section, *.rad = station (file holds the stream URL), *.m3u = playlist of stations.
const RADIO_DIR  = '/usr/aplayer/Radio';
const RADIO_PAGE = 6;
const NAME_MAX   = 24;
const AP         = 'http://127.0.0.1:7778/';

function dirs() {
    $d = [];
    foreach (@scandir(RADIO_DIR) ?: [] as $e)
        if ($e[0] !== '.' && is_dir(RADIO_DIR . "/$e")) $d[] = $e;
    usort($d, 'strnatcasecmp');
    return $d;
}

// stations of a section: *.rad files (name = file name) and entries of *.m3u playlists
// (name = #EXTINF title), in the order APlayer shows them: .rad sorted, then playlists
function stations($dir) {
    $rad = []; $m3u = [];
    $files = @scandir(RADIO_DIR . "/$dir") ?: [];
    usort($files, 'strnatcasecmp');
    foreach ($files as $e) {
        if (preg_match('/\.rad$/i', $e)) { $rad[] = substr($e, 0, -4); continue; }
        if (!preg_match('/\.m3u8?$/i', $e)) continue;
        $title = '';
        foreach (@file(RADIO_DIR . "/$dir/$e", FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $l) {
            $l = trim($l, "\xEF\xBB\xBF \t\r");
            if (stripos($l, '#EXTINF') === 0) { $p = strpos($l, ','); $title = $p === false ? '' : trim(substr($l, $p + 1)); continue; }
            if ($l === '' || $l[0] === '#') continue;
            $m3u[] = $title !== '' ? $title : $l;
            $title = '';
        }
    }
    usort($rad, 'strnatcasecmp');
    return array_merge($rad, $m3u);
}

// UTF-8 -> short ASCII for the amp menu font
function amp_name($s) {
    static $tr = null;
    if ($tr === null) {
        $tr = [];
        $cyr = ['А'=>'A','Б'=>'B','В'=>'V','Г'=>'G','Д'=>'D','Е'=>'E','Ё'=>'E','Ж'=>'Zh','З'=>'Z','И'=>'I',
                'Й'=>'Y','К'=>'K','Л'=>'L','М'=>'M','Н'=>'N','О'=>'O','П'=>'P','Р'=>'R','С'=>'S','Т'=>'T',
                'У'=>'U','Ф'=>'F','Х'=>'Kh','Ц'=>'Ts','Ч'=>'Ch','Ш'=>'Sh','Щ'=>'Sch','Ъ'=>'','Ы'=>'Y','Ь'=>'',
                'Э'=>'E','Ю'=>'Yu','Я'=>'Ya','І'=>'I','Ї'=>'Yi','Є'=>'Ye','Ґ'=>'G','Ў'=>'U'];
        foreach ($cyr as $k => $v) {
            $tr[$k] = $v;
            $lk = mb_lc($k);
            $tr[$lk] = strtolower($v);
        }
        $tr += ['—'=>'-','–'=>'-','«'=>'"','»'=>'"','“'=>'"','”'=>'"','’'=>"'",'…'=>'...',
                'ä'=>'a','ö'=>'o','ü'=>'u','Ä'=>'A','Ö'=>'O','Ü'=>'U','ß'=>'ss','é'=>'e','è'=>'e',
                'ê'=>'e','à'=>'a','â'=>'a','ç'=>'c','ô'=>'o','î'=>'i','ñ'=>'n','ă'=>'a','â'=>'a',
                'ș'=>'s','ş'=>'s','ț'=>'t','ţ'=>'t','Ș'=>'S','Ț'=>'T','å'=>'a','Å'=>'A','ø'=>'o','æ'=>'ae'];
    }
    $s = strtr($s, $tr);
    $s = preg_replace('/[^\x20-\x7E]/', '', $s);
    // bitrate/format notes are noise on a small screen: "(128kbps)", "(320 kbps AAC)", "- AAC HD 256k"
    $s = preg_replace('/\s*\((?=[^)]*(kbps|aac|flac|mp3|ogg|\d+k\b))[^)]*\)/i', '', $s);
    $s = preg_replace('/\s*-\s*(aac|mp3|flac|ogg)\b.*$/i', '', $s);
    $s = trim(preg_replace('/\s+/', ' ', $s));
    if ($s === '') $s = '?';
    return strlen($s) > NAME_MAX ? rtrim(substr($s, 0, NAME_MAX - 1)) . '.' : $s;
}

// lowercase of one Cyrillic capital (no mbstring in this PHP)
function mb_lc($ch) {
    static $u = 'АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯІЇЄҐЎ';
    static $l = 'абвгдеёжзийклмнопрстуфхцчшщъыьэюяіїєґў';
    $U = preg_split('//u', $u, -1, PREG_SPLIT_NO_EMPTY);
    $L = preg_split('//u', $l, -1, PREG_SPLIT_NO_EMPTY);
    $i = array_search($ch, $U, true);
    return $i === false ? $ch : $L[$i];
}

// &lines=N: N lines of RADIO_PAGE names each (the amp keeps a window of 12)
function page($kind, $names, $from) {
    $total = count($names);
    $from  = max(0, min($from, $total));
    $lines = max(1, min(4, (int)($_GET['lines'] ?? 1)));
    $out   = [];
    for ($l = 0; $l < $lines; $l++) {
        $f = $from + $l * RADIO_PAGE;
        if ($l > 0 && $f >= $total) break;
        $o = "$kind $total $f";
        foreach (array_slice($names, $f, RADIO_PAGE) as $n) $o .= "\t" . amp_name($n);
        $out[] = $o;
    }
    echo implode("\n", $out);
}

// ---- APlayer web API ----
function ap($q, $timeout = 4) {
    $ctx = stream_context_create(['http' => ['timeout' => $timeout]]);
    $r = @file_get_contents(AP . '?' . $q, false, $ctx);
    if ($r === false) return null;
    $j = json_decode($r, true);
    return is_array($j) ? $j : [];
}
function norm($s) {
    $s = preg_replace('/\s*\[[^\]]*\]\s*$/', '', $s);        // "Section [12]" in radio mode
    $s = preg_replace('/\.rad$/i', '', $s);
    return strtolower(trim($s));
}
function find_idx($list, $name) {
    $n = norm($name);
    foreach ($list as $i => $v) if (norm((string)$v) === $n) return $i;
    foreach ($list as $i => $v) if (strpos(norm((string)$v), $n) === 0) return $i;
    foreach ($list as $i => $v) if ($n !== '' && strpos(norm((string)$v), $n) !== false) return $i;
    return -1;
}

const RADIO_LAST = '/etc/digifox/radio_last';           // "раздел\nстанция" — что играло последним

function play($di, $si) {
    $d = dirs();
    if (!isset($d[$di])) return 'ERR no_section';
    $s = stations($d[$di]);
    if (!isset($s[$si])) return 'ERR no_station';
    @touch('/tmp/radio_play_req');                         // radio-resume: выбор уже сделан
    return play_name($d[$di], $s[$si], true);
}

// включить станцию по названиям раздела и станции (индексы APlayer ищутся заново)
function play_name($section, $station, $switch) {
    // APlayer must be the active player (its web API runs only then)
    $st = @json_decode(@file_get_contents('/tmp/system_status.json'), true);
    $usb = file_exists('/etc/usb_to_i2s.state');
    if ($switch && ($usb || ($st['active_service'] ?? '') !== 'aplayer')) {
        $post = function ($url, $data) {
            $ctx = stream_context_create(['http' => ['method' => 'POST', 'timeout' => 60,
                'header' => 'Content-Type: application/x-www-form-urlencoded', 'content' => $data]]);
            return @file_get_contents("http://127.0.0.1/$url", false, $ctx);
        };
        if ($usb) $post('usb_to_i2s.php', 'action=disable');
        $post('handle_service.php', 'service=aplayer');
    }
    $state = null;
    for ($i = 0; $i < 40 && $state === null; $i++) {          // up to ~10 s for APlayer to start
        $state = ap('State', 2);
        if ($state === null) usleep(250000);
    }
    if ($state === null) return 'ERR aplayer_not_running';

    if (empty($state['RadioMode'])) { ap('RadioMode', 10); usleep(300000); }
    $al = ap('GetAlbums', 10);
    $a  = find_idx($al['Albums'] ?? [], $section);
    if ($a < 0) {                                              // list not in radio mode yet
        ap('RadioMode', 10); usleep(300000);
        $al = ap('GetAlbums', 10);
        $a  = find_idx($al['Albums'] ?? [], $section);
    }
    $so = $a >= 0 ? ap('GetSongs&album=' . $a, 10) : [];
    $k  = find_idx($so['Songs'] ?? [], $station);
    if ($a < 0 || $k < 0) {                                    // edited on the radio page: let APlayer rescan
        ap_rescan();
        $al = ap('GetAlbums', 10);
        $a  = find_idx($al['Albums'] ?? [], $section);
        if ($a < 0) return 'ERR section_not_in_aplayer';
        $so = ap('GetSongs&album=' . $a, 10);
        $k  = find_idx($so['Songs'] ?? [], $station);
    }
    if ($k < 0) return 'ERR station_not_in_aplayer';
    if (!empty($state['Playing'])) { ap('Stop', 5); usleep(200000); }
    $r = ap('Play&album=' . $a . '&song=' . $k, 15);
    if (($r['Result'] ?? '') !== 'OK') return 'ERR ' . preg_replace('/\s+/', '_', $r['Result'] ?? 'play_failed');
    radio_remember($section, $station);
    return 'OK ' . amp_name($station);
}

function radio_remember($section, $station) {
    $v = "$section\n$station\n";
    if (@file_get_contents(RADIO_LAST) === $v) return;        // флеш — только при смене
    @mkdir(dirname(RADIO_LAST), 0755, true);
    @file_put_contents(RADIO_LAST . '.tmp', $v) && @rename(RADIO_LAST . '.tmp', RADIO_LAST);
}

// после запуска APlayer (S95aplayer): если сам ничего не играет и станцию только что
// не выбрали с усилителя — включить последнюю
function resume() {
    $l = @file(RADIO_LAST, FILE_IGNORE_NEW_LINES);
    if (!$l || count($l) < 2) return 'ERR nothing_saved';
    $st = ap('State', 3);
    if ($st === null) return 'ERR aplayer_not_running';
    if (!empty($st['Playing'])) return 'OK already_playing';
    if (@filemtime('/tmp/radio_play_req') > time() - 30) return 'OK chosen';
    return play_name($l[0], $l[1], false);
}

function unfav() {                                             // playing station out of !Favorites
    $st = ap('State', 3);
    if ($st === null) return 'ERR aplayer_not_running';
    $name = (string)($st['PlayingName'] ?? '');
    if ($name === '') return 'ERR nothing_playing';
    $al = ap('GetAlbums', 10);
    $f = -1;
    foreach ($al['Albums'] ?? [] as $i => $a) if (stripos(norm((string)$a), '!favorites') === 0) { $f = $i; break; }
    if ($f < 0) return 'ERR no_favorites';
    $k = find_idx(ap('GetSongs&album=' . $f, 10)['Songs'] ?? [], $name);
    if ($k < 0) return 'ERR not_in_favorites';
    $r = ap('DeleteStation&album=' . $f . '&song=' . $k, 10);
    if ($r === null) return 'ERR delete_failed';
    // APlayer leaves radio mode listing after a change; keep its lists in radio mode
    $s2 = ap('State', 3);
    if ($s2 !== null && empty($s2['RadioMode'])) ap('RadioMode', 10);
    return 'OK';
}

// APlayer reads the Radio folder when it enters radio mode: leave it and come back
function ap_rescan() {
    $st = ap('State', 3);
    if ($st === null) return;
    if (!empty($st['RadioMode'])) { ap('RadioMode', 10); usleep(300000); }
    ap('RadioMode', 10); usleep(500000);
}
