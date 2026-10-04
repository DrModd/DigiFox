<?php
define('PUREFOX_VERSION', '2.8');   // upstream PureFox base
define('DIGIFOX_VERSION', '1.0');
// full build id (e.g. 1.0.12), written at build time by post-build.sh
$digifox_build = @file_get_contents('/etc/digifox-release');
define('VERSION', $digifox_build !== false && trim($digifox_build) !== '' ? trim($digifox_build) : DIGIFOX_VERSION);


