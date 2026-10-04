#!/bin/sh
# DigiFox: online update from the upstream PureFox server is disabled.
# It rsyncs the whole rootfs from luckfox.puredsd.ru and would overwrite
# DigiFox with stock PureFox. Flash a new DigiFox image instead.
echo "DigiFox: online update is disabled."
echo "Download a new image from https://github.com/DrModd/DigiFox and flash it with RV1106_Toolkit."
exit 0
