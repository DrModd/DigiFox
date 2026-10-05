<?php require_once 'config.php'; ?>
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#101113" media="(prefers-color-scheme: dark)">
    <meta name="theme-color" content="#eef0f2" media="(prefers-color-scheme: light)">
    <title>DigiFox — I2S</title>
    <link rel="icon" href="favicon.svg" type="image/svg+xml">
    <link rel="icon" href="favicon.ico" sizes="any">
    <link rel="apple-touch-icon" href="apple-touch-icon.png">
    <link rel="stylesheet" href="assets/css/digifox.css?v=<?php echo VERSION; ?>">
</head>
<body>
<main class="wrap">

    <header class="head">
        <a class="back" href="index.php" aria-label="Назад">
            <svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path d="M15 5l-7 7 7 7" stroke="currentColor" stroke-width="1.6" fill="none"/></svg>
        </a>
        <div class="brand">I2S</div>
        <span class="dot" id="dot"></span>
    </header>

    <div class="banner" id="reboot-banner" hidden>
        <div class="grow" data-t="needReboot">Тактирование изменится после перезагрузки Фокса</div>
        <button class="hifi on" id="reboot-btn" data-t="reboot">ПЕРЕЗАГРУЗИТЬ</button>
    </div>

    <div class="status" id="status"></div>

    <section class="panel">
        <div class="set-head"><div class="cap grow" data-t="src">ПЕРЕСЧЁТ ЧАСТОТЫ</div><div class="set-val" id="v-src"></div></div>
        <div class="row2">
            <button class="hifi" data-src="ak4137">AK4137</button>
            <button class="hifi" data-src="fox" data-t="srcFox">ФОКС</button>
        </div>
        <div id="src-filter" hidden>
            <div class="set-label" data-t="fPhase">Фаза фильтра</div>
            <div class="row3 tight">
                <button class="hifi" data-f="phase" data-v="lin" data-t="fLin">ЛИНЕЙНАЯ</button>
                <button class="hifi" data-f="phase" data-v="int" data-t="fInt">ПРОМЕЖ.</button>
                <button class="hifi" data-f="phase" data-v="min" data-t="fMin">МИНИМАЛ.</button>
            </div>
            <div class="set-label" data-t="fRoll">Срез</div>
            <div class="row3 tight">
                <button class="hifi" data-f="rolloff" data-v="steep" data-t="fSteep">КРУТОЙ</button>
                <button class="hifi" data-f="rolloff" data-v="std" data-t="fStd">ОБЫЧНЫЙ</button>
                <button class="hifi" data-f="rolloff" data-v="slow" data-t="fSlow">ПОЛОГИЙ</button>
            </div>
            <div class="set-label" data-t="fGain">Запас по уровню</div>
            <div class="row2 tight">
                <button class="hifi" data-f="gain" data-v="0">0 dB</button>
                <button class="hifi" data-f="gain" data-v="-3">−3 dB</button>
            </div>
            <div class="set-label" data-t="fLoud">Тонкомпенсация</div>
            <div class="row2 tight">
                <button class="hifi" data-f="loudness" data-v="off" data-t="offU">ВЫКЛ</button>
                <button class="hifi" data-f="loudness" data-v="on" data-t="onU">ВКЛ</button>
            </div>
            <div class="hint" data-t="fLoudHint">На тихой громкости ухо хуже слышит низ и верх. Тонкомпенсация поднимает их тем сильнее, чем тише стоит громкость усилителя: в верхних 10 дБ шкалы звук не меняется, на −40 дБ низ поднят на ~10 дБ, верх на ~3 дБ. Чтобы не было перегрузки, середина при этом тише — громкость чуть добавьте.</div>
            <div class="hint" data-t="fHint">Слышно примерно через секунду, можно сравнивать на ходу. Минимальная фаза — без «звона» перед атакой (как SHORT у AK4137). Пологий срез — мягче на самом верху (как SLOW). Запас −3 dB убирает перегрузку пиков между отсчётами на громких записях; громкость добирается усилителем.</div>
        </div>
        <div class="hint" data-t="srcHint">AK4137 — Фокс отдаёт звук как есть, частоту пересчитывает AK4137 в усилителе. ФОКС — Фокс сам переводит всё в PCM 192 кГц / 32 бит: PCM через soxr, DSD64–DSD256 через дециматор; DSD512 в этом режиме не поддерживается. Переключение перезапускает плеер.</div>
    </section>

    <section class="panel">
        <div class="set-head"><div class="cap grow" data-t="clock">ТАКТИРОВАНИЕ</div><div class="set-val" id="v-mode"></div></div>
        <div class="row2">
            <button class="hifi" data-k="mode" data-v="pll">PLL</button>
            <button class="hifi" data-k="mode" data-v="ext">EXT</button>
        </div>
        <div class="hint" data-t="clockHint">PLL — встроенный синтезатор RV1106, вывод MCLK работает как выход. EXT — внешний генератор, MCLK — вход. Применяется после перезагрузки.</div>
    </section>

    <section class="panel">
        <div class="set-head"><div class="cap grow">MCLK</div><div class="set-val" id="v-mclk"></div></div>
        <div class="row2">
            <button class="hifi" data-k="mclk" data-v="512">512 × FS</button>
            <button class="hifi" data-k="mclk" data-v="1024">1024 × FS</button>
        </div>
        <div class="hint" data-t="mclkHint">В режиме PLL меняется сразу, в режиме EXT — после перезагрузки.</div>
    </section>

    <section class="panel">
        <div class="cap" data-t="swaps">ПЕРЕСТАНОВКИ</div>
        <div class="switch-row">
            <div class="grow"><div class="t" data-t="pcmSwap">Каналы PCM</div><div class="d" data-t="pcmSwapD">Поменять местами левый и правый</div></div>
            <button class="hifi" data-k="pcm_swap">—</button>
        </div>
        <div class="switch-row">
            <div class="grow"><div class="t" data-t="dsdSwap">Каналы DSD</div><div class="d" data-t="dsdSwapD">Поменять физические линии DSD</div></div>
            <button class="hifi" data-k="dsd_swap">—</button>
        </div>
        <div class="switch-row">
            <div class="grow"><div class="t" data-t="freqSwap">Частоты 44,1 / 48</div><div class="d" data-t="freqSwapD">Поменять местами частотные домены 44,1 и 48 кГц</div></div>
            <button class="hifi" data-k="freq_swap">—</button>
        </div>
    </section>

    <footer class="foot">DigiFox v<?php echo htmlspecialchars(VERSION); ?></footer>
</main>

<script src="assets/js/digifox-settings.js?v=<?php echo VERSION; ?>"></script>
</body>
</html>
