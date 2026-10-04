// amp.js — DigiFox: the web UI volume slider controls the DigiD D1 amplifier.
//
// The Fox output is always 100% (bit-perfect); volume lives in the amplifier
// MCU (STM32) + AX5689. This script takes over the volume slider, the mute icon,
// the mouse wheel and the arrow/volume keys and talks to amp.php, the same way
// the Fox Remote app does:
//   amp.php -> /tmp/amp_req -> pfctl serve -> UART -> STM32 -> AX5689
//
// app.js is left untouched: the slider, icon and display are replaced with
// clones, so its own handlers stay bound to detached elements.
(function () {
    'use strict';

    var POLL_MS = 1000;        // amplifier state poll
    var SEND_MS = 120;         // at most one volume request per this interval
    var HOLD_MS = 1500;        // keep the user's value this long after the last move

    function swap(id) {
        var old = document.getElementById(id);
        if (!old) return null;
        var el = old.cloneNode(true);
        old.parentNode.replaceChild(el, old);
        return el;
    }

    var slider = swap('volume-slider');
    var icon = swap('volume-icon');
    var display = swap('volume-display');
    if (!slider || !display) return;

    var ru = (window.currentLang || navigator.language || 'en').toLowerCase().indexOf('ru') === 0;
    var T = ru
        ? { vol: 'Громкость усилителя', mute: 'Включить звук', off: 'Усилитель выключен', none: 'Усилитель не на связи' }
        : { vol: 'Amplifier volume', mute: 'Unmute', off: 'Amplifier is off', none: 'Amplifier not connected' };

    var amp = null;            // last state from amp.php
    var lastUserAt = 0;        // time of the last user change
    var pending = null;        // value waiting to be sent
    var inFlight = false;
    var sendTimer = null;
    var pollTimer = null;

    function usable() {
        return !!(amp && amp.present && amp.power);
    }

    function fmt(pos) {
        if (!amp) return '--';
        return amp.db ? (pos - amp.max) + ' dB' : String(pos);
    }

    function setEnabled(on) {
        slider.disabled = !on;
        slider.style.opacity = on ? '1' : '0.4';
        slider.style.cursor = on ? 'pointer' : 'not-allowed';
        slider.style.pointerEvents = on ? 'auto' : 'none';
        if (icon) {
            icon.style.opacity = on ? '1' : '0.4';
            icon.style.cursor = on ? 'pointer' : 'not-allowed';
            icon.style.pointerEvents = on ? 'auto' : 'none';
        }
    }

    function render() {
        if (!amp || !amp.present) {
            setEnabled(false);
            display.textContent = '--';
            slider.title = T.none;
            if (icon) { icon.src = 'assets/img/volume.svg'; icon.title = T.none; }
            return;
        }
        slider.min = '0';
        slider.max = String(amp.max);
        if (!amp.power) {
            setEnabled(false);
            display.textContent = 'OFF';
            slider.title = T.off;
            if (icon) { icon.src = 'assets/img/volume.svg'; icon.title = T.off; }
            return;
        }
        setEnabled(true);
        slider.title = T.vol;
        var holding = pending !== null || inFlight || Date.now() - lastUserAt < HOLD_MS;
        if (!holding && slider.value !== String(amp.pos)) slider.value = String(amp.pos);
        display.textContent = fmt(Number(slider.value));
        if (icon) {
            icon.src = amp.mute ? 'assets/img/mute.svg' : 'assets/img/volume.svg';
            icon.title = amp.mute ? T.mute : T.vol;
        }
    }

    function post(body) {
        return fetch('amp.php', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (r) { return r.json(); });
    }

    function poll() {
        fetch('amp.php', { cache: 'no-store' })
            .then(function (r) { return r.json(); })
            .then(function (st) { amp = st; render(); })
            .catch(function () { amp = null; render(); });
    }

    function schedulePoll(ms) {
        clearTimeout(pollTimer);
        pollTimer = setTimeout(function loop() {
            if (document.visibilityState !== 'hidden') poll();
            pollTimer = setTimeout(loop, POLL_MS);
        }, ms);
    }

    function send() {
        sendTimer = null;
        if (inFlight || pending === null) return;
        var pos = pending;
        pending = null;
        inFlight = true;
        post('action=vol&pos=' + pos)
            .then(function (r) {
                if (r && r.ok && amp) { amp.pos = pos; amp.mute = false; }
            })
            .catch(function (e) { console.error('amp volume:', e); })
            .then(function () {
                inFlight = false;
                if (pending !== null) sendTimer = setTimeout(send, SEND_MS);
                else render();
            });
    }

    function setPos(pos) {
        if (!usable()) return;
        pos = Math.max(0, Math.min(amp.max, Math.round(pos)));
        slider.value = String(pos);
        display.textContent = fmt(pos);
        lastUserAt = Date.now();
        pending = pos;
        if (!inFlight && !sendTimer) sendTimer = setTimeout(send, 0);
    }

    function step(dir) {
        if (!usable()) return;
        setPos(Number(slider.value) + dir);
    }

    // ---- slider ----
    slider.addEventListener('input', function () { setPos(Number(this.value)); });
    slider.addEventListener('change', function () { setPos(Number(this.value)); });

    // ---- mute ----
    if (icon) {
        icon.addEventListener('click', function (e) {
            e.preventDefault();
            if (!usable()) return;
            post('action=mute')
                .catch(function (err) { console.error('amp mute:', err); })
                .then(function () { schedulePoll(250); });   // the amplifier confirms in ~0.2 s
        });
    }

    // ---- keys and wheel anywhere on the page (as app.js did for the Fox volume) ----
    function isTextEntry(t) {
        if (!t || t.nodeType !== 1) return false;
        if (t.isContentEditable) return true;
        var tag = t.tagName.toLowerCase();
        if (tag === 'textarea' || tag === 'select') return true;
        if (tag !== 'input') return false;
        var type = (t.type || '').toLowerCase();
        return ['range', 'button', 'checkbox', 'radio', 'submit', 'reset'].indexOf(type) < 0;
    }

    function keyDir(e) {
        if (e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return 0;
        var k = e.key || '';
        if (['ArrowUp', 'ArrowRight', 'AudioVolumeUp', 'VolumeUp', 'MediaVolumeUp'].indexOf(k) >= 0 ||
            e.keyCode === 175 || e.keyCode === 24) return 1;
        if (['ArrowDown', 'ArrowLeft', 'AudioVolumeDown', 'VolumeDown', 'MediaVolumeDown'].indexOf(k) >= 0 ||
            e.keyCode === 174 || e.keyCode === 25) return -1;
        return 0;
    }

    document.addEventListener('keydown', function (e) {
        if (document.visibilityState === 'hidden' || isTextEntry(e.target)) return;
        var d = keyDir(e);
        if (!d) return;
        if (e.target === slider) {                // the slider moves itself, 'input' sends it
            return;
        }
        e.preventDefault();
        e.stopPropagation();
        step(d);
    }, true);

    document.addEventListener('wheel', function (e) {
        if (document.visibilityState === 'hidden' || e.deltaY === 0 || isTextEntry(e.target) || !usable()) return;
        e.preventDefault();
        e.stopPropagation();
        step(e.deltaY < 0 ? 1 : -1);
    }, { passive: false, capture: true });

    document.addEventListener('visibilitychange', function () {
        if (document.visibilityState === 'visible') schedulePoll(0);
    });

    render();
    schedulePoll(0);
})();
