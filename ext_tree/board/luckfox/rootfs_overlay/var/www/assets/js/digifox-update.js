// DigiFox — страница «Обновление»: DigiFox и прошивка усилителя в одном месте.
//   update_check.php    — есть ли новая сборка и что в ней нового (с GitHub)
//   run_update.php      — установка DigiFox (журнал по мере работы, потом перезагрузка)
//   amp.php?full=1      — версия усилителя и встроенная в Фокс прошивка (fw_builtin, fw_update)
//   amp_flash.php       — прошивка усилителя: builtin=1 (встроенная) или файл .bin
(function () {
    'use strict';

    function $(id) { return document.getElementById(id); }
    var msgTimer = null, busy = false, amp = null, check = null;

    function status(text, spin) {
        clearTimeout(msgTimer);
        var st = $('status');
        st.className = 'status' + (spin ? ' busy' : '');
        st.innerHTML = '';
        if (!text) return;
        if (spin) { var s = document.createElement('span'); s.className = 'spin'; st.appendChild(s); }
        var t = document.createElement('span'); t.textContent = text; st.appendChild(t);
        if (!spin) msgTimer = setTimeout(function () { st.innerHTML = ''; }, 6000);
    }

    function fillChanges(ul, list) {
        ul.innerHTML = '';
        (list || []).forEach(function (c) {
            var h = document.createElement('div');
            h.className = 'ver'; h.textContent = c.ver;
            ul.appendChild(h);
            c.items.forEach(function (it) {
                var li = document.createElement('li');
                li.textContent = it;
                ul.appendChild(li);
            });
        });
        return (list || []).length > 0;
    }

    function toggle(btn, ul) {
        ul.hidden = !ul.hidden;
        btn.textContent = ul.hidden ? 'ЧТО НОВОГО' : 'СКРЫТЬ';
    }

    // журнал запроса, который отдаёт текст по мере работы (обновление, прошивка)
    function stream(url, opts, log) {
        log.hidden = false; log.textContent = '';
        return fetch(url, opts).then(function (r) {
            if (!r.body || !r.body.getReader) return r.text().then(function (t) { log.textContent = t; });
            var rd = r.body.getReader(), dec = new TextDecoder();
            return (function pump() {
                return rd.read().then(function (x) {
                    if (x.done) return;
                    log.textContent += dec.decode(x.value, { stream: true });
                    log.scrollTop = log.scrollHeight;
                    return pump();
                });
            })();
        });
    }

    function waitForFox(then) {
        var tries = 0;
        (function again() {
            setTimeout(function () {
                fetch('status_fast.php', { cache: 'no-store' }).then(function (r) {
                    if (r.ok) then(); else again();
                }).catch(function () { if (++tries < 120) again(); });
            }, 3000);
        })();
    }

    // ---------------- DigiFox ----------------
    function renderFox() {
        var st = $('fox-state'), ins = $('fox-install'), nb = $('fox-notes');
        if (!check) { st.textContent = 'Проверяю…'; st.className = 'upd-state dim'; ins.hidden = nb.hidden = true; return; }
        if (!check.online) {
            st.textContent = 'Нет связи с GitHub — проверьте интернет на Фоксе';
            st.className = 'upd-state dim'; ins.hidden = nb.hidden = true; return;
        }
        if (check.newer) {
            st.textContent = 'Доступна версия ' + check.latest;
            st.className = 'upd-state';
            ins.hidden = false; ins.textContent = 'УСТАНОВИТЬ ' + check.latest;
            ins.disabled = busy;
            nb.hidden = !fillChanges($('fox-changes'), check.changes);
        } else {
            st.textContent = 'Установлена последняя версия';
            st.className = 'upd-state dim';
            ins.hidden = nb.hidden = true; $('fox-changes').hidden = true;
        }
        $('fox-check').disabled = busy;
    }

    function checkFox() {
        check = null; renderFox();
        return fetch('update_check.php', { cache: 'no-store' }).then(function (r) { return r.json(); })
            .then(function (j) { check = j; $('dot').className = 'dot on'; })
            .catch(function () { check = { online: false }; })
            .then(function () { renderFox(); renderAmp(); });
    }

    function installFox() {
        if (busy || !check || !check.newer) return;
        if (!confirm('Установить DigiFox ' + check.latest + '? Звук прервётся, потом Фокс перезагрузится.')) return;
        busy = true; renderFox(); renderAmp();
        status('Обновление DigiFox… не выключайте питание', true);
        var log = $('fox-log');
        stream('run_update.php', { cache: 'no-store' }, log).catch(function () {
            log.textContent += '\n[связь прервалась — если Фокс перезагружается, страница обновится сама]\n';
        }).then(function () {
            log.scrollTop = log.scrollHeight;
            if (/Перезагрузка через|reboot/i.test(log.textContent)) {
                status('Фокс перезагружается…', true);
                setTimeout(function () { waitForFox(function () { location.reload(); }); }, 8000);
            } else {
                busy = false; status('Обновление не прошло — см. журнал'); renderFox(); renderAmp();
            }
        });
    }

    // ---------------- Усилитель ----------------
    function renderAmp() {
        var st = $('amp-state'), ins = $('amp-install'), nb = $('amp-notes');
        if (!amp || !amp.present) {
            $('amp-ver').textContent = '';
            st.textContent = amp ? 'Усилитель не на связи' : '—';
            st.className = 'upd-state dim'; ins.hidden = nb.hidden = true;
            $('fw-go').disabled = true;
            return;
        }
        $('amp-ver').textContent = amp.ver ? 'установлена ' + amp.ver : '';
        if (amp.fw_update) {
            st.textContent = 'Доступна версия ' + amp.fw_builtin;
            st.className = 'upd-state';
            ins.hidden = false; ins.textContent = 'ОБНОВИТЬ УСИЛИТЕЛЬ ДО ' + amp.fw_builtin;
            nb.hidden = !fillChanges($('amp-changes'), check && check.amp_changes);
        } else {
            st.textContent = amp.fw_builtin ? 'Установлена последняя версия' : 'Для этого экрана обновление — только файлом';
            st.className = 'upd-state dim';
            ins.hidden = nb.hidden = true; $('amp-changes').hidden = true;
        }
        ins.disabled = busy;
        $('fw-go').disabled = busy || !$('fw-file').files.length;
    }

    function loadAmp() {
        return fetch('amp.php?full=1', { cache: 'no-store' }).then(function (r) { return r.json(); })
            .then(function (j) { amp = j; }).catch(function () { amp = null; })
            .then(renderAmp);
    }

    function flashAmp(builtin) {
        if (busy) return;
        var f = $('fw-file').files[0], fd = new FormData();
        if (builtin) {
            if (!confirm('Обновить прошивку усилителя до ' + amp.fw_builtin + '? Около минуты усилитель будет выключен.')) return;
            fd.append('builtin', '1');
        } else {
            if (!f) return;
            if (!/\.bin$/i.test(f.name)) { status('Нужен файл .bin'); return; }
            fd.append('fw', f);
        }
        busy = true; renderAmp(); renderFox();
        status('Прошивка усилителя… не выключайте питание', true);
        var log = $('amp-log');
        stream('amp_flash.php', { method: 'POST', body: fd }, log).catch(function () {
            log.textContent += '\n[связь прервалась]\n';
        }).then(function () {
            busy = false;
            var ok = /\[OK\]\s*$/.test(log.textContent);
            status(ok ? 'Прошивка усилителя обновлена' : 'Прошивка не удалась — см. журнал');
            if (ok) { $('fw-file').value = ''; $('fw-name').textContent = 'Выбрать файл .bin…'; }
            setTimeout(function () { loadAmp(); checkFox(); }, 6000);
            renderAmp(); renderFox();
        });
    }

    function init() {
        $('fox-check').addEventListener('click', checkFox);
        $('fox-install').addEventListener('click', installFox);
        $('fox-notes').addEventListener('click', function () { toggle(this, $('fox-changes')); });
        $('amp-notes').addEventListener('click', function () { toggle(this, $('amp-changes')); });
        $('amp-install').addEventListener('click', function () { flashAmp(true); });
        $('fw-file').addEventListener('change', function () {
            var f = this.files[0];
            $('fw-name').textContent = f ? f.name + ' · ' + Math.round(f.size / 1024) + ' КБ' : 'Выбрать файл .bin…';
            renderAmp();
        });
        $('fw-go').addEventListener('click', function () { flashAmp(false); });
        renderFox(); renderAmp();
        loadAmp();
        checkFox();
        setInterval(function () { if (!busy && document.visibilityState !== 'hidden') loadAmp(); }, 5000);
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();
