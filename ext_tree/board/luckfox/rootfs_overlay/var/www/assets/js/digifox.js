// DigiFox web UI — the same screen as the Fox Remote app.
// Uses the same PHP endpoints as the app:
//   status_fast.php, usb_to_i2s.php, handle_service.php, rate.php, track.php,
//   amp.php (amplifier volume / mute / power through pfctl -> UART -> STM32),
//   run_update.php, reboot.php
(function () {
    'use strict';

    var RU = (navigator.language || 'ru').toLowerCase().indexOf('ru') === 0 ||
             (navigator.language || '').toLowerCase().indexOf('uk') === 0;

    var T = RU ? {
        source: 'ИСТОЧНИК', amp: 'УСИЛИТЕЛЬ DIGID D1', volume: 'ГРОМКОСТЬ УСИЛИТЕЛЯ', mode: 'РЕЖИМ',
        players: 'ПЛЕЕРЫ', net: 'СЕТЬ', power: 'POWER', cancel: 'ОТМЕНА', close: 'Закрыть',
        m_i2s: 'Настройки I2S', m_update: 'Обновление прошивки', m_reboot: 'Перезагрузить Фокс',
        m_classic: 'Классический интерфейс PureFox',
        noPlayer: 'Нет плеера', on: 'Включён', standby: 'Дежурный режим',
        noLink: 'Нет связи с Фоксом', ampNone: 'Усилитель не на связи (UART) — громкость регулируется только им',
        ampStandby: 'Усилитель в дежурном режиме',
        ampOn: 'Фокс на 100% (bit-perfect), громкость регулирует AX5689',
        stopped: 'нет сигнала', starting: 'Запуск: ', usbOn: 'Включение USB → I2S…', usbOff: 'Возврат в сеть…',
        powerOn: 'Включение усилителя…', powerOff: 'Выключение усилителя…',
        askUpdate: 'Проверить и установить обновление DigiFox с GitHub? Во время обновления звук прервётся, затем Фокс перезагрузится.',
        askReboot: 'Перезагрузить Фокс?', rebooting: 'Перезагрузка Фокса…', back: 'Фокс снова на связи',
        updStart: 'Запуск обновления…\n', updLost: '\n[связь прервалась — если Фокс перезагружается, страница обновится сама]\n',
        players_: {
            qobuz: 'Qobuz Connect', naa: 'HQPlayer NAA', raat: 'Roon Ready', shairport: 'AirPlay',
            spotify: 'Spotify Connect', lms: 'Squeezelite', aprenderer: 'UPnP Renderer', mpd: 'MPD',
            aplayer: 'Веб-радио', apscream: 'APScream'
        }
    } : {
        source: 'SOURCE', amp: 'DIGID D1 AMPLIFIER', volume: 'AMPLIFIER VOLUME', mode: 'MODE',
        players: 'PLAYERS', net: 'NETWORK', power: 'POWER', cancel: 'CANCEL', close: 'Close',
        m_i2s: 'I2S settings', m_update: 'Firmware update', m_reboot: 'Reboot the Fox',
        m_classic: 'Classic PureFox interface',
        noPlayer: 'No player', on: 'On', standby: 'Standby',
        noLink: 'No connection to the Fox', ampNone: 'Amplifier not connected (UART) — volume is set only by the amplifier',
        ampStandby: 'Amplifier is in standby',
        ampOn: 'Fox at 100% (bit-perfect), volume is set by the AX5689',
        stopped: 'no signal', starting: 'Starting: ', usbOn: 'Switching to USB → I2S…', usbOff: 'Back to network…',
        powerOn: 'Switching the amplifier on…', powerOff: 'Switching the amplifier to standby…',
        askUpdate: 'Check and install a DigiFox update from GitHub? Playback stops during the update, then the Fox reboots.',
        askReboot: 'Reboot the Fox?', rebooting: 'Rebooting the Fox…', back: 'The Fox is back',
        updStart: 'Starting update…\n', updLost: '\n[connection lost — if the Fox is rebooting, the page reloads by itself]\n',
        players_: {
            qobuz: 'Qobuz Connect', naa: 'HQPlayer NAA', raat: 'Roon Ready', shairport: 'AirPlay',
            spotify: 'Spotify Connect', lms: 'Squeezelite', aprenderer: 'UPnP Renderer', mpd: 'MPD',
            aplayer: 'Web radio', apscream: 'APScream'
        }
    };

    var PLAYERS = ['qobuz', 'naa', 'raat', 'shairport', 'spotify', 'lms', 'aprenderer', 'mpd', 'aplayer', 'apscream'];

    function $(id) { return document.getElementById(id); }

    // ---------------- state ----------------
    var S = {
        connected: false,
        service: '',
        usb: null,
        rate: null,
        amp: null,          // {present,pos,max,mute,db,power}
        track: null,        // {artist,title,album}
        busy: null,
        error: null
    };
    var localVol = null, lastUserAt = 0, pending = null, inFlight = false, sendTimer = null;
    var pollTimer = null, refreshing = false;

    // ---------------- requests ----------------
    function get(url) {
        return fetch(url, { cache: 'no-store' }).then(function (r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.text();
        });
    }
    function post(url, data, timeoutMs) {
        var body = Object.keys(data).map(function (k) {
            return encodeURIComponent(k) + '=' + encodeURIComponent(data[k]);
        }).join('&');
        var ctl = window.AbortController ? new AbortController() : null;
        var t = ctl ? setTimeout(function () { ctl.abort(); }, timeoutMs || 15000) : null;
        return fetch(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body,
            signal: ctl ? ctl.signal : undefined
        }).then(function (r) {
            clearTimeout(t);
            return r.text().then(function (txt) {
                if (!r.ok) throw new Error('HTTP ' + r.status + ': ' + txt.slice(0, 120));
                return txt;
            });
        }, function (e) { clearTimeout(t); throw e; });
    }
    function json(txt) { try { return JSON.parse(txt); } catch (e) { return null; } }

    function formatRate(raw) {
        if (!raw) return null;
        raw = raw.trim();
        if (raw === 'STOP') return T.stopped;
        if (raw.indexOf('DSD') === 0) return raw;
        var p = raw.split(' '), hz = parseInt(p[0], 10);
        if (!hz) return raw;
        var khz = hz % 1000 === 0 ? String(hz / 1000) : (hz / 1000).toFixed(1);
        var m = /[SU](\d+)/.exec(p[1] || '');
        return m ? khz + ' kHz · ' + m[1] + ' bit' : khz + ' kHz';
    }

    // ---------------- polling ----------------
    function refresh() {
        if (refreshing) return Promise.resolve();
        refreshing = true;
        return Promise.all([
            get('status_fast.php').then(json),
            post('usb_to_i2s.php', { action: 'status' }).then(json),
            get('amp.php').then(json).catch(function () { return null; }),
            get('rate.php').catch(function () { return null; }),
            get('track.php').then(json).catch(function () { return null; })
        ]).then(function (r) {
            var st = r[0] || {};
            S.connected = true;
            S.error = null;
            S.service = st.active_service || '';
            S.usb = r[1] ? !!r[1].enabled : null;
            S.amp = r[2] && r[2].present ? r[2] : null;
            S.rate = formatRate(r[3]);
            var t = r[4];
            S.track = t && (t.title || t.artist) && !S.usb && S.rate !== T.stopped ? t : null;
        }).catch(function () {
            S.connected = false;
            S.error = T.noLink;
        }).then(function () {
            refreshing = false;
            render();
        });
    }

    function schedule(ms) {
        clearTimeout(pollTimer);
        pollTimer = setTimeout(function loop() {
            if (document.visibilityState !== 'hidden' && !S.busy) refresh();
            pollTimer = setTimeout(loop, 2000);
        }, ms);
    }

    // ---------------- render ----------------
    function setText(id, txt) { var e = $(id); if (e.textContent !== txt) e.textContent = txt; }

    function render() {
        var c = S.connected, amp = S.amp, idle = c && !S.busy;

        setText('host', location.hostname);
        $('dot').className = 'dot' + (c ? ' on' : '');

        var src = !c ? '—' : S.usb ? 'USB → I2S' : !S.service ? T.noPlayer : (T.players_[S.service] || S.service);
        setText('source', src);
        $('source').className = 'source' + (c ? '' : ' off');

        $('track').hidden = !S.track;
        if (S.track) {
            setText('t-title', S.track.title || '');
            setText('t-artist', [S.track.artist, S.track.album].filter(Boolean).join('  ·  '));
        }
        setText('rate', S.rate || ' ');
        setText('mode-cap', S.usb === true ? 'USB' : S.usb === false ? T.net : '');

        // status line
        var st = $('status');
        if (S.busy) {
            st.className = 'status busy';
            st.innerHTML = '<span class="spin"></span><span></span>';
            st.lastChild.textContent = S.busy;
        } else {
            st.className = 'status';
            st.textContent = S.error || '';
        }

        // amplifier power
        $('power-row').hidden = !amp;
        if (amp) {
            setText('power-state', amp.power ? T.on : T.standby);
            $('power-state').className = 'power-state' + (amp.power ? '' : ' off');
            $('power-btn').className = 'hifi' + (amp.power ? ' on' : '');
            $('power-btn').disabled = !idle;
        }

        // volume
        var vOn = !!(c && amp && amp.power);
        var slider = $('vol');
        slider.disabled = !vOn;
        $('mute-btn').disabled = !vOn;
        $('mute-btn').className = 'hifi mute' + (amp && amp.mute ? ' on' : '');
        if (amp) {
            slider.max = String(amp.max);
            var holding = pending !== null || inFlight || Date.now() - lastUserAt < 1500;
            var v = holding && localVol !== null ? localVol : amp.pos;
            if (!holding && slider.value !== String(v)) slider.value = String(v);
            setText('vol-num', amp.db ? String(v - amp.max) : String(v));
            setText('vol-unit', amp.db ? 'dB' : '');
            paintSlider(slider);
        } else {
            setText('vol-num', '--');
            setText('vol-unit', '');
        }
        $('vol-num').parentNode.className = 'vol-num' + (vOn && !(amp && amp.mute) ? '' : ' off');
        setText('vol-note', !c ? '' : !amp ? T.ampNone : !amp.power ? T.ampStandby : T.ampOn);

        // mode
        $('mode-net').className = 'hifi' + (S.usb === false ? ' on' : '');
        $('mode-usb').className = 'hifi' + (S.usb === true ? ' on' : '');
        $('mode-net').disabled = $('mode-usb').disabled = !idle;

        // players
        var btns = $('players').children;
        for (var i = 0; i < btns.length; i++) {
            var id = btns[i].getAttribute('data-id');
            btns[i].className = 'hifi' + (S.usb === false && S.service === id ? ' on' : '');
            btns[i].disabled = !idle;
        }
    }

    // filled part of the slider track (WebKit has no ::range-progress)
    function paintSlider(s) {
        var max = Number(s.max) || 1, pct = Math.max(0, Math.min(100, Number(s.value) / max * 100));
        s.style.setProperty('--pct', pct + '%');
        s.style.setProperty('--fill', s.disabled ? '#444444' : '#ededed');
    }

    // ---------------- actions ----------------
    function action(message, fn) {
        if (S.busy || !S.connected) return;
        S.busy = message; S.error = null; render();
        Promise.resolve().then(fn).catch(function (e) {
            S.error = (e && e.message) || 'Ошибка';
        }).then(function () {
            S.busy = null;
            return refresh();
        });
    }

    function selectPlayer(id) {
        if (S.usb === false && S.service === id) return;
        action(T.starting + (T.players_[id] || id) + '…', function () {
            var p = S.usb ? post('usb_to_i2s.php', { action: 'disable' }, 60000) : Promise.resolve();
            return p.then(function () {
                return post('handle_service.php', { service: id }, 60000);
            }).then(function (txt) {
                var j = json(txt);
                if (!j || j.status !== 'success') throw new Error((j && j.message) || txt.slice(0, 120));
            });
        });
    }

    function setUsb(on) {
        if (S.usb === on) return;
        action(on ? T.usbOn : T.usbOff, function () {
            return post('usb_to_i2s.php', { action: on ? 'enable' : 'disable' }, 60000).then(function (txt) {
                if (txt.indexOf('successfully') < 0) throw new Error(txt.slice(0, 120));
            });
        });
    }

    function sendVol() {
        sendTimer = null;
        if (inFlight || pending === null) return;
        var pos = pending; pending = null; inFlight = true;
        post('amp.php', { action: 'vol', pos: pos }).then(function () {
            if (S.amp) { S.amp.pos = pos; S.amp.mute = false; }
        }).catch(function (e) { S.error = e.message; }).then(function () {
            inFlight = false;
            if (pending !== null) sendTimer = setTimeout(sendVol, 120);
            else render();
        });
    }

    function setVol(v) {
        var amp = S.amp;
        if (!S.connected || !amp || !amp.power) return;
        v = Math.max(0, Math.min(amp.max, Math.round(v)));
        localVol = v; lastUserAt = Date.now(); pending = v;
        var s = $('vol');
        s.value = String(v);
        setText('vol-num', amp.db ? String(v - amp.max) : String(v));
        paintSlider(s);
        if (!inFlight && !sendTimer) sendTimer = setTimeout(sendVol, 0);
    }

    function ampPost(what, msg) {
        if (!S.amp) return;
        if (msg) {
            action(msg, function () {
                return post('amp.php', { action: what }).then(function () {
                    return new Promise(function (ok) { setTimeout(ok, 1200); });   // the amp reports back
                });
            });
        } else {
            post('amp.php', { action: what }).then(function () {
                if (what === 'mute' && S.amp) S.amp.mute = !S.amp.mute;
                render();
                setTimeout(refresh, 300);
            }).catch(function (e) { S.error = e.message; render(); });
        }
    }

    // ---------------- sheets ----------------
    function show(id, on) { $(id).hidden = !on; }
    var confirmCb = null;
    function confirmBox(text, cb) {
        $('confirm-text').textContent = text;
        confirmCb = cb;
        show('confirm', true);
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

    function runUpdate() {
        var log = $('update-log');
        log.textContent = T.updStart;
        show('update', true);
        var reboot = false;
        fetch('run_update.php', { cache: 'no-store' }).then(function (r) {
            if (!r.body || !r.body.getReader) return r.text().then(function (t) { log.textContent += t; });
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
            log.textContent += T.updLost;
        }).then(function () {
            reboot = /Перезагрузка через|reboot/i.test(log.textContent);
            log.scrollTop = log.scrollHeight;
            if (reboot) {
                log.textContent += '\n' + T.rebooting + '\n';
                setTimeout(function () { waitForFox(function () { location.reload(); }); }, 8000);
            }
        });
    }

    function reboot() {
        S.busy = T.rebooting; render();
        post('reboot.php', {}).catch(function () {}).then(function () {
            setTimeout(function () { waitForFox(function () { location.reload(); }); }, 8000);
        });
    }

    // ---------------- setup ----------------
    function init() {
        if (!RU) {
            var ts = document.querySelectorAll('[data-t]');
            for (var i = 0; i < ts.length; i++) {
                var k = ts[i].getAttribute('data-t');
                if (T[k]) ts[i].textContent = T[k];
            }
            document.documentElement.lang = 'en';
        }

        var grid = $('players');
        PLAYERS.forEach(function (id) {
            var b = document.createElement('button');
            b.className = 'hifi';
            b.setAttribute('data-id', id);
            b.textContent = T.players_[id];
            b.title = T.players_[id];
            b.addEventListener('click', function () { selectPlayer(id); });
            grid.appendChild(b);
        });

        $('mode-net').addEventListener('click', function () { setUsb(false); });
        $('mode-usb').addEventListener('click', function () { setUsb(true); });
        $('power-btn').addEventListener('click', function () {
            if (S.amp) ampPost('power', S.amp.power ? T.powerOff : T.powerOn);
        });
        $('mute-btn').addEventListener('click', function () { ampPost('mute'); });
        $('vol').addEventListener('input', function () { setVol(Number(this.value)); });

        // keys and wheel anywhere on the page change the amplifier volume by 1 step
        document.addEventListener('keydown', function (e) {
            if (e.ctrlKey || e.altKey || e.metaKey || !$('menu').hidden || !$('confirm').hidden || !$('update').hidden) return;
            if (e.target === $('vol')) return;     // the slider handles its own arrows
            var k = e.key, d = 0;
            if (k === 'ArrowUp' || k === 'ArrowRight' || k === 'AudioVolumeUp') d = 1;
            else if (k === 'ArrowDown' || k === 'ArrowLeft' || k === 'AudioVolumeDown') d = -1;
            else if (k === 'm' || k === 'M' || k === 'ь' || k === 'Ь') { ampPost('mute'); return; }
            if (!d || !S.amp || !S.amp.power) return;
            e.preventDefault();
            setVol((localVol !== null && Date.now() - lastUserAt < 1500 ? localVol : S.amp.pos) + d);
        });
        document.addEventListener('wheel', function (e) {
            if (!S.amp || !S.amp.power || e.deltaY === 0 || !$('menu').hidden) return;
            if (!(e.target === $('vol') || e.target.closest('.volume'))) return;   // only over the volume block
            e.preventDefault();
            setVol((localVol !== null && Date.now() - lastUserAt < 1500 ? localVol : S.amp.pos) + (e.deltaY < 0 ? 1 : -1));
        }, { passive: false });

        $('menu-btn').addEventListener('click', function () { show('menu', true); });
        $('m-close').addEventListener('click', function () { show('menu', false); });
        $('menu').addEventListener('click', function (e) { if (e.target === this) show('menu', false); });
        $('m-update').addEventListener('click', function () {
            show('menu', false);
            confirmBox(T.askUpdate, runUpdate);
        });
        $('m-reboot').addEventListener('click', function () {
            show('menu', false);
            confirmBox(T.askReboot, reboot);
        });
        $('confirm-no').addEventListener('click', function () { show('confirm', false); confirmCb = null; });
        $('confirm-yes').addEventListener('click', function () {
            show('confirm', false);
            var cb = confirmCb; confirmCb = null;
            if (cb) cb();
        });
        $('update-close').addEventListener('click', function () { show('update', false); });
        document.addEventListener('visibilitychange', function () {
            if (document.visibilityState === 'visible') schedule(0);
        });

        render();
        refresh();
        schedule(2000);
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();
