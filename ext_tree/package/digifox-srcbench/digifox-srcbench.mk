################################################################################
#
# digifox-srcbench
#
################################################################################

DIGIFOX_SRCBENCH_VERSION = 1.0
DIGIFOX_SRCBENCH_SITE_METHOD = local
DIGIFOX_SRCBENCH_SITE = $(BR2_EXTERNAL_ext_tree_PATH)/package/digifox-srcbench
DIGIFOX_SRCBENCH_LICENSE = GPL-2.0+
DIGIFOX_SRCBENCH_DEPENDENCIES = libsoxr

define DIGIFOX_SRCBENCH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -O3 -ffast-math -mfpu=neon-vfpv4 -Wall -s \
		-o $(@D)/digifox-srcbench $(@D)/srcbench.c $(@D)/dsd2pcm.c -lsoxr -lm
endef

define DIGIFOX_SRCBENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/digifox-srcbench $(TARGET_DIR)/usr/bin/digifox-srcbench
endef

$(eval $(generic-package))
