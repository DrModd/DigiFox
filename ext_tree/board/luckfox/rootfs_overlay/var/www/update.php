<?php require_once 'config.php'; ?>
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#101113" media="(prefers-color-scheme: dark)">
    <meta name="theme-color" content="#eef0f2" media="(prefers-color-scheme: light)">
    <title>DigiFox — обновление</title>
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
        <div class="brand">ОБНОВЛЕНИЕ</div>
        <span class="dot" id="dot"></span>
    </header>

    <div class="status" id="status"></div>

    <!-- DigiFox -->
    <section class="panel">
        <div class="set-head"><div class="cap grow">DIGIFOX</div><div class="set-val">установлена <?php echo htmlspecialchars(VERSION); ?></div></div>
        <div class="upd-state dim" id="fox-state">Проверяю…</div>
        <button class="hifi wide" id="fox-notes" hidden>ЧТО НОВОГО</button>
        <ul class="changes" id="fox-changes" hidden></ul>
        <button class="hifi wide on" id="fox-install" hidden></button>
        <button class="hifi wide" id="fox-check">ПРОВЕРИТЬ СНОВА</button>
        <div class="hint">Во время обновления звук прервётся, затем Фокс перезагрузится (около 3 минут). Настройки сохранятся.</div>
        <pre class="log small" id="fox-log" hidden></pre>
    </section>

    <!-- Amplifier -->
    <section class="panel">
        <div class="set-head"><div class="cap grow">УСИЛИТЕЛЬ DIGID D1</div><div class="set-val" id="amp-ver"></div></div>
        <div class="upd-state dim" id="amp-state">—</div>
        <button class="hifi wide" id="amp-notes" hidden>ЧТО НОВОГО</button>
        <ul class="changes" id="amp-changes" hidden></ul>
        <button class="hifi wide on" id="amp-install" hidden></button>
        <div class="hint">Около минуты усилитель будет выключен. Настройки и коды пульта сохранятся.</div>
        <details class="manual">
            <summary>Прошить файл вручную</summary>
            <label class="file" for="fw-file"><span id="fw-name">Выбрать файл .bin…</span></label>
            <input type="file" id="fw-file" accept=".bin,application/octet-stream" hidden>
            <button class="hifi wide" id="fw-go" disabled>ПРОШИТЬ</button>
        </details>
        <pre class="log small" id="amp-log" hidden></pre>
    </section>

    <footer class="foot">DigiFox v<?php echo htmlspecialchars(VERSION); ?></footer>
</main>

<script src="assets/js/digifox-update.js?v=<?php echo VERSION; ?>"></script>
</body>
</html>
