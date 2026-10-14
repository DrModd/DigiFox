<?php

const PUREFOX_AUDIO_LOCK_FILE = '/tmp/purefox-audio.lock';

function acquireAudioTransitionLock() {
    $lock = fopen(PUREFOX_AUDIO_LOCK_FILE, 'ce');
    if ($lock === false) {
        return false;
    }

    // DigiFox: подождать до 8 с, если переключение ещё идёт (выход из
    // USB -> I2S, обработчик USB), а не отказывать сразу
    for ($i = 0; !flock($lock, LOCK_EX | LOCK_NB); $i++) {
        if ($i >= 80) {
            fclose($lock);
            return false;
        }
        usleep(100000);
    }

    return $lock;
}

function releaseAudioTransitionLock($lock) {
    if (is_resource($lock)) {
        flock($lock, LOCK_UN);
        fclose($lock);
    }
}
