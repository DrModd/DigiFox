<?php require_once 'config.php'; ?>
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#101113" media="(prefers-color-scheme: dark)">
    <meta name="theme-color" content="#eef0f2" media="(prefers-color-scheme: light)">
    <title>DigiFox</title>
    <link rel="icon" href="favicon.svg" type="image/svg+xml">
    <link rel="icon" href="favicon.ico" sizes="any">
    <link rel="apple-touch-icon" href="apple-touch-icon.png">
    <link rel="stylesheet" href="assets/css/digifox.css?v=<?php echo VERSION; ?>">
</head>
<body>
<main class="wrap">

    <header class="head">
        <div class="brand">DIGIFOX</div>
        <div class="host" id="host"></div>
        <span class="dot" id="dot" title=""></span>
        <button class="icon-btn" id="menu-btn" aria-label="Меню">
            <svg viewBox="0 0 24 24" width="20" height="20" aria-hidden="true"><path d="M4 7h16M4 12h16M4 17h16" stroke="currentColor" stroke-width="1.5" fill="none"/></svg>
        </button>
    </header>

    <section class="display">
        <div class="cap" data-t="source">ИСТОЧНИК</div>
        <div class="source" id="source">—</div>
        <div class="track" id="track" hidden>
            <div class="title" id="t-title"></div>
            <div class="artist" id="t-artist"></div>
        </div>
        <div class="rate-row">
            <span class="rate" id="rate">&nbsp;</span>
            <span class="cap" id="mode-cap"></span>
        </div>
    </section>

    <div class="status" id="status"></div>

    <section class="panel">
    <div class="power-row" id="power-row">
        <div class="grow">
            <div class="cap" data-t="amp">УСИЛИТЕЛЬ DIGID D1</div>
            <div class="power-state" id="power-state">—</div>
        </div>
        <button class="hifi power" id="power-btn" data-t="power">POWER</button>
    </div>

    <div class="volume">
        <div class="vol-head">
            <div class="grow">
                <div class="cap" data-t="volume">ГРОМКОСТЬ УСИЛИТЕЛЯ</div>
                <div class="vol-num"><span id="vol-num">--</span><span class="unit" id="vol-unit"></span></div>
            </div>
            <button class="hifi mute" id="mute-btn">MUTE</button>
        </div>
        <input type="range" id="vol" min="0" max="100" value="0" disabled aria-label="Громкость">
        <div class="note" id="vol-note"></div>
    </div>
    </section>

    <section class="panel">
        <div class="cap" data-t="mode">РЕЖИМ</div>
        <div class="row2">
            <button class="hifi" id="mode-net" data-t="net">СЕТЬ</button>
            <button class="hifi" id="mode-usb">USB → I2S</button>
        </div>
    </section>

    <section class="panel">
        <div class="cap" data-t="players">ПЛЕЕРЫ</div>
        <div class="grid" id="players"></div>
    </section>

    <footer class="foot" id="foot">DigiFox v<?php echo htmlspecialchars(VERSION); ?></footer>
</main>

<!-- menu -->
<div class="sheet" id="menu" hidden>
    <div class="sheet-box">
        <div class="cap">DIGIFOX v<?php echo htmlspecialchars(VERSION); ?></div>
        <a class="menu-item" href="settings.php" data-t="m_i2s">Настройки I2S</a>
        <button class="menu-item" id="m-update" data-t="m_update">Обновление прошивки</button>
        <button class="menu-item" id="m-reboot" data-t="m_reboot">Перезагрузить Фокс</button>
        <a class="menu-item" href="classic.php" data-t="m_classic">Классический интерфейс PureFox</a>
        <button class="menu-item dim" id="m-close" data-t="close">Закрыть</button>
    </div>
</div>

<!-- confirm -->
<div class="sheet" id="confirm" hidden>
    <div class="sheet-box">
        <div class="confirm-text" id="confirm-text"></div>
        <div class="row2">
            <button class="hifi" id="confirm-no" data-t="cancel">ОТМЕНА</button>
            <button class="hifi on" id="confirm-yes">OK</button>
        </div>
    </div>
</div>

<!-- update log -->
<div class="sheet" id="update" hidden>
    <div class="sheet-box wide">
        <div class="cap" data-t="m_update">ОБНОВЛЕНИЕ ПРОШИВКИ</div>
        <pre class="log" id="update-log"></pre>
        <button class="hifi" id="update-close" data-t="close">ЗАКРЫТЬ</button>
    </div>
</div>

<script src="assets/js/digifox.js?v=<?php echo VERSION; ?>"></script>
</body>
</html>
