// DigiFox: amplifier page — DigiD D1 settings, sleep timer, alarm, amp firmware.
// API: amp.php?full=1 (GET), amp.php POST action=set|sleep|alarm|tz, amp_flash.php (upload).
(function () {
    'use strict';

    function $(id) { return document.getElementById(id); }
    function post(data) {
        var body = Object.keys(data).map(function (k) {
            return encodeURIComponent(k) + '=' + encodeURIComponent(data[k]);
        }).join('&');
        return fetch('amp.php', {
            method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: body
        }).then(function (r) {
            return r.json().catch(function () { return {}; }).then(function (j) {
                if (!r.ok) throw new Error(j.error === 'amplifier not connected' ? 'Усилитель не на связи' : (j.error || 'HTTP ' + r.status));
                return j;
            });
        });
    }

    var S = null, pending = {}, alarmLoaded = false, alarmDirty = false, flashing = false, msgTimer = null;

    function status(text, spin) {
        clearTimeout(msgTimer);
        var st = $('status');
        st.className = 'status' + (spin ? ' busy' : '');
        st.innerHTML = '';
        if (!text) return;
        if (spin) { var s = document.createElement('span'); s.className = 'spin'; st.appendChild(s); }
        var t = document.createElement('span'); t.textContent = text; st.appendChild(t);
        if (!spin) msgTimer = setTimeout(function () { st.innerHTML = ''; }, 5000);
    }

    // ---------------- time zones ----------------
    (function fillTz() {
        var sel = $('tz');
        for (var h = -12; h <= 14; h++) {
            var o = document.createElement('option');
            // POSIX: sign is inverted (UTC+3 -> XXX-3)
            o.value = h === 3 ? 'MSK-3' : 'UTC' + (h > 0 ? '-' : h < 0 ? '+' : '') + Math.abs(h);
            o.textContent = 'UTC' + (h > 0 ? '+' : h < 0 ? '−' : '±') + Math.abs(h) + (h === 3 ? ' (Москва)' : '');
            sel.appendChild(o);
        }
    })();

    // ---------------- render ----------------
    function render() {
        var amp = S && S.present ? S : null;
        $('dot').className = 'dot' + (amp ? ' on' : '');

        var btns = document.querySelectorAll('[data-k]');
        for (var i = 0; i < btns.length; i++) {
            var b = btns[i], k = b.getAttribute('data-k'), v = b.getAttribute('data-v');
            var cur = amp && amp.cfg ? amp.cfg[k] : null;
            if (pending[k] !== undefined) cur = pending[k];
            b.className = 'hifi' + (cur !== null && String(cur) === v ? ' on' : '');
            b.disabled = !amp || !amp.cfg || flashing;
        }

        // sleep timer
        var left = S ? S.sleep_left : 0;
        $('sleep-val').textContent = left > 0 ? 'через ' + Math.ceil(left / 60) + ' мин' : '';
        $('sleep-off').hidden = !(left > 0);
        var sb = document.querySelectorAll('[data-sleep]');
        for (var j = 0; j < sb.length; j++) sb[j].disabled = flashing || !S;

        // clock
        if (S) $('clock-val').textContent = S.time_ok ? 'сейчас ' + S.now : 'время не синхронизировано';

        // alarm form — loaded once, then left to the user
        if (S && !alarmLoaded && S.alarm) {
            alarmLoaded = true;
            var a = S.alarm;
            $('a-time').value = a.time;
            setOn(a.on);
            setDays(a.days);
            $('a-src').value = a.src;
            if ($('a-src').value !== a.src) $('a-src').value = 'keep';
            $('a-vol').value = a.vol;
            $('tz').value = S.tz;
            showVol();
        }

        // firmware
        $('fw-ver').textContent = S && S.ver ? 'версия ' + S.ver : (amp ? 'версия до 1.1' : '');
        $('fw-go').disabled = flashing || !$('fw-file').files.length;
    }

    function refresh() {
        return fetch('amp.php?full=1', { cache: 'no-store' }).then(function (r) { return r.json(); })
            .then(function (j) { S = j; }).catch(function () { S = null; })
            .then(render);
    }

    // ---------------- settings ----------------
    function setCfg(k, v) {
        if (!S || !S.present) return;
        pending[k] = v; render();
        post({ action: 'set', key: k, val: v }).then(function () {
            // the amplifier confirms with its next report (acfg)
            setTimeout(function () { delete pending[k]; refresh(); }, 1200);
        }).catch(function (e) {
            delete pending[k]; status(e.message); render();
        });
    }

    // ---------------- alarm ----------------
    var alarmOn = false;
    function setOn(on) {
        alarmOn = !!on;
        $('a-on').textContent = alarmOn ? 'ВКЛ' : 'ВЫКЛ';
        $('a-on').className = 'hifi' + (alarmOn ? ' on' : '');
    }
    function setDays(days) {
        var b = $('a-days').children;
        for (var i = 0; i < b.length; i++) {
            b[i].className = 'hifi' + (days.indexOf(b[i].getAttribute('data-d')) >= 0 ? ' on' : '');
        }
    }
    function getDays() {
        var b = $('a-days').children, d = '';
        for (var i = 0; i < b.length; i++) if (b[i].className.indexOf(' on') >= 0) d += b[i].getAttribute('data-d');
        return d;
    }
    function showVol() { $('a-vol-val').textContent = $('a-vol').value + ' dB'; }
    function dirty() { alarmDirty = true; $('a-save').disabled = false; }

    function saveAlarm() {
        var days = getDays();
        if (alarmOn && !days) { status('Выберите хотя бы один день'); return; }
        status('Сохраняю…', true);
        post({ action: 'tz', tz: $('tz').value }).then(function () {
            return post({
                action: 'alarm', on: alarmOn ? 1 : 0, time: $('a-time').value || '07:00',
                days: days, src: $('a-src').value, vol: $('a-vol').value
            });
        }).then(function () {
            alarmDirty = false;
            status(alarmOn ? 'Будильник на ' + $('a-time').value + ' сохранён' : 'Будильник выключен');
            refresh();
        }).catch(function (e) { status(e.message); });
    }

    // ---------------- firmware ----------------
    function flash() {
        var f = $('fw-file').files[0];
        if (!f || flashing) return;
        if (!/\.bin$/i.test(f.name)) { status('Нужен файл .bin'); return; }
        flashing = true; render();
        var log = $('fw-log');
        log.hidden = false; log.textContent = '';
        status('Прошивка усилителя… не выключайте питание', true);
        var fd = new FormData();
        fd.append('fw', f);
        fetch('amp_flash.php', { method: 'POST', body: fd }).then(function (r) {
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
        }).catch(function () {
            log.textContent += '\n[связь прервалась]\n';
        }).then(function () {
            flashing = false;
            var ok = /\[OK\]\s*$/.test(log.textContent);
            status(ok ? 'Прошивка усилителя обновлена' : 'Прошивка не удалась — см. журнал');
            if (ok) { $('fw-file').value = ''; $('fw-name').textContent = 'Выбрать файл .bin…'; }
            setTimeout(refresh, 6000);
            render();
        });
    }

    // ---------------- setup ----------------
    function init() {
        var btns = document.querySelectorAll('[data-k]');
        for (var i = 0; i < btns.length; i++) {
            btns[i].addEventListener('click', function () {
                setCfg(this.getAttribute('data-k'), this.getAttribute('data-v'));
            });
        }
        var sb = document.querySelectorAll('[data-sleep]');
        for (var j = 0; j < sb.length; j++) {
            sb[j].addEventListener('click', function () {
                var m = this.getAttribute('data-sleep');
                post({ action: 'sleep', min: m }).then(function () {
                    status('Усилитель выключится через ' + m + ' мин'); refresh();
                }).catch(function (e) { status(e.message); });
            });
        }
        $('sleep-off').addEventListener('click', function () {
            post({ action: 'sleep', min: 0 }).then(function () { status('Таймер сна отменён'); refresh(); });
        });

        $('a-on').addEventListener('click', function () { setOn(!alarmOn); dirty(); });
        var db = $('a-days').children;
        for (var d = 0; d < db.length; d++) {
            db[d].addEventListener('click', function () {
                this.className = this.className.indexOf(' on') >= 0 ? 'hifi' : 'hifi on'; dirty();
            });
        }
        $('a-time').addEventListener('change', dirty);
        $('a-src').addEventListener('change', dirty);
        $('tz').addEventListener('change', dirty);
        $('a-vol').addEventListener('input', function () { showVol(); dirty(); });
        $('a-save').addEventListener('click', saveAlarm);

        $('fw-file').addEventListener('change', function () {
            var f = this.files[0];
            $('fw-name').textContent = f ? f.name + ' · ' + Math.round(f.size / 1024) + ' КБ' : 'Выбрать файл .bin…';
            render();
        });
        $('fw-go').addEventListener('click', flash);

        showVol();
        render();
        refresh();
        setInterval(function () { if (!flashing && document.visibilityState !== 'hidden') refresh(); }, 2000);
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();
