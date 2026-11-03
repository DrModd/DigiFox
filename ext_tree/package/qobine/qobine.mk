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
# Build scripts and proc-macros run on the build machine: sqlx's macros pull ring and
# libsqlite3-sys (C code) for the host too. The cc crate picks CC_<triple> first —
# without these it used the ARM cross compiler (and its -mcpu flags) for x86_64.
QOBINE_CARGO_ENV += \
	CC_x86_64_unknown_linux_gnu="$(HOSTCC)" \
	CXX_x86_64_unknown_linux_gnu="$(HOSTCXX)" \
	AR_x86_64_unknown_linux_gnu="$(HOSTAR)" \
	CFLAGS_x86_64_unknown_linux_gnu="-O2" \
	CXXFLAGS_x86_64_unknown_linux_gnu="-O2"
QOBINE_CARGO_BUILD_OPTS = -p connect-module --bin qobine-connect

# stream-download takes its crate docs from ../README.md (include_str!), which is not
# in its published crate: an empty one lets it compile (the docs are not used)
define QOBINE_VENDOR_README
	for c in $(@D)/VENDOR/stream-download*; do \
		[ -f $$c/README.md ] || : > $$c/README.md; \
	done
endef
QOBINE_POST_PATCH_HOOKS += QOBINE_VENDOR_README

define QOBINE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$$(ls -d $(@D)/target/*/release/qobine-connect $(@D)/target/release/qobine-connect 2>/dev/null | head -1) \
		$(TARGET_DIR)/usr/lib/qobine/qobuz-connect
endef

$(eval $(cargo-package))
