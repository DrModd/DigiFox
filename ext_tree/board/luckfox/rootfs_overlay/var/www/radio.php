<?php
// Web radio (APlayer) for the amplifier menu (pfctl "radio ..." commands).
//
//   GET ?dirs&from=F          -> "d <total> <from>\t<name>\t..."   sections of APlayer's Radio folder
//   GET ?st&dir=D&from=F      -> "s <total> <from>\t<name>\t..."   stations of section D
//   GET ?play&dir=D&st=S      -> "OK <name>" | "ERR <reason>"      switch to APlayer and play it
//
// Lists come from /usr/aplayer/Radio (folders = sections, *.rad = stations), RADIO_PAGE
// names per answer, transliterated to ASCII and shortened for the amp screen. To play,
// the station is found in APlayer by name (its own indices) through its web API on :7778.
header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');

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

function page($kind, $names, $from) {
    $total = count($names);
    $from  = max(0, min($from, $total));
    $out   = "$kind $total $from";
    foreach (array_slice($names, $from, RADIO_PAGE) as $n) $out .= "\t" . amp_name($n);
    echo $out;
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
    return -1;
}

function play($di, $si) {
    $d = dirs();
    if (!isset($d[$di])) return 'ERR no_section';
    $s = stations($d[$di]);
    if (!isset($s[$si])) return 'ERR no_station';

    // APlayer must be the active player (its web API runs only then)
    $st = @json_decode(@file_get_contents('/tmp/system_status.json'), true);
    $usb = file_exists('/etc/usb_to_i2s.state');
    if ($usb || ($st['active_service'] ?? '') !== 'aplayer') {
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
    $a  = find_idx($al['Albums'] ?? [], $d[$di]);
    if ($a < 0) {                                              // list not in radio mode yet
        ap('RadioMode', 10); usleep(300000);
        $al = ap('GetAlbums', 10);
        $a  = find_idx($al['Albums'] ?? [], $d[$di]);
    }
    if ($a < 0) return 'ERR section_not_in_aplayer';
    $so = ap('GetSongs&album=' . $a, 10);
    $k  = find_idx($so['Songs'] ?? [], $s[$si]);
    if ($k < 0) return 'ERR station_not_in_aplayer';
    if (!empty($state['Playing'])) { ap('Stop', 5); usleep(200000); }
    $r = ap('Play&album=' . $a . '&song=' . $k, 15);
    return (($r['Result'] ?? '') === 'OK') ? 'OK ' . amp_name($s[$si]) : 'ERR ' . preg_replace('/\s+/', '_', $r['Result'] ?? 'play_failed');
}

if (isset($_GET['dirs'])) {
    page('d', dirs(), (int)($_GET['from'] ?? 0));
} elseif (isset($_GET['st'])) {
    $d = dirs();
    $di = (int)($_GET['dir'] ?? 0);
    page('s', isset($d[$di]) ? stations($d[$di]) : [], (int)($_GET['from'] ?? 0));
} elseif (isset($_GET['play'])) {
    echo play((int)($_GET['dir'] ?? 0), (int)($_GET['st'] ?? 0));
} else {
    echo 'ERR usage';
}
