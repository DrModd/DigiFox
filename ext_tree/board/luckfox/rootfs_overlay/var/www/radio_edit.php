<?php
// radio_edit.php — редактор станций веб-радио (APlayer) в веб-интерфейсе DigiFox.
//   GET                          -> страница
//   GET  ?api=sections           -> [{"name":..,"n":..}]
//   GET  ?api=stations&sec=S     -> {"stations":[{"name","url","kind":"rad|m3u","file"}],"playlists":[{"file","n"}]}
//   POST op=add      sec name url        станция (.rad) в раздел
//   POST op=edit     sec old name url to переименовать / новый адрес / в другой раздел
//   POST op=del      sec name
//   POST op=sec_add  name | op=sec_ren old name | op=sec_del name (со всеми станциями)
//   POST op=m3u      sec + файл f         загрузить плейлист .m3u в раздел
//   POST op=m3u_del  sec file
//   POST op=check    url                  отвечает ли поток (первые байты, тип, icy-name)
//   POST op=play     sec name             включить станцию
// Ответы POST: {"ok":true|false,"msg":".."}
// Раздел — папка /usr/aplayer/Radio/<раздел>, станция — файл <название>.rad с адресом потока.
// После правок обновления Фокса список больше не трогают (метка /etc/digifox/radio_edited).
require_once 'config.php';
require_once __DIR__ . '/radio_lib.php';

const EDITED_MARK = '/etc/digifox/radio_edited';

function out($a) {
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo json_encode($a, JSON_UNESCAPED_UNICODE);
    exit;
}
function fail($m) { out(['ok' => false, 'msg' => $m]); }

// имя раздела/станции -> безопасное имя файла (без / \ и управляющих символов)
function safe_name($s) {
    $s = trim(preg_replace('/[\x00-\x1F\x7F\/\\\\]+/', '-', (string)$s));
    $s = ltrim($s, '.');
    if (strlen($s) > 120) $s = rtrim(substr($s, 0, 120));
    return $s;
}
function sec_path($sec) {
    $n = safe_name($sec);
    if ($n === '' || $n !== $sec) return null;
    $p = RADIO_DIR . '/' . $n;
    return is_dir($p) ? $p : null;
}
function good_url($u) { return preg_match('#^https?://\S+$#i', trim((string)$u)); }

function edited() {
    @mkdir(dirname(EDITED_MARK), 0755, true);
    @touch(EDITED_MARK);
    // APlayer перечитает папку, если сейчас ничего не играет (иначе — при следующем
    // включении станции: play_name сам попросит пересканировать)
    $st = ap('State', 2);
    if ($st !== null && empty($st['Playing'])) ap_rescan();
}

function m3u_entries($file) {
    $out = []; $title = '';
    foreach (@file($file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $l) {
        $l = trim($l, "\xEF\xBB\xBF \t\r");
        if (stripos($l, '#EXTINF') === 0) { $p = strpos($l, ','); $title = $p === false ? '' : trim(substr($l, $p + 1)); continue; }
        if ($l === '' || $l[0] === '#') continue;
        $out[] = ['name' => $title !== '' ? $title : $l, 'url' => $l];
        $title = '';
    }
    return $out;
}

// ---------------- API ----------------
if (isset($_GET['api'])) {
    if ($_GET['api'] === 'sections') {
        $r = [];
        foreach (dirs() as $d) $r[] = ['name' => $d, 'n' => count(stations($d))];
        out($r);
    }
    if ($_GET['api'] === 'stations') {
        $p = sec_path((string)($_GET['sec'] ?? ''));
        if (!$p) out(['stations' => [], 'playlists' => []]);
        $files = @scandir($p) ?: [];
        usort($files, 'strnatcasecmp');
        $st = []; $pl = [];
        foreach ($files as $f) {
            if (preg_match('/\.rad$/i', $f)) {
                $url = trim((string)@file_get_contents("$p/$f"));
                $url = strtok($url, "\r\n") ?: '';
                $st[] = ['name' => substr($f, 0, -4), 'url' => $url, 'kind' => 'rad', 'file' => $f];
            } elseif (preg_match('/\.m3u8?$/i', $f)) {
                $e = m3u_entries("$p/$f");
                $pl[] = ['file' => $f, 'n' => count($e)];
                foreach ($e as $x) $st[] = ['name' => $x['name'], 'url' => $x['url'], 'kind' => 'm3u', 'file' => $f];
            }
        }
        out(['stations' => $st, 'playlists' => $pl]);
    }
    fail('bad api');
}

if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['op'])) {
    $op = $_POST['op'];
    $P = function ($k) { return trim((string)($_POST[$k] ?? '')); };

    if ($op === 'check') {
        $u = $P('url');
        if (!good_url($u)) fail('Адрес должен начинаться с http:// или https://');
        $h = tempnam('/tmp', 'rchk');
        $cmd = 'timeout 8 wget -S -T 6 -O - ' . escapeshellarg($u) . ' 2>' . escapeshellarg($h) . ' | head -c 32768 | wc -c';
        $bytes = (int)trim((string)@shell_exec($cmd));
        $hdr = (string)@file_get_contents($h); @unlink($h);
        preg_match('/content-type:\s*([^\r\n;]+)/i', $hdr, $m1);
        preg_match('/icy-name:\s*([^\r\n]+)/i', $hdr, $m2);
        $type = trim($m1[1] ?? '');
        $ok = $bytes >= 4096 && !preg_match('#text/html#i', $type);
        out(['ok' => $ok, 'bytes' => $bytes, 'type' => $type, 'icy' => trim($m2[1] ?? ''),
             'msg' => $ok ? 'Станция отвечает' . ($type ? " ($type)" : '') :
                    ($bytes ? 'Отвечает, но это не похоже на аудиопоток' . ($type ? " ($type)" : '') : 'Нет ответа')]);
    }

    if ($op === 'play') {
        $r = play_name($P('sec'), $P('name'), true);
        out(['ok' => strpos($r, 'OK') === 0, 'msg' => $r]);
    }

    if ($op === 'add' || $op === 'edit') {
        $sec = $P('sec');
        $p = sec_path($sec);
        if (!$p) fail('Нет такого раздела');
        $name = safe_name($P('name'));
        $url = $P('url');
        if ($name === '') fail('Введите название станции');
        if (!good_url($url)) fail('Адрес должен начинаться с http:// или https://');
        $to = $op === 'edit' ? sec_path($P('to') !== '' ? $P('to') : $sec) : $p;
        if (!$to) fail('Нет такого раздела');
        $dst = "$to/$name.rad";
        if ($op === 'edit') {
            $old = safe_name($P('old'));
            $src = "$p/$old.rad";
            if (!is_file($src)) fail('Станция не найдена');
            if ($dst !== $src && file_exists($dst)) fail('Станция с таким названием уже есть');
            if (@file_put_contents("$dst.tmp", $url . "\n") === false || !@rename("$dst.tmp", $dst)) fail('Не удалось сохранить');
            if ($dst !== $src) @unlink($src);
        } else {
            if (file_exists($dst)) fail('Станция с таким названием уже есть');
            if (@file_put_contents($dst, $url . "\n") === false) fail('Не удалось сохранить');
        }
        edited();
        out(['ok' => true, 'msg' => 'Сохранено']);
    }

    if ($op === 'del') {
        $p = sec_path($P('sec'));
        $f = $p ? "$p/" . safe_name($P('name')) . '.rad' : '';
        if (!$p || !is_file($f)) fail('Станция не найдена');
        if (!@unlink($f)) fail('Не удалось удалить');
        edited();
        out(['ok' => true, 'msg' => 'Удалено']);
    }

    if ($op === 'sec_add') {
        $n = safe_name($P('name'));
        if ($n === '') fail('Введите название раздела');
        if (file_exists(RADIO_DIR . "/$n")) fail('Такой раздел уже есть');
        if (!@mkdir(RADIO_DIR . "/$n", 0755, true)) fail('Не удалось создать');
        edited();
        out(['ok' => true, 'msg' => 'Раздел создан', 'name' => $n]);
    }

    if ($op === 'sec_ren') {
        $p = sec_path($P('old'));
        $n = safe_name($P('name'));
        if (!$p) fail('Нет такого раздела');
        if ($n === '') fail('Введите название раздела');
        if (file_exists(RADIO_DIR . "/$n")) fail('Такой раздел уже есть');
        if (!@rename($p, RADIO_DIR . "/$n")) fail('Не удалось переименовать');
        edited();
        out(['ok' => true, 'msg' => 'Переименовано', 'name' => $n]);
    }

    if ($op === 'sec_del') {
        $p = sec_path($P('name'));
        if (!$p) fail('Нет такого раздела');
        foreach (@scandir($p) ?: [] as $f) if ($f !== '.' && $f !== '..') @unlink("$p/$f");
        if (!@rmdir($p)) fail('Не удалось удалить раздел (внутри есть папки?)');
        edited();
        out(['ok' => true, 'msg' => 'Раздел удалён']);
    }

    if ($op === 'm3u') {
        $p = sec_path($P('sec'));
        if (!$p) fail('Нет такого раздела');
        $up = $_FILES['f'] ?? null;
        if (!$up || $up['error'] !== UPLOAD_ERR_OK) fail('Файл не загрузился');
        $n = safe_name(basename((string)$up['name']));
        if (!preg_match('/\.m3u8?$/i', $n)) $n .= '.m3u';
        $e = m3u_entries($up['tmp_name']);
        if (!$e) fail('В файле нет станций');
        if (!@move_uploaded_file($up['tmp_name'], "$p/$n")) fail('Не удалось сохранить');
        edited();
        out(['ok' => true, 'msg' => 'Плейлист добавлен: ' . count($e) . ' станций']);
    }

    if ($op === 'm3u_del') {
        $p = sec_path($P('sec'));
        $f = $p ? "$p/" . safe_name($P('file')) : '';
        if (!$p || !is_file($f) || !preg_match('/\.m3u8?$/i', $f)) fail('Плейлист не найден');
        if (!@unlink($f)) fail('Не удалось удалить');
        edited();
        out(['ok' => true, 'msg' => 'Плейлист удалён']);
    }
    fail('bad op');
}
?>
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#101113" media="(prefers-color-scheme: dark)">
    <meta name="theme-color" content="#eef0f2" media="(prefers-color-scheme: light)">
    <title>DigiFox — радиостанции</title>
    <link rel="icon" href="favicon.svg" type="image/svg+xml">
    <link rel="icon" href="favicon.ico" sizes="any">
    <link rel="stylesheet" href="assets/css/digifox.css?v=<?php echo htmlspecialchars(VERSION); ?>">
    <style>
        input.txt {
            width: 100%; min-width: 0; height: 46px; padding: 0 10px; margin-top: 6px;
            background: var(--raise); color: var(--fg); border: 1px solid var(--line); border-radius: 4px;
            font: inherit; font-size: 16px; box-sizing: border-box;
        }
        input.txt:focus-visible { outline: 2px solid var(--dim); outline-offset: 2px; }
        .lbl { color: var(--dim); font-size: 12px; margin-top: 10px; }
        .st-list { margin-top: 8px; }
        .st { display: flex; align-items: center; gap: 8px; padding: 9px 0; border-top: 1px solid var(--line); }
        .st:first-child { border-top: 0; }
        .st .nm { flex: 1; min-width: 0; }
        .st .nm b { display: block; font-weight: 500; font-size: 15px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .st .nm span { display: block; color: var(--dim); font-size: 11px; font-family: var(--mono);
                       overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .st .ib { flex: none; width: 40px; height: 36px; padding: 0; letter-spacing: 0; font-size: 15px; }
        .tag { color: var(--dim); font-size: 10px; letter-spacing: 1px; text-transform: uppercase; }
        .msg { min-height: 18px; margin-top: 8px; font-size: 13px; color: var(--dim); }
        .msg.ok { color: var(--live); }
        .msg.err { color: #e5675e; }
        .empty { color: var(--dim); font-size: 14px; padding: 10px 0; }
        .count { color: var(--dim); font-size: 12px; margin-left: 6px; }
        .row3 .hifi, .row2 .hifi { letter-spacing: .3px; padding: 0 4px; }
    </style>
</head>
<body>
<main class="wrap">
    <header class="head">
        <a class="back" href="index.php" aria-label="Назад">
            <svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path d="M15 5l-7 7 7 7" stroke="currentColor" stroke-width="1.6" fill="none"/></svg>
        </a>
        <div class="brand">РАДИОСТАНЦИИ</div>
        <span class="dot"></span>
    </header>

    <section class="panel">
        <div class="cap">РАЗДЕЛ</div>
        <select id="sec"></select>
        <div class="row3">
            <button class="hifi" id="sec-add">НОВЫЙ</button>
            <button class="hifi" id="sec-ren" title="Переименовать раздел">НАЗВАНИЕ</button>
            <button class="hifi" id="sec-del">УДАЛИТЬ</button>
        </div>
        <div class="msg" id="sec-msg"></div>
    </section>

    <section class="panel">
        <div class="cap">СТАНЦИИ<span class="count" id="st-count"></span></div>
        <input class="txt" id="filter" type="search" placeholder="Поиск в разделе" autocomplete="off">
        <div class="st-list" id="list"></div>
        <div class="msg" id="list-msg"></div>
    </section>

    <section class="panel">
        <div class="cap">ДОБАВИТЬ СТАНЦИЮ В РАЗДЕЛ</div>
        <div class="lbl">Название</div>
        <input class="txt" id="add-name" type="text" autocomplete="off">
        <div class="lbl">Адрес потока (http://… или https://…)</div>
        <input class="txt" id="add-url" type="url" autocomplete="off" inputmode="url">
        <div class="row2">
            <button class="hifi" id="add-check">ПРОВЕРИТЬ</button>
            <button class="hifi" id="add-ok">ДОБАВИТЬ</button>
        </div>
        <div class="msg" id="add-msg"></div>
    </section>

    <section class="panel">
        <div class="cap">ПЛЕЙЛИСТЫ .M3U В РАЗДЕЛЕ</div>
        <div id="pl"></div>
        <label class="file" id="m3u-label">Загрузить .m3u в этот раздел…<input type="file" id="m3u" accept=".m3u,.m3u8,audio/x-mpegurl" hidden></label>
        <div class="msg" id="pl-msg"></div>
        <div class="hint">Станции из плейлиста видны в списке с пометкой «плейлист». Править их можно только целиком: удалить плейлист и загрузить исправленный.</div>
    </section>

    <section class="panel">
        <div class="hint">Изменения сразу видны в меню усилителя (SOURCE → WEB RADIO). Обновления Фокса после правок здесь список больше не трогают.
            Порядок — по алфавиту; раздел «!Favorites» — избранное (в него добавляет кнопка «+» APlayer и пункт ADD TO FAVORITES усилителя).</div>
    </section>

    <footer class="foot">DigiFox v<?php echo htmlspecialchars(VERSION); ?></footer>
</main>

<!-- правка станции -->
<div class="sheet" id="ed" hidden>
    <div class="sheet-box wide">
        <div class="cap">СТАНЦИЯ</div>
        <div class="lbl">Название</div>
        <input class="txt" id="ed-name" type="text" autocomplete="off">
        <div class="lbl">Адрес потока</div>
        <input class="txt" id="ed-url" type="url" autocomplete="off" inputmode="url">
        <div class="lbl">Раздел</div>
        <select id="ed-sec"></select>
        <div class="row3">
            <button class="hifi" id="ed-check">ПРОВЕРИТЬ</button>
            <button class="hifi" id="ed-cancel">ОТМЕНА</button>
            <button class="hifi on" id="ed-ok">СОХРАНИТЬ</button>
        </div>
        <div class="msg" id="ed-msg"></div>
    </div>
</div>

<script>
(function () {
    var $ = function (id) { return document.getElementById(id); };
    var secs = [], cur = '', stations = [], editing = null;

    function msg(id, text, cls) { var e = $(id); e.textContent = text || ''; e.className = 'msg' + (cls ? ' ' + cls : ''); }
    function post(data, file) {
        var fd = new FormData();
        for (var k in data) fd.append(k, data[k]);
        if (file) fd.append('f', file);
        return fetch('radio_edit.php', { method: 'POST', body: fd, cache: 'no-store' })
            .then(function (r) { return r.json(); })
            .catch(function () { return { ok: false, msg: 'Нет связи с Фоксом' }; });
    }
    function getj(q) { return fetch('radio_edit.php?' + q, { cache: 'no-store' }).then(function (r) { return r.json(); }); }

    function fillSel(sel, value) {
        sel.innerHTML = '';
        secs.forEach(function (s) {
            var o = document.createElement('option');
            o.value = s.name; o.textContent = s.name + '  (' + s.n + ')';
            if (s.name === value) o.selected = true;
            sel.appendChild(o);
        });
    }
    function loadSections(select) {
        return getj('api=sections').then(function (r) {
            secs = r || [];
            if (select !== undefined) cur = select;
            if (!secs.some(function (s) { return s.name === cur; })) cur = secs.length ? secs[0].name : '';
            fillSel($('sec'), cur);
            try { localStorage.setItem('radio_sec', cur); } catch (e) {}
            return loadStations();
        });
    }
    function loadStations() {
        if (!cur) { stations = []; render(); renderPl([]); return Promise.resolve(); }
        return getj('api=stations&sec=' + encodeURIComponent(cur)).then(function (r) {
            stations = r.stations || [];
            render();
            renderPl(r.playlists || []);
        });
    }

    function btn(text, title, fn) {
        var b = document.createElement('button');
        b.className = 'hifi ib'; b.textContent = text; b.title = title; b.setAttribute('aria-label', title);
        b.onclick = fn; return b;
    }
    function render() {
        var f = $('filter').value.trim().toLowerCase(), list = $('list');
        list.innerHTML = '';
        var shown = 0;
        stations.forEach(function (s) {
            if (f && s.name.toLowerCase().indexOf(f) < 0 && s.url.toLowerCase().indexOf(f) < 0) return;
            shown++;
            var row = document.createElement('div'); row.className = 'st';
            var nm = document.createElement('div'); nm.className = 'nm';
            var b = document.createElement('b'); b.textContent = s.name; b.title = s.name;
            var u = document.createElement('span');
            u.textContent = (s.kind === 'm3u' ? 'плейлист ' + s.file + ' · ' : '') + s.url; u.title = s.url;
            nm.appendChild(b); nm.appendChild(u); row.appendChild(nm);
            row.appendChild(btn('▶', 'Включить', function () { play(s); }));
            if (s.kind === 'rad') {
                row.appendChild(btn('✎', 'Изменить', function () { edit(s); }));
                row.appendChild(btn('✕', 'Удалить', function () { del(s); }));
            }
            list.appendChild(row);
        });
        $('st-count').textContent = stations.length ? '  ' + stations.length : '';
        if (!shown) {
            var e = document.createElement('div'); e.className = 'empty';
            e.textContent = stations.length ? 'Ничего не найдено' : 'В разделе пока нет станций';
            list.appendChild(e);
        }
    }
    function renderPl(pl) {
        var box = $('pl'); box.innerHTML = '';
        pl.forEach(function (p) {
            var row = document.createElement('div'); row.className = 'st';
            var nm = document.createElement('div'); nm.className = 'nm';
            var b = document.createElement('b'); b.textContent = p.file;
            var u = document.createElement('span'); u.textContent = p.n + ' станций';
            nm.appendChild(b); nm.appendChild(u); row.appendChild(nm);
            row.appendChild(btn('✕', 'Удалить плейлист', function () {
                if (!confirm('Удалить плейлист «' + p.file + '» и все его станции (' + p.n + ')?')) return;
                post({ op: 'm3u_del', sec: cur, file: p.file }).then(function (r) {
                    msg('pl-msg', r.msg, r.ok ? 'ok' : 'err'); if (r.ok) loadSections();
                });
            }));
            box.appendChild(row);
        });
    }

    function play(s) {
        msg('list-msg', 'Включаю «' + s.name + '»…');
        post({ op: 'play', sec: cur, name: s.name }).then(function (r) {
            msg('list-msg', r.ok ? 'Играет: ' + s.name : 'Не включилась: ' + r.msg, r.ok ? 'ok' : 'err');
        });
    }
    function del(s) {
        if (!confirm('Удалить станцию «' + s.name + '»?')) return;
        post({ op: 'del', sec: cur, name: s.name }).then(function (r) {
            msg('list-msg', r.msg, r.ok ? 'ok' : 'err'); if (r.ok) loadSections();
        });
    }
    function edit(s) {
        editing = s;
        $('ed-name').value = s.name; $('ed-url').value = s.url;
        fillSel($('ed-sec'), cur);
        msg('ed-msg', '');
        $('ed').hidden = false;
        $('ed-name').focus();
    }
    function check(url, out) {
        msg(out, 'Проверяю…');
        post({ op: 'check', url: url }).then(function (r) {
            msg(out, r.msg + (r.icy ? ' — ' + r.icy : ''), r.ok ? 'ok' : 'err');
        });
    }

    $('sec').onchange = function () {
        cur = this.value; $('filter').value = '';
        try { localStorage.setItem('radio_sec', cur); } catch (e) {}
        msg('list-msg', ''); msg('pl-msg', ''); loadStations();
    };
    $('filter').oninput = render;

    $('sec-add').onclick = function () {
        var n = prompt('Название нового раздела:'); if (!n) return;
        post({ op: 'sec_add', name: n }).then(function (r) {
            msg('sec-msg', r.msg, r.ok ? 'ok' : 'err'); if (r.ok) loadSections(r.name);
        });
    };
    $('sec-ren').onclick = function () {
        if (!cur) return;
        var n = prompt('Новое название раздела:', cur); if (!n || n === cur) return;
        post({ op: 'sec_ren', old: cur, name: n }).then(function (r) {
            msg('sec-msg', r.msg, r.ok ? 'ok' : 'err'); if (r.ok) loadSections(r.name);
        });
    };
    $('sec-del').onclick = function () {
        if (!cur) return;
        if (!confirm('Удалить раздел «' + cur + '» вместе со всеми станциями (' + stations.length + ')?')) return;
        post({ op: 'sec_del', name: cur }).then(function (r) {
            msg('sec-msg', r.msg, r.ok ? 'ok' : 'err'); if (r.ok) loadSections('');
        });
    };

    $('add-check').onclick = function () { check($('add-url').value, 'add-msg'); };
    $('add-ok').onclick = function () {
        post({ op: 'add', sec: cur, name: $('add-name').value, url: $('add-url').value }).then(function (r) {
            msg('add-msg', r.msg, r.ok ? 'ok' : 'err');
            if (r.ok) { $('add-name').value = ''; $('add-url').value = ''; loadSections(); }
        });
    };

    $('ed-check').onclick = function () { check($('ed-url').value, 'ed-msg'); };
    $('ed-cancel').onclick = function () { $('ed').hidden = true; };
    $('ed').onclick = function (e) { if (e.target === this) this.hidden = true; };
    $('ed-ok').onclick = function () {
        post({ op: 'edit', sec: cur, old: editing.name, name: $('ed-name').value,
               url: $('ed-url').value, to: $('ed-sec').value }).then(function (r) {
            msg('ed-msg', r.msg, r.ok ? 'ok' : 'err');
            if (r.ok) { $('ed').hidden = true; msg('list-msg', 'Сохранено', 'ok'); loadSections(); }
        });
    };

    $('m3u').onchange = function () {
        var f = this.files[0]; if (!f || !cur) return;
        msg('pl-msg', 'Загружаю…');
        post({ op: 'm3u', sec: cur }, f).then(function (r) {
            msg('pl-msg', r.msg, r.ok ? 'ok' : 'err'); $('m3u').value = ''; if (r.ok) loadSections();
        });
    };

    var saved = ''; try { saved = localStorage.getItem('radio_sec') || ''; } catch (e) {}
    loadSections(saved);
})();
</script>
</body>
</html>
