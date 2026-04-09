# K230Claw 源码编译 Makefile
#
# Buildroot 通过 k230claw.mk 传入:
#   CC           - 交叉编译器 (riscv64-unknown-linux-gnu-gcc)
#   CXX          - C++ 交叉编译器
#   EXTRA_CFLAGS - 包含 staging 头文件路径 + -DKC_HAS_K230_HW
#   EXTRA_LDFLAGS - 链接 libcurl/openssl/pthread/alsa/jpeg/nncase 等

CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
CC ?= $(CROSS_COMPILE)gcc
CXX ?= $(CROSS_COMPILE)g++

TARGET = k230claw

# C 源文件列表
SRCS = main.c \
       hal/kc_config.c \
       bus/message_bus.c \
       channels/channel_mgr.c \
       sensors/sensor_mgr.c \
       sensors/sensor_state.c \
       cron/cron_service.c \
       agent/agent_loop.c \
       agent/context_builder.c \
       llm/llm_proxy.c \
       memory/memory_store.c \
       memory/session_mgr.c \
       gateway/ws_server.c \
       cli/serial_cli.c \
       tools/tool_registry.c \
       tools/tool_hw.c \
       tools/tool_get_time.c \
       tools/tool_files.c \
       tools/tool_shell.c \
       tools/tool_cron.c \
       tools/tool_web.c \
       skills/skill_loader.c \
       roles/role_mgr.c \
       third_party/cJSON/cJSON.c

# 硬件 C 源文件（仅 K230 目标板）
ifneq ($(findstring KC_HAS_K230_HW,$(EXTRA_CFLAGS)),)
SRCS += hal/hal_audio.c \
        hal/hal_camera.c \
        hal/camera_utils.c \
        sensors/sensor_stream.c \
        tools/tool_camera.c \
        tools/tool_audio.c \
        tools/tool_face.c \
        sensors/sensor_face.c \
        sensors/anchors_320.c \
        sensors/sensor_voice.c
endif

# mongoose: 自动检测 third_party/mongoose/mongoose.c 是否存在
MONGOOSE_SRC = $(wildcard third_party/mongoose/mongoose.c)
ifneq ($(MONGOOSE_SRC),)
SRCS += third_party/mongoose/mongoose.c
MONGOOSE_CFLAGS = -DKC_HAS_MONGOOSE -DMG_ENABLE_LINES=0 -DMG_ENABLE_LOG=0
else
MONGOOSE_CFLAGS =
endif

OBJS = $(SRCS:.c=.o)

# C++ 源文件（仅 K230 目标板，需要 nncase）
CXX_SRCS =
ifneq ($(findstring KC_HAS_K230_HW,$(EXTRA_CFLAGS)),)
CXX_SRCS += hal/yolo_detect.cpp \
            hal/hal_camera_stream.cpp \
            hal/kpu_wrapper.cpp \
            hal/kws_features.cpp \
            hal/fft.cc \
            hal/tts/tts_engine.cpp \
            hal/tts/zh_frontend.cpp \
            hal/tts/pypinyin.cpp \
            hal/tts/pinyin_utils.cpp \
            hal/tts/_utils.cpp \
            hal/tts/tone_sanhi.cpp \
            hal/tts/tone.cpp \
            hal/tts/finals.cpp \
            hal/tts/text_normalization.cpp \
            hal/tts/char_convert.cpp \
            hal/tts/num.cpp \
            hal/tts/chronology.cpp \
            hal/tts/phonecode.cpp \
            hal/tts/quantifier.cpp \
            hal/tts/standard.cpp \
            hal/tts/jieba_utils.cpp \
            hal/tts/VoxCommon.cpp \
            hal/tts/utils_tts.cpp \
            hal/tts/length_regulator.cpp
endif

CXX_OBJS = $(patsubst %.cpp,%.o,$(patsubst %.cc,%.o,$(CXX_SRCS)))

# 编译选项
CFLAGS = -Wall -O2 -D_GNU_SOURCE -I. -I./third_party $(MONGOOSE_CFLAGS) $(EXTRA_CFLAGS)
CXXFLAGS = -Wall -O2 -D_GNU_SOURCE -I. -I./third_party $(MONGOOSE_CFLAGS) $(EXTRA_CFLAGS)
ifneq ($(findstring KC_HAS_K230_HW,$(EXTRA_CFLAGS)),)
CXXFLAGS += -I./hal/tts/include
endif
LDFLAGS = $(EXTRA_LDFLAGS)

all: $(TARGET)
	@echo "K230Claw build done."

# 链接：有 C++ 时用 g++
ifneq ($(CXX_OBJS),)
$(TARGET): $(OBJS) $(CXX_OBJS)
	$(CXX) $(OBJS) $(CXX_OBJS) -o $(TARGET) $(LDFLAGS)
else
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)
endif

%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -f $(OBJS) $(CXX_OBJS) $(TARGET)
	@echo "Clean done."

.PHONY: all clean
