# DigiFox

**Сетевой стример для усилителя DigiD D1 на Luckfox Pico Max (Rockchip RV1106)**

DigiFox — форк прошивки [PureFox](https://github.com/ppy2/PureFox_v2) (автор — ppy2, форум [PureDSD](https://forum.puredsd.ru/t/luckfox-pico-max-ultra-endpoint-s-vneshnimi-klokami-na-rockchip-rv1106/1172)), переработанный под цифровой усилитель DigiD D1 (STM32 + AX5689 + AK4137).
Описание исходной прошивки PureFox: [README_PUREFOX.md](README_PUREFOX.md).

## Отличия от PureFox

- **Фиксированная громкость 100%.** Громкостью управляют МК и AX5689 в усилителе, стример отдаёт сигнал без изменений:
  - убран `volume-encoder` (программная громкость с энкодера);
  - librespot запускается с `--volume-ctrl fixed`;
  - shairport-sync: `ignore_volume_control = "yes"`;
  - MPD: `mixer_type "none"`;
  - Roon: `volume: null` (как и в PureFox).
- **Убраны** CelMusper и Tidal Connect.
- **Онлайн-обновление отключено.** Штатный механизм PureFox синхронизирует всю rootfs с сервера автора и вернул бы стоковую прошивку. Обновление — только перепрошивкой нового образа DigiFox.
- Имя устройства: `digifox` (`http://digifox/` или `http://digifox.local/`).

### Что настраивается на стороне источника

Эти плееры считают громкость у себя, их нужно выставить на фиксированный уровень вручную:

- **LMS (squeezelite):** настройки плеера в LMS → громкость фиксирована на 100%;
- **HQPlayer (NAA):** громкость 0 dB или отключить регулировку;
- **Qobuz Connect, APlayer, APrenderer:** закрытые бинарники — проверить, что ползунок в приложении не меняет уровень.

## Сборка

Требуется Ubuntu 22.04 (или Docker-контейнер на её основе).

```sh
git clone https://github.com/DrModd/DigiFox.git
cd DigiFox
sudo ./build.sh --bootstrap   # первый раз: ставит зависимости через apt и собирает
./build.sh                    # последующие сборки
./build.sh --clean            # полная пересборка
```

Готовые файлы для прошивки — в `buildroot/output/images/`.

## Прошивка

1. Установите драйверы с [Luckfox Wiki](https://wiki.luckfox.com/Luckfox-Pico-RV1106/Downloads).
2. Запустите RV1106_Toolkit от имени администратора, выберите **rv1106**.
3. Зажмите **BOOT** на плате и подключите USB, дождитесь **Maskrom**.
4. Download → USB → укажите папку с образом, отметьте все файлы → **Download**.
5. После **Download done** подождите минуту и отключите плату.

SSH: логин `root`, пароль `digifox`.

## Синхронизация с PureFox

```sh
git remote add upstream https://github.com/ppy2/PureFox_v2.git
git fetch upstream
git merge upstream/MAX_6.X
```

## Лицензия

GPL-2.0, как и исходный проект (см. [LICENSE](LICENSE)). Закрытые компоненты (Roon RAAT, NAA, squeezeliteR2, Qobuz Connect, APlayer, APrenderer) распространяются на условиях их правообладателей.
