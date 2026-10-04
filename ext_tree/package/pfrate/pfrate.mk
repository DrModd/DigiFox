################################################################################
#
# pfrate
#
################################################################################

PFRATE_VERSION = 1.0
PFRATE_SITE_METHOD = local
PFRATE_SITE = $(BR2_EXTERNAL_ext_tree_PATH)/package/pfrate
PFRATE_LICENSE = GPL-2.0+

define PFRATE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -O2 -s \
		-o $(@D)/pfrate $(@D)/pfrate.c
endef

define PFRATE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pfrate $(TARGET_DIR)/usr/bin/pfrate
endef

$(eval $(generic-package))
