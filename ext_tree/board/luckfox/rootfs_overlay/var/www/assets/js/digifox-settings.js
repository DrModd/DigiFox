// DigiFox I2S settings page. Same requests as the classic PureFox I2S dialog:
//   GET  handle_i2s.php?action=getStatus
//   POST handle_i2s.php  mode=pll|ext | mclk=512|1024 | submode=std|8ch|lr|plr
//                        pcm_swap=0|1 | dsd_swap=0|1 | freq_swap=0|1
// Every setting is applied at once; the clock source (and MCLK in EXT mode)
// rewrites the DTB in flash and takes effect after a reboot.
(function () {
    'use strict';

    var RU = (navigator.language || 'ru').toLowerCase().indexOf('ru') === 0;
    var T = RU ? {
        on: 'ВКЛ', off: 'ВЫКЛ', applying: 'Применяю…', saved: 'Сохранено', rebooting: 'Перезагрузка Фокса…',
        noLink: 'Нет связи с Фоксом', busy: 'Фокс занят переключением звука, повторите через пару секунд',
        usbStd: 'В режиме USB → I2S доступен только STD', needI2s: 'Подрежимы работают только при выходе I2S',
        pll: 'PLL — встроенный', ext: 'EXT — внешний генератор',
        sub: { std: 'стерео', '8ch': '8 каналов', lr: 'дуал-моно', plr: 'дуал-моно, баланс' }
    } : {
        on: 'ON', off: 'OFF', applying: 'Applying…', saved: 'Saved', rebooting: 'Rebooting the Fox…',
        noLink: 'No connection to the Fox', busy: 'The Fox is switching audio, try again in a few seconds',
        usbStd: 'Only STD is available in USB → I2S mode', needI2s: 'Sub-modes need the I2S output',
        pll: 'PLL — internal', ext: 'EXT — external clock',
        sub: { std: 'stereo', '8ch': '8 channels', lr: 'dual mono', plr: 'dual mono, balanced' }
    };
    var EN_TEXT = {
        needReboot: 'The clock change takes effect after the Fox reboots', reboot: 'REBOOT',
        clock: 'CLOCK SOURCE', output: 'OUTPUT', swaps: 'SWAPS',
        clockHint: 'PLL — the RV1106 internal synthesizer, MCLK pin is an output. EXT — external clock, MCLK is an input. Takes effect after a reboot.',
        mclkHint: 'In PLL mode it changes at once, in EXT mode after a reboot.',
        outputHint: 'STD — stereo; 8CH — 8 channels up to 192 kHz; L/R — dual mono; ±L/±R — dual mono, balanced. Applied at once.',
        pcmSwap: 'PCM channels', pcmSwapD: 'Swap left and right',
        dsdSwap: 'DSD channels', dsdSwapD: 'Swap the physical DSD lines',
        freqSwap: '44.1 / 48 families', freqSwapD: 'Swap the 44.1 and 48 kHz frequency domains'
    };

    function $(id) { return document.getElementById(id); }
    var cfg = null, usb = false, busy = false, msgTimer = null;
    // the reboot banner survives page reloads until the Fox actually reboots
    var pendingReboot = false;
    try { pendingReboot = sessionStorage.getItem('df_i2s_reboot') === '1'; } catch (e) {}

    function setPending(on) {
        pendingReboot = on;
        try { on ? sessionStorage.setItem('df_i2s_reboot', '1') : sessionStorage.removeItem('df_i2s_reboot'); } catch (e) {}
        $('reboot-banner').hidden = !on;
    }

    function status(text, spin) {
        clearTimeout(msgTimer);
        var st = $('status');
        st.className = 'status' + (spin ? ' busy' : '');
        st.innerHTML = '';
        if (!text) return;
        if (spin) { var s = document.createElement('span'); s.className = 'spin'; st.appendChild(s); }
        var t = document.createElement('span'); t.textContent = text; st.appendChild(t);
        if (!spin) msgTimer = setTimeout(function () { st.innerHTML = ''; }, 4000);
    }

    function load() {
        return Promise.all([
            fetch('handle_i2s.php?action=getStatus', { cache: 'no-store' }).then(function (r) {
                if (!r.ok) throw new Error('HTTP ' + r.status); return r.json();
            }),
            fetch('usb_to_i2s.php', {
                method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'action=status'
            }).then(function (r) { return r.json(); }).catch(function () { return { enabled: false }; })
        ]).then(function (r) {
            cfg = r[0]; usb = !!(r[1] && r[1].enabled);
            $('dot').className = 'dot on';
            render();
        }).catch(function () {
            $('dot').className = 'dot';
            status(T.noLink);
        });
    }

    function render() {
        var btns = document.querySelectorAll('[data-k]');
        for (var i = 0; i < btns.length; i++) {
            var b = btns[i], k = b.getAttribute('data-k'), v = b.getAttribute('data-v');
            var cur = cfg ? String(cfg[k]) : null;
            if (v !== null) {
                b.className = 'hifi' + (cur === v ? ' on' : '');
                b.disabled = !cfg || busy || (k === 'submode' && usb && v !== 'std');
            } else {   // on/off switch
                var on = cur === '1';
                b.className = 'hifi' + (on ? ' on' : '');
                b.textContent = on ? T.on : T.off;
                b.disabled = !cfg || busy;
            }
        }
        if (cfg) {
            $('v-mode').textContent = cfg.mode === 'ext' ? T.ext : T.pll;
            $('v-mclk').textContent = (cfg.mclk || '?') + ' × FS';
            $('v-sub').textContent = T.sub[cfg.submode] || cfg.submode;
        }
        if (usb) $('sub-hint').textContent = T.usbStd;
        $('reboot-banner').hidden = !pendingReboot;
    }

    function apply(k, v) {
        if (busy || !cfg) return;
        if (String(cfg[k]) === v) return;
        var needsReboot = k === 'mode' || (k === 'mclk' && cfg.mode === 'ext');
        busy = true; render(); status(T.applying, true);
        fetch('handle_i2s.php', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: encodeURIComponent(k) + '=' + encodeURIComponent(v)
        }).then(function (r) {
            return r.text().then(function (txt) {
                if (r.status === 409 && k === 'submode') throw new Error(T.needI2s);
                if (r.status === 409) throw new Error(T.busy);
                if (r.status === 403) throw new Error(T.usbStd);
                if (!r.ok) throw new Error('HTTP ' + r.status + (txt ? ': ' + txt.slice(0, 100) : ''));
            });
        }).then(function () {
            if (needsReboot) setPending(true);
            status(T.saved);
        }).catch(function (e) {
            status(e.message || 'Ошибка');
        }).then(function () {
            busy = false;
            return load();
        });
    }

    function reboot() {
        busy = true; render(); status(T.rebooting, true);
        setPending(false);
        fetch('reboot.php', { method: 'POST' }).catch(function () {}).then(function () {
            var tries = 0;
            setTimeout(function again() {
                fetch('handle_i2s.php?action=getStatus', { cache: 'no-store' }).then(function (r) {
                    if (r.ok) location.href = 'index.php'; else throw 0;
                }).catch(function () { if (++tries < 120) setTimeout(again, 3000); });
            }, 10000);
        });
    }

    function init() {
        if (!RU) {
            var ts = document.querySelectorAll('[data-t]');
            for (var i = 0; i < ts.length; i++) {
                var k = ts[i].getAttribute('data-t');
                if (EN_TEXT[k]) ts[i].textContent = EN_TEXT[k];
            }
            document.documentElement.lang = 'en';
        }
        var btns = document.querySelectorAll('[data-k]');
        for (var j = 0; j < btns.length; j++) {
            btns[j].addEventListener('click', function () {
                var k = this.getAttribute('data-k'), v = this.getAttribute('data-v');
                if (v === null) v = cfg && String(cfg[k]) === '1' ? '0' : '1';   // switch
                apply(k, v);
            });
        }
        $('reboot-btn').addEventListener('click', reboot);
        render();
        load();
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();
