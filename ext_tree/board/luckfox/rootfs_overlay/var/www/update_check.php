<?php
// Страница «Обновление»: есть ли новая сборка DigiFox и что в ней нового.
//   GET update_check.php -> {installed, latest, newer, changes:[{ver, items[]}], amp_changes:[...]}
// latest и changes.txt — из последнего релиза на GitHub (DrModd/DigiFox);
// amp_changes — из /usr/share/digifox/amp-fw.changes (прошивка усилителя внутри Фокса).
require_once 'config.php';
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

const BASE = 'https://github.com/DrModd/DigiFox/releases/latest/download/';

function fetch_text($name) {
    $out = shell_exec('wget -q -T 15 -O - ' . escapeshellarg(BASE . $name) . ' 2>/dev/null');
    return $out === null ? '' : (string)$out;
}

// «## 1.0.128» + строки «- …» -> [{ver, items}] новее $after (и не новее $upto, если задано)
function parse_changes($text, $after, $upto = '') {
    $res = []; $cur = null;
    foreach (preg_split('/\r?\n/', $text) as $l) {
        if (preg_match('/^##\s+(\S+)/', $l, $m)) {
            $cur = null;
            $v = $m[1];
            if (!preg_match('/^\d+(\.\d+)+$/', $v)) continue;
            if ($after !== '' && version_compare($v, $after, '<=')) continue;
            if ($upto !== '' && version_compare($v, $upto, '>')) continue;
            $res[] = ['ver' => $v, 'items' => []];
            $cur = count($res) - 1;
        } elseif ($cur !== null && preg_match('/^\s*-\s+(.+)$/', $l, $m)) {
            $res[$cur]['items'][] = trim($m[1]);
        }
    }
    return $res;
}

$installed = VERSION;
$latest = trim(fetch_text('version.txt'));
$out = ['installed' => $installed, 'latest' => $latest !== '' ? $latest : null, 'online' => $latest !== ''];
$out['newer'] = $latest !== '' && preg_match('/^\d+(\.\d+)+$/', $latest) && version_compare($latest, $installed, '>');
$out['changes'] = $out['newer'] ? parse_changes(fetch_text('changes.txt'), $installed, $latest) : [];

$amp_ver = trim((string)@file_get_contents('/tmp/amp_ver'));
$amp_changes = (string)@file_get_contents('/usr/share/digifox/amp-fw.changes');
$out['amp_changes'] = $amp_ver !== '' ? parse_changes($amp_changes, $amp_ver) : [];

echo json_encode($out, JSON_UNESCAPED_UNICODE);
