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
        <div class="set-head"><div class="cap grow" data-t="output">ВЫХОД</div><div class="set-val" id="v-sub"></div></div>
        <div class="row4">
            <button class="hifi" data-k="submode" data-v="std">STD</button>
            <button class="hifi" data-k="submode" data-v="8ch">8CH</button>
            <button class="hifi" data-k="submode" data-v="lr">L / R</button>
            <button class="hifi" data-k="submode" data-v="plr">±L / ±R</button>
        </div>
        <div class="hint" id="sub-hint" data-t="outputHint">STD — стерео; 8CH — 8 каналов до 192 кГц; L/R — дуал-моно; ±L/±R — дуал-моно с балансным выходом. Применяется сразу.</div>
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
