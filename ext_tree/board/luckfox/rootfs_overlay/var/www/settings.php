<?php require_once 'config.php'; ?>
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#101113" media="(prefers-color-scheme: dark)">
    <meta name="theme-color" content="#eef0f2" media="(prefers-color-scheme: light)">
    <title>DigiFox — Настройки</title>
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
        <div class="brand" data-t="title">НАСТРОЙКИ</div>
        <span class="dot" id="dot"></span>
    </header>

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
            <div class="hint" data-t="fLoudHint">На тихой громкости ухо хуже слышит низ. Тонкомпенсация мягко поднимает самый низ (ниже 80 Гц) тем сильнее, чем тише громкость усилителя: в верхних 20 дБ шкалы звук не меняется, на −40 дБ подъём около 4 дБ, не больше 8 дБ. Середина и верх не трогаются.</div>
            <div class="hint" data-t="fHint">Слышно примерно через секунду, можно сравнивать на ходу. Минимальная фаза — без «звона» перед атакой (как SHORT у AK4137). Пологий срез — мягче на самом верху (как SLOW). Запас −3 dB убирает перегрузку пиков между отсчётами на громких записях; громкость добирается усилителем.</div>
        </div>
        <div class="hint" data-t="srcHint">AK4137 — частоту пересчитывает усилитель (AK4137), звук идёт с Фокса как есть. ФОКС — Фокс сам переводит всё в 192 кГц / 32 бит (DSD до DSD256). Переключение перезапускает плеер.</div>
    </section>

    <!-- AK4137 (усилитель, amp.php) -->
    <section class="panel">
        <div class="cap">ПРЕОБРАЗОВАТЕЛЬ ЧАСТОТЫ AK4137</div>
        <div class="set-label">Фильтр</div>
        <div class="row2 tight">
            <button class="hifi" data-a="filter" data-v="0">SHARP</button>
            <button class="hifi" data-a="filter" data-v="1">SLOW</button>
        </div>
        <div class="set-label">Задержка фильтра</div>
        <div class="row2 tight">
            <button class="hifi" data-a="delay" data-v="0">NORMAL</button>
            <button class="hifi" data-a="delay" data-v="1">SHORT</button>
        </div>
        <div class="set-label">Уровень DSD</div>
        <div class="row2 tight">
            <button class="hifi" data-a="dsdgain" data-v="0">0 dB</button>
            <button class="hifi" data-a="dsdgain" data-v="1">+6 dB</button>
        </div>
    </section>

    <!-- AX5689 (усилитель, amp.php) -->
    <section class="panel">
        <div class="cap">РЕЖИМ УСИЛИТЕЛЯ</div>
        <div class="row2">
            <button class="hifi" data-a="mode" data-v="0">ZCM</button>
            <button class="hifi" data-a="mode" data-v="1">BD</button>
        </div>
    </section>

    <section class="panel">
        <div class="set-head"><div class="cap grow">MCLK</div><div class="set-val" id="v-mclk"></div></div>
        <div class="row2">
            <button class="hifi" data-k="mclk" data-v="512">512 × FS</button>
            <button class="hifi" data-k="mclk" data-v="1024">1024 × FS</button>
        </div>
        <div class="hint" data-t="mclkHint">Частота MCLK на усилитель. Меняется сразу.</div>
    </section>

    <footer class="foot">DigiFox v<?php echo htmlspecialchars(VERSION); ?></footer>
</main>

<script src="assets/js/digifox-settings.js?v=<?php echo VERSION; ?>"></script>
</body>
</html>
