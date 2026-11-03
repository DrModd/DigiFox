################################################################################
#
# go-librespot (prebuilt release binary)
#
################################################################################

GO_LIBRESPOT_VERSION = 0.10.5
GO_LIBRESPOT_SOURCE = go-librespot_linux_armv6.tar.gz
GO_LIBRESPOT_SITE = https://github.com/devgianlu/go-librespot/releases/download/v$(GO_LIBRESPOT_VERSION)
GO_LIBRESPOT_LICENSE = GPL-3.0
GO_LIBRESPOT_STRIP_COMPONENTS = 0
GO_LIBRESPOT_DEPENDENCIES = alsa-lib

# the binary is named "librespot" in its own directory: the process name stays
# "librespot", so status_monitor, handle_service.php and killall keep working
define GO_LIBRESPOT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/go-librespot $(TARGET_DIR)/usr/lib/go-librespot/librespot
endef

$(eval $(generic-package))
