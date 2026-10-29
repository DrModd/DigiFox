<?php
// amp_text.php — текст для экрана усилителя DigiD D1: ASCII + кириллица 0x80..0xBF
// (кодировка шрифтов усилителя); иероглифы, кана, хангыль — латиницей (amp_translit).
// Нужен track.php (трек) и radio_lib.php (списки меню). Без mbstring (в PHP Фокса его нет).

// код символа UTF-8 и обратно (вместо mb_ord / mb_chr)
function u_ord($c) {
    $b = array_values(unpack('C*', $c));
    $n = count($b);
    if ($n === 1) return $b[0];
    if ($n === 2) return (($b[0] & 0x1F) << 6) | ($b[1] & 0x3F);
    if ($n === 3) return (($b[0] & 0x0F) << 12) | (($b[1] & 0x3F) << 6) | ($b[2] & 0x3F);
    return (($b[0] & 0x07) << 18) | (($b[1] & 0x3F) << 12) | (($b[2] & 0x3F) << 6) | ($b[3] & 0x3F);
}
function u_chr($cp) {
    if ($cp < 0x80) return chr($cp);
    if ($cp < 0x800) return chr(0xC0 | ($cp >> 6)) . chr(0x80 | ($cp & 0x3F));
    if ($cp < 0x10000) return chr(0xE0 | ($cp >> 12)) . chr(0x80 | (($cp >> 6) & 0x3F)) . chr(0x80 | ($cp & 0x3F));
    return chr(0xF0 | ($cp >> 18)) . chr(0x80 | (($cp >> 12) & 0x3F)) . chr(0x80 | (($cp >> 6) & 0x3F)) . chr(0x80 | ($cp & 0x3F));
}

// Иероглифы, кана и хангыль -> латиница (у усилителя шрифт только латиница + кириллица):
//   китайский — пиньинь без тонов, по слогу на знак: 周杰伦 -> "Zhou Jie Lun"
//     (/usr/share/digifox/pinyin.bin: uint16 на знак U+3400..U+9FFF, номер слога из pinyin.syl;
//      данные pinyin-data, MIT); японские кандзи читаются так же — по-китайски
//   кана — ромадзи по Хэпбёрну: さくら -> "sakura", っ — удвоение, ー — повтор гласной
//   хангыль — упрощённая новая романизация по слогам: 아이유 -> "aiyu"
//   полноширинные ＡＢＣ１２３ и знаки 「」。、 -> ASCII
function amp_translit($s) {
    if (!preg_match('/[\x{3000}-\x{9FFF}\x{AC00}-\x{D7A3}\x{FF01}-\x{FF5E}]/u', $s)) return $s;
    static $kana = null, $syl = null, $fh = null;
    if ($kana === null) {
        $h = ['あ'=>'a','い'=>'i','う'=>'u','え'=>'e','お'=>'o','か'=>'ka','き'=>'ki','く'=>'ku','け'=>'ke','こ'=>'ko',
              'さ'=>'sa','し'=>'shi','す'=>'su','せ'=>'se','そ'=>'so','た'=>'ta','ち'=>'chi','つ'=>'tsu','て'=>'te','と'=>'to',
              'な'=>'na','に'=>'ni','ぬ'=>'nu','ね'=>'ne','の'=>'no','は'=>'ha','ひ'=>'hi','ふ'=>'fu','へ'=>'he','ほ'=>'ho',
              'ま'=>'ma','み'=>'mi','む'=>'mu','め'=>'me','も'=>'mo','や'=>'ya','ゆ'=>'yu','よ'=>'yo',
              'ら'=>'ra','り'=>'ri','る'=>'ru','れ'=>'re','ろ'=>'ro','わ'=>'wa','ゐ'=>'i','ゑ'=>'e','を'=>'o','ん'=>'n',
              'が'=>'ga','ぎ'=>'gi','ぐ'=>'gu','げ'=>'ge','ご'=>'go','ざ'=>'za','じ'=>'ji','ず'=>'zu','ぜ'=>'ze','ぞ'=>'zo',
              'だ'=>'da','ぢ'=>'ji','づ'=>'zu','で'=>'de','ど'=>'do','ば'=>'ba','び'=>'bi','ぶ'=>'bu','べ'=>'be','ぼ'=>'bo',
              'ぱ'=>'pa','ぴ'=>'pi','ぷ'=>'pu','ぺ'=>'pe','ぽ'=>'po','ぁ'=>'a','ぃ'=>'i','ぅ'=>'u','ぇ'=>'e','ぉ'=>'o',
              'ゃ'=>'ya','ゅ'=>'yu','ょ'=>'yo','ゎ'=>'wa','ゔ'=>'vu'];
        $kana = [];
        foreach ($h as $k => $v) {
            $kana[$k] = $v;
            $kana[u_chr(u_ord($k) + 0x60)] = $v;            // катакана — та же таблица со сдвигом
        }
        $syl = @file('/usr/share/digifox/pinyin.syl', FILE_IGNORE_NEW_LINES) ?: [];
    }
    $ch = preg_split('//u', $s, -1, PREG_SPLIT_NO_EMPTY);
    $out = ''; $n = count($ch); $prev_han = false;
    for ($i = 0; $i < $n; $i++) {
        $c = $ch[$i]; $cp = u_ord($c);
        // китайские иероглифы
        if ($cp >= 0x3400 && $cp <= 0x9FFF) {
            $py = '';
            if ($fh === null) $fh = @fopen('/usr/share/digifox/pinyin.bin', 'rb') ?: false;
            if ($fh) {
                fseek($fh, ($cp - 0x3400) * 2);
                $b = fread($fh, 2);
                $k = strlen($b) === 2 ? unpack('v', $b)[1] : 0;
                if ($k > 0 && isset($syl[$k - 1])) $py = ucfirst(str_replace('v', 'u', $syl[$k - 1]));
            }
            if ($py === '') $py = '?';
            if ($out !== '' && preg_match('/[A-Za-z0-9]$/', $out)) $out .= ' ';
            $out .= $py; $prev_han = true;
            continue;
        }
        if ($prev_han && ($cp >= 0x3040 || preg_match('/^[A-Za-z0-9]$/', $c))) $out .= ' ';   // после иероглифов — пробел перед словом
        $prev_han = false;
        // кана
        if (isset($kana[$c])) {
            $r = $kana[$c];
            $nx = $ch[$i + 1] ?? '';
            // きゃ -> kya, しゃ -> sha, ちゃ -> cha, じゃ -> ja
            if ($nx !== '' && in_array($nx, ['ゃ','ゅ','ょ','ャ','ュ','ョ'], true) && strlen($r) >= 2 && substr($r, -1) === 'i') {
                $y = $kana[$nx];
                $base = substr($r, 0, -1);
                $r = in_array($base, ['sh', 'ch', 'j'], true) ? $base . substr($y, 1) : $base . $y;
                $i++;
            }
            $out .= $r;
            continue;
        }
        if ($c === 'っ' || $c === 'ッ') {                 // удвоение следующей согласной
            $nx = $kana[$ch[$i + 1] ?? ''] ?? '';
            if ($nx !== '') $out .= $nx[0] === 'c' ? 't' : $nx[0];
            continue;
        }
        if ($c === 'ー') { $v = substr($out, -1); if (strpos('aeiou', $v) !== false) $out .= $v; continue; }
        // хангыль: слог = начальная + гласная + конечная
        if ($cp >= 0xAC00 && $cp <= 0xD7A3) {
            static $L = ['g','kk','n','d','tt','r','m','b','pp','s','ss','','j','jj','ch','k','t','p','h'];
            static $V = ['a','ae','ya','yae','eo','e','yeo','ye','o','wa','wae','oe','yo','u','wo','we','wi','yu','eu','ui','i'];
            static $T = ['','k','k','k','n','n','n','t','l','k','m','l','l','l','p','l','m','p','p','t','t','ng','t','t','k','t','p','t'];
            $x = $cp - 0xAC00;
            $out .= $L[intdiv($x, 588)] . $V[intdiv($x % 588, 28)] . $T[$x % 28];
            continue;
        }
        // полноширинные латиница/цифры/знаки и знаки CJK
        if ($cp >= 0xFF01 && $cp <= 0xFF5E) { $out .= chr($cp - 0xFEE0); continue; }
        static $punct = ['　'=>' ','、'=>', ','。'=>'. ','「'=>' "','」'=>'" ','『'=>' "','』'=>'" ','（'=>' (','）'=>') ',
                         '【'=>'[','】'=>']','《'=>' "','》'=>'" ','〈'=>'<','〉'=>'>','・'=>' ','〜'=>'~','～'=>'~'];
        if (isset($punct[$c])) { $out .= $punct[$c]; continue; }
        $out .= $c;
    }
    return trim(preg_replace('/ {2,}/', ' ', $out));
}

// UTF-8 -> кодировка шрифта усилителя
function amp_encode($s, $max) {
    $s = amp_translit((string)$s);
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

// Умеет ли прошивка усилителя кириллицу в списках меню (с 1.9); старой — транслит
function amp_menu_cyr() {
    static $c = null;
    if ($c === null) {
        $v = trim((string)@file_get_contents('/tmp/amp_ver'));
        $c = $v !== '' && version_compare($v, '1.9', '>=');
    }
    return $c;
}
