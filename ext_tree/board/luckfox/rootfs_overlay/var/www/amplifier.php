<?php require_once 'config.php'; ?>
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#101113" media="(prefers-color-scheme: dark)">
    <meta name="theme-color" content="#eef0f2" media="(prefers-color-scheme: light)">
    <title>DigiFox — усилитель</title>
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
        <div class="brand">DIGID D1</div>
        <span class="dot" id="dot"></span>
    </header>

    <div class="status" id="status"></div>

    <!-- AK4137 -->
    <section class="panel">
        <div class="cap">ПРЕОБРАЗОВАТЕЛЬ ЧАСТОТЫ AK4137</div>
        <div class="set-label">Фильтр</div>
        <div class="row2 tight">
            <button class="hifi" data-k="filter" data-v="0">SHARP</button>
            <button class="hifi" data-k="filter" data-v="1">SLOW</button>
        </div>
        <div class="set-label">Задержка фильтра</div>
        <div class="row2 tight">
            <button class="hifi" data-k="delay" data-v="0">NORMAL</button>
            <button class="hifi" data-k="delay" data-v="1">SHORT</button>
        </div>
        <div class="set-label">Уровень DSD</div>
        <div class="row2 tight">
            <button class="hifi" data-k="dsdgain" data-v="0">0 dB</button>
            <button class="hifi" data-k="dsdgain" data-v="1">+6 dB</button>
        </div>
    </section>

    <!-- AX5689 -->
    <section class="panel">
        <div class="cap">РЕЖИМ AX5689</div>
        <div class="row2">
            <button class="hifi" data-k="mode" data-v="0">ZCM</button>
            <button class="hifi" data-k="mode" data-v="1">BD</button>
        </div>
    </section>

    <!-- Power -->
    <section class="panel">
        <div class="cap">ПИТАНИЕ</div>
        <div class="set-label">Автовыключение без сигнала</div>
        <div class="row3 tight">
            <button class="hifi" data-k="standby" data-v="0">ВЫКЛ</button>
            <button class="hifi" data-k="standby" data-v="1">20 МИН</button>
            <button class="hifi" data-k="standby" data-v="2">60 МИН</button>
        </div>
        <div class="set-label">Включаться, когда Фокс начал играть</div>
        <div class="row2 tight">
            <button class="hifi" data-k="autoon" data-v="0">ВЫКЛ</button>
            <button class="hifi" data-k="autoon" data-v="1">ВКЛ</button>
        </div>
    </section>

    <!-- Sleep timer -->
    <section class="panel">
        <div class="set-head"><div class="cap grow">ТАЙМЕР СНА</div><div class="set-val" id="sleep-val"></div></div>
        <div class="row4">
            <button class="hifi" data-sleep="15">15 МИН</button>
            <button class="hifi" data-sleep="30">30 МИН</button>
            <button class="hifi" data-sleep="60">1 ЧАС</button>
            <button class="hifi" data-sleep="90">1,5 ЧАСА</button>
        </div>
        <button class="hifi wide" id="sleep-off" hidden>ОТМЕНИТЬ ТАЙМЕР</button>
    </section>

    <!-- Alarm -->
    <section class="panel">
        <div class="set-head"><div class="cap grow">БУДИЛЬНИК</div><div class="set-val" id="clock-val"></div></div>
        <div class="alarm-top">
            <input type="time" id="a-time" value="07:00" aria-label="Время будильника">
            <button class="hifi" id="a-on">ВЫКЛ</button>
        </div>
        <div class="row7" id="a-days">
            <button class="hifi" data-d="1">ПН</button><button class="hifi" data-d="2">ВТ</button>
            <button class="hifi" data-d="3">СР</button><button class="hifi" data-d="4">ЧТ</button>
            <button class="hifi" data-d="5">ПТ</button><button class="hifi" data-d="6">СБ</button>
            <button class="hifi" data-d="7">ВС</button>
        </div>
        <div class="set-label">Источник</div>
        <select id="a-src" aria-label="Источник">
            <option value="keep">Последний плеер</option>
            <option value="aplayer">Веб-радио</option>
            <option value="mpd">MPD</option>
            <option value="lms">Squeezelite</option>
            <option value="aprenderer">UPnP Renderer</option>
            <option value="raat">Roon Ready</option>
            <option value="qobuz">Qobuz Connect</option>
            <option value="spotify">Spotify Connect</option>
        </select>
        <div class="set-label">Громкость <span id="a-vol-val"></span></div>
        <input type="range" id="a-vol" min="-60" max="0" value="-30" aria-label="Громкость будильника">
        <div class="hint">Усилитель включится, запустит источник и за минуту плавно поднимет громкость с −20 dB от выбранной. Qobuz и Spotify сами не заиграют — их запускает телефон.</div>
        <div class="set-label">Часовой пояс</div>
        <select id="tz" aria-label="Часовой пояс"></select>
        <button class="hifi wide on" id="a-save">СОХРАНИТЬ БУДИЛЬНИК</button>
    </section>

    <!-- Firmware -->
    <section class="panel">
        <div class="set-head"><div class="cap grow">ПРОШИВКА УСИЛИТЕЛЯ</div><div class="set-val" id="fw-ver"></div></div>
        <label class="file" for="fw-file"><span id="fw-name">Выбрать файл .bin…</span></label>
        <input type="file" id="fw-file" accept=".bin,application/octet-stream" hidden>
        <button class="hifi wide" id="fw-go" disabled>ПРОШИТЬ</button>
        <div class="hint">Файл Test_i2c.bin из папки Debug проекта в STM32CubeIDE. Настройки и коды пульта сохранятся. Во время прошивки усилитель выключен.</div>
        <pre class="log small" id="fw-log" hidden></pre>
    </section>

    <footer class="foot">DigiFox v<?php echo htmlspecialchars(VERSION); ?></footer>
</main>

<script src="assets/js/digifox-amp.js?v=<?php echo VERSION; ?>"></script>
</body>
</html>
