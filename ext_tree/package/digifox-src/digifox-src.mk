################################################################################
#
# digifox-src
#
################################################################################

DIGIFOX_SRC_VERSION = 1.0
DIGIFOX_SRC_SITE_METHOD = local
DIGIFOX_SRC_SITE = $(BR2_EXTERNAL_ext_tree_PATH)/package/digifox-src
DIGIFOX_SRC_LICENSE = GPL-2.0+
DIGIFOX_SRC_DEPENDENCIES = libsoxr alsa-lib

DIGIFOX_SRC_CFLAGS = $(TARGET_CFLAGS) -O3 -ffast-math -mfpu=neon-vfpv4 -Wall

define DIGIFOX_SRC_BUILD_CMDS
	$(TARGET_CC) $(DIGIFOX_SRC_CFLAGS) $(TARGET_LDFLAGS) -s \
		-o $(@D)/digifox-srcbench $(@D)/srcbench.c $(@D)/dsd2pcm.c -lsoxr -lm
	$(TARGET_CC) $(DIGIFOX_SRC_CFLAGS) $(TARGET_LDFLAGS) -fPIC -shared -s \
		-DPIC -o $(@D)/libasound_module_pcm_digifox.so \
		$(@D)/pcm_digifox.c $(@D)/dsd2pcm.c -lasound -lsoxr -lm -lpthread
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -fPIC -shared -s \
		-o $(@D)/raat_card.so $(@D)/raat_card.c -ldl
endef

define DIGIFOX_SRC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/digifox-srcbench $(TARGET_DIR)/usr/bin/digifox-srcbench
	$(INSTALL) -D -m 0755 $(@D)/libasound_module_pcm_digifox.so \
		$(TARGET_DIR)/usr/lib/alsa-lib/libasound_module_pcm_digifox.so
	$(INSTALL) -D -m 0644 $(@D)/raat_card.so $(TARGET_DIR)/usr/lib/digifox/raat_card.so
endef

$(eval $(generic-package))
