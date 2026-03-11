CC = gcc
PROFILE ?= full

COMMON_CFLAGS = -Wall -Wextra -std=c99 -Iinclude -O2 -ffunction-sections -fdata-sections
LDFLAGS = -lffi -ldl -lm -Wl,--gc-sections

PROFILE_CAP_OS_full = 1
PROFILE_CAP_FS_full = 1
PROFILE_CAP_WEB_full = 1
PROFILE_CAP_DB_full = 1
PROFILE_CAP_AI_full = 1
PROFILE_CAP_CACHE_full = 1
PROFILE_CAP_UTIL_full = 1
PROFILE_CAP_META_full = 1

PROFILE_CAP_OS_game = 1
PROFILE_CAP_FS_game = 1
PROFILE_CAP_WEB_game = 0
PROFILE_CAP_DB_game = 0
PROFILE_CAP_AI_game = 0
PROFILE_CAP_CACHE_game = 1
PROFILE_CAP_UTIL_game = 1
PROFILE_CAP_META_game = 1

PROFILE_CAP_OS_embedded = 1
PROFILE_CAP_FS_embedded = 0
PROFILE_CAP_WEB_embedded = 0
PROFILE_CAP_DB_embedded = 0
PROFILE_CAP_AI_embedded = 0
PROFILE_CAP_CACHE_embedded = 0
PROFILE_CAP_UTIL_embedded = 1
PROFILE_CAP_META_embedded = 1

PROFILE_CAP_OS = $(PROFILE_CAP_OS_$(PROFILE))
PROFILE_CAP_FS = $(PROFILE_CAP_FS_$(PROFILE))
PROFILE_CAP_WEB = $(PROFILE_CAP_WEB_$(PROFILE))
PROFILE_CAP_DB = $(PROFILE_CAP_DB_$(PROFILE))
PROFILE_CAP_AI = $(PROFILE_CAP_AI_$(PROFILE))
PROFILE_CAP_CACHE = $(PROFILE_CAP_CACHE_$(PROFILE))
PROFILE_CAP_UTIL = $(PROFILE_CAP_UTIL_$(PROFILE))
PROFILE_CAP_META = $(PROFILE_CAP_META_$(PROFILE))

ifeq ($(strip $(PROFILE_CAP_OS)),)
$(error Unknown PROFILE='$(PROFILE)'. Use one of: full, game, embedded)
endif

CAP_OS_FINAL = $(if $(CAP_OS),$(CAP_OS),$(PROFILE_CAP_OS))
CAP_FS_FINAL = $(if $(CAP_FS),$(CAP_FS),$(PROFILE_CAP_FS))
CAP_WEB_FINAL = $(if $(CAP_WEB),$(CAP_WEB),$(PROFILE_CAP_WEB))
CAP_DB_FINAL = $(if $(CAP_DB),$(CAP_DB),$(PROFILE_CAP_DB))
CAP_AI_FINAL = $(if $(CAP_AI),$(CAP_AI),$(PROFILE_CAP_AI))
CAP_CACHE_FINAL = $(if $(CAP_CACHE),$(CAP_CACHE),$(PROFILE_CAP_CACHE))
CAP_UTIL_FINAL = $(if $(CAP_UTIL),$(CAP_UTIL),$(PROFILE_CAP_UTIL))
CAP_META_FINAL = $(if $(CAP_META),$(CAP_META),$(PROFILE_CAP_META))
PROFILE_NAME_FINAL = $(if $(PROFILE_NAME),$(PROFILE_NAME),$(PROFILE))

PROFILE_WARN_FLAGS =
ifneq ($(CAP_OS_FINAL)$(CAP_FS_FINAL)$(CAP_WEB_FINAL)$(CAP_DB_FINAL)$(CAP_AI_FINAL)$(CAP_CACHE_FINAL)$(CAP_UTIL_FINAL)$(CAP_META_FINAL),11111111)
PROFILE_WARN_FLAGS += -Wno-unused-function
endif

CFLAGS = $(COMMON_CFLAGS) \
	-DVIPER_PROFILE_NAME=\"$(PROFILE_NAME_FINAL)\" \
	-DVIPER_CAP_OS=$(CAP_OS_FINAL) \
	-DVIPER_CAP_FS=$(CAP_FS_FINAL) \
	-DVIPER_CAP_WEB=$(CAP_WEB_FINAL) \
	-DVIPER_CAP_DB=$(CAP_DB_FINAL) \
	-DVIPER_CAP_AI=$(CAP_AI_FINAL) \
	-DVIPER_CAP_CACHE=$(CAP_CACHE_FINAL) \
	-DVIPER_CAP_UTIL=$(CAP_UTIL_FINAL) \
	-DVIPER_CAP_META=$(CAP_META_FINAL) \
	$(PROFILE_WARN_FLAGS)

SRCS = $(wildcard src/*.c)
CAP_TAG = os$(CAP_OS_FINAL)_fs$(CAP_FS_FINAL)_web$(CAP_WEB_FINAL)_db$(CAP_DB_FINAL)_ai$(CAP_AI_FINAL)_cache$(CAP_CACHE_FINAL)_util$(CAP_UTIL_FINAL)_meta$(CAP_META_FINAL)_name$(PROFILE_NAME_FINAL)
OBJDIR = .build/$(PROFILE)-$(CAP_TAG)
OBJS = $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRCS))
TARGET = viper
TIMEOUT ?= 10

EXT_NET = lib/std/net.so
EXT_MATH = lib/std/math.so

.PHONY: all runtime clean test show-profile

all: $(TARGET) $(EXT_NET) $(EXT_MATH)
runtime: $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(EXT_NET): lib/std/net_ext.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

$(EXT_MATH): lib/std/math_ext.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -lm

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

show-profile:
	@echo "PROFILE=$(PROFILE)"
	@echo "CFLAGS=$(CFLAGS)"

test: $(TARGET)
	@./tests/run_tests.sh ./$(TARGET) tests/scripts $(TIMEOUT)

clean:
	rm -rf .build
	rm -f $(TARGET) $(EXT_NET) $(EXT_MATH)
