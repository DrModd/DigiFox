<?php
// amp_flash.php — обновление прошивки усилителя DigiD D1 из веб-интерфейса.
//   POST multipart fw=<файл .bin>  -> текстовый журнал digifox-ampflash (по мере выполнения)
//   POST builtin=1                 -> прошивка, встроенная в DigiFox (/usr/share/digifox/amp-fw.bin)
header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');
header('X-Accel-Buffering: no');

$builtin = '/usr/share/digifox/amp-fw.bin';
$bin = '/tmp/amp_fw.bin';
if ($_SERVER['REQUEST_METHOD'] === 'POST' && ($_POST['builtin'] ?? '') === '1') {
    if (!is_file($builtin) || !@copy($builtin, $bin)) {
        http_response_code(404);
        echo "ОШИБКА: в этой версии DigiFox нет встроенной прошивки усилителя\n";
        exit;
    }
} else {
    if ($_SERVER['REQUEST_METHOD'] !== 'POST' || !isset($_FILES['fw'])) {
        http_response_code(400);
        echo "ОШИБКА: файл прошивки не передан\n";
        exit;
    }
    $f = $_FILES['fw'];
    if ($f['error'] !== UPLOAD_ERR_OK || $f['size'] < 4096 || $f['size'] > 63488) {
        http_response_code(400);
        echo "ОШИБКА: нужен файл .bin прошивки усилителя (4..62 КБ)\n";
        exit;
    }
    if (!move_uploaded_file($f['tmp_name'], $bin)) {
        http_response_code(500);
        echo "ОШИБКА: не удалось сохранить файл\n";
        exit;
    }
}

@set_time_limit(300);
while (ob_get_level() > 0) ob_end_flush();
ob_implicit_flush(true);

$p = popen('/usr/bin/sudo /usr/bin/digifox-ampflash ' . escapeshellarg($bin) . ' 2>&1', 'r');
if (!$p) { echo "ОШИБКА: не удалось запустить digifox-ampflash\n"; exit; }
while (($l = fgets($p)) !== false) {
    echo $l;
    flush();
}
$rc = pclose($p);
@unlink($bin);
echo $rc === 0 ? "\n[OK]\n" : "\n[FAIL]\n";
