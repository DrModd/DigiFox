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
- **Связь с усилителем встроена в прошивку** (раньше ставилась отдельно из [FoxRemoteN/extras](https://github.com/DrModd/FoxRemoteN/tree/main/extras)):
  - консольный UART (UART2 / `ttyFIQ0`) занят сервером `pfctl serve` — протокол с STM32 (`pf_link`): `@RATE`, `@SRC`, `@USB`, `@TRACK`, команды `src`, `usb`, `st`, `rate`, `track`, `avol`; шелла на этом UART нет, доступ — по SSH;
  - `@RATE` шлёт `pfrate` (C): в течение ~20 мс после открытия потока, чтобы МК успел замьютить AX5689; `STOP` — через 0,4 с (короткая пауза между треками не мигает);
  - `pfmeta` собирает названия треков: Qobuz (вывод qobuz-connect), Spotify (`--onevent` librespot), AirPlay (канал метаданных shairport-sync), MPD;
  - `amp.php`, `rate.php`, `track.php` — для приложения Fox Remote;
  - ползунок громкости, иконка mute, колесо мыши и стрелки в веб-интерфейсе управляют громкостью усилителя (через `amp.php`, как приложение), шкала в дБ;
  - кнопка питания внизу справа включает усилитель и переводит его в дежурный режим (зелёная — включён); сам Фокс ею больше не выключается;
  - команды `vol` и `mute` в `pfctl` отвечают `@ERR fixed_volume`: громкость только в усилителе.
- **Убран `S90ak4137`** (инициализация AK4137 для платы DSD'it): в DigiD D1 AK4137 управляет STM32.
- **Убраны** CelMusper и Tidal Connect.
- **Онлайн-обновление из GitHub Releases этого репозитория** (кнопка с номером версии в веб-интерфейсе, или `/opt/update.sh` по SSH; `--check` — только проверить, `--force` — переустановить):
  - скачивает `rootfs.squashfs` и `boot.img` последнего релиза, проверяет SHA-256, синхронизирует файлы через rsync;
  - сохраняет настройки: I2S (режим, MCLK, подрежим, вывод), активный плеер, пароль root, SSH-ключи, MAC, список радио;
  - ядро перезаписывается, только если изменилось; DTB — для текущего режима I2S;
  - idblock, U-Boot и его окружение онлайн не трогаются — для них полная прошивка через RV1106_Toolkit.
- **Связь с сервером автора PureFox убрана:** нет `export.sh`, `update_ap.sh` больше не подменяет себя с его сервера (плееры APlayer по-прежнему качаются с albumplayer.ru).
- **SSH-ключ хоста свой на каждом устройстве:** генерируется при первом подключении (в PureFox ключ общий и лежит в репозитории).
- Имя устройства: `digifox` (`http://digifox/` или `http://digifox.local/`).

### Что настраивается на стороне источника

Эти плееры считают громкость у себя, их нужно выставить на фиксированный уровень вручную:

- **LMS (squeezelite):** настройки плеера в LMS → громкость фиксирована на 100%;
- **HQPlayer (NAA):** громкость 0 dB или отключить регулировку;
- **Qobuz Connect, APlayer, APrenderer:** закрытые бинарники — проверить, что ползунок в приложении не меняет уровень.

## Готовые образы

Каждое изменение в ветке `digifox` собирается в GitHub Actions; результат — во вкладке **Releases**:
`DigiFox-<версия>.zip` для RV1106_Toolkit, а также `rootfs.squashfs`, `boot.img`, `version.txt`, `SHA256SUMS` для онлайн-обновления.

## Сборка вручную

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

SSH: логин `root`, пароль `digifox`. При первом подключении ключ хоста создаётся заново (несколько секунд).

## Синхронизация с PureFox

```sh
git remote add upstream https://github.com/ppy2/PureFox_v2.git
git fetch upstream
git merge upstream/MAX_6.X
```

## Лицензия

GPL-2.0, как и исходный проект (см. [LICENSE](LICENSE)). Закрытые компоненты (Roon RAAT, NAA, squeezeliteR2, Qobuz Connect, APlayer, APrenderer) распространяются на условиях их правообладателей.
