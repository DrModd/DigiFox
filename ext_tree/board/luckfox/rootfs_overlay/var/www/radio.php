<?php
// Web radio (APlayer) for the amplifier menu (pfctl "radio ..." commands).
//
//   GET ?dirs&from=F          -> "d <total> <from>\t<name>\t..."   sections of APlayer's Radio folder
//   GET ?st&dir=D&from=F      -> "s <total> <from>\t<name>\t..."   stations of section D
//   GET ?play&dir=D&st=S      -> "OK <name>" | "ERR <reason>"      switch to APlayer and play it
//   GET ?fav                  -> "OK" | "ERR <reason>"             playing station -> !Favorites
//   GET ?unfav                -> "OK" | "ERR <reason>"             playing station out of !Favorites
//   GET ?resume               -> "OK ..." | "ERR <reason>"         play the last station (after APlayer starts)
//
// Lists come from /usr/aplayer/Radio (folders = sections, *.rad = stations), RADIO_PAGE
// names per answer, transliterated to ASCII and shortened for the amp screen. To play,
// the station is found in APlayer by name (its own indices) through its web API on :7778.
require_once __DIR__ . '/radio_lib.php';
header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');

if (isset($_GET['resume'])) {
    echo resume();
} elseif (isset($_GET['unfav'])) {
    echo unfav();
} elseif (isset($_GET['fav'])) {                                     // playing station -> !Favorites
    $r = ap('Favor', 5);
    echo $r === null ? 'ERR aplayer_not_running' : ((($r['Result'] ?? 'OK') === 'OK') ? 'OK' : 'ERR ' . preg_replace('/\s+/', '_', $r['Result']));
} elseif (isset($_GET['play'])) {                                    // first: it also has "st"
    echo play((int)($_GET['dir'] ?? 0), (int)($_GET['st'] ?? 0));
} elseif (isset($_GET['dirs'])) {
    page('d', dirs(), (int)($_GET['from'] ?? 0));
} elseif (isset($_GET['st'])) {
    $d = dirs();
    $di = (int)($_GET['dir'] ?? 0);
    page('s', isset($d[$di]) ? stations($d[$di]) : [], (int)($_GET['from'] ?? 0));
} else {
    echo 'ERR usage';
}
