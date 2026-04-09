################################################################################
#
# k230claw - AI assistant for K230 Linux
#
################################################################################

K230CLAW_SITE = $(realpath $(TOPDIR))/package/k230claw/src
K230CLAW_SITE_METHOD = local
K230CLAW_APP_NAME := k230claw

# Phase 1-3 依赖
K230CLAW_DEPENDENCIES = libcurl openssl

# Phase 4 硬件依赖（K230 目标板）
K230CLAW_DEPENDENCIES += alsa-lib jpeg vvcam libdrm

# nncase 和相关库（如果在 Buildroot 中可用）
# K230CLAW_DEPENDENCIES += libnncase

# libcurl 和 openssl 的头文件/库通过 STAGING_DIR 获取（Buildroot 自动安装）
K230CLAW_CFLAGS = $(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include -DKC_HAS_K230_HW -I$(STAGING_DIR)/usr/include/libdrm
K230CLAW_CXXFLAGS = $(K230CLAW_CFLAGS)
K230CLAW_LDFLAGS = -L$(STAGING_DIR)/usr/lib \
	-lcurl -lssl -lcrypto -lpthread \
	-lasound -ljpeg \
	-lNncase.Runtime.Native -lnncase.rt_modules.k230 -lfunctional_k230 \
	-lv4l2-drm -lmmz -ldrm \
	-lstdc++ -lm

# 编译：调用 src/Makefile，传入交叉编译器和依赖路径
define K230CLAW_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" -C $(@D) \
		EXTRA_CFLAGS="$(K230CLAW_CFLAGS)" \
		EXTRA_LDFLAGS="$(K230CLAW_LDFLAGS)"
endef

# 安装到 rootfs 的 /usr/bin/
define K230CLAW_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/$(K230CLAW_APP_NAME) $(TARGET_DIR)/usr/bin/$(K230CLAW_APP_NAME)
endef

$(eval $(generic-package))
