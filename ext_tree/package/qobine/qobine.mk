################################################################################
#
# qobine — open source Qobuz Connect player (only the connect-module binary)
#
################################################################################

QOBINE_VERSION = v2026-11-03-1
QOBINE_SITE = https://github.com/SofusA/qobine.git
QOBINE_SITE_METHOD = git
QOBINE_LICENSE = GPL-3.0
QOBINE_LICENSE_FILES = LICENSE
QOBINE_DEPENDENCIES = alsa-lib openssl host-pkgconf host-cmake

# sqlx query macros: use the checked-in .sqlx data, no database at build time
QOBINE_CARGO_ENV = SQLX_OFFLINE=true
QOBINE_CARGO_BUILD_OPTS = -p connect-module --bin qobine-connect

define QOBINE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$$(ls -d $(@D)/target/*/release/qobine-connect $(@D)/target/release/qobine-connect 2>/dev/null | head -1) \
		$(TARGET_DIR)/usr/lib/qobine/qobuz-connect
endef

$(eval $(cargo-package))
