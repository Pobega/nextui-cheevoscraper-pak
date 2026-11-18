PAK_NAME ?= RA Cache
PLATFORMS ?= tg5040 h700

RCHEEVOS_REPO ?= https://github.com/RetroAchievements/rcheevos.git
RCHEEVOS_COMMIT ?= 40d916de00fe757bab40fb4db41a7912193a48e3

CONTAINER_RUNTIME ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null || (command -v flatpak-spawn >/dev/null 2>&1 && echo "flatpak-spawn --host podman"))
ifneq (,$(findstring podman,$(CONTAINER_RUNTIME)))
CONTAINER_MOUNT_OPTS ?= --security-opt label=disable
else
CONTAINER_MOUNT_OPTS ?=
endif

BUILD_DIR := $(CURDIR)/build
RCHEEVOS_DIR := $(BUILD_DIR)/rcheevos
DIST_DIR := $(CURDIR)/dist

RCHEEVOS_SRCDIR := $(RCHEEVOS_DIR)/src
RCHEEVOS_INCDIR := $(RCHEEVOS_DIR)/include
RCHEEVOS_SOURCES := \
	$(RCHEEVOS_SRCDIR)/rc_compat.c \
	$(RCHEEVOS_SRCDIR)/rc_util.c \
	$(RCHEEVOS_SRCDIR)/rc_version.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/alloc.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/condition.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/condset.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/consoleinfo.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/format.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/lboard.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/memref.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/operand.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/richpresence.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/runtime.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/runtime_progress.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/trigger.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/value.c \
	$(RCHEEVOS_SRCDIR)/rcheevos/rc_validate.c \
	$(RCHEEVOS_SRCDIR)/rapi/rc_api_common.c \
	$(RCHEEVOS_SRCDIR)/rapi/rc_api_editor.c \
	$(RCHEEVOS_SRCDIR)/rapi/rc_api_info.c \
	$(RCHEEVOS_SRCDIR)/rapi/rc_api_runtime.c \
	$(RCHEEVOS_SRCDIR)/rapi/rc_api_user.c \
	$(RCHEEVOS_SRCDIR)/rhash/aes.c \
	$(RCHEEVOS_SRCDIR)/rhash/cdreader.c \
	$(RCHEEVOS_SRCDIR)/rhash/hash.c \
	$(RCHEEVOS_SRCDIR)/rhash/hash_disc.c \
	$(RCHEEVOS_SRCDIR)/rhash/hash_encrypted.c \
	$(RCHEEVOS_SRCDIR)/rhash/hash_rom.c \
	$(RCHEEVOS_SRCDIR)/rhash/hash_zip.c \
	$(RCHEEVOS_SRCDIR)/rhash/md5.c

APP_SOURCES := \
	$(CURDIR)/src/main.c \
	$(CURDIR)/src/cache_writer.c \
	$(CURDIR)/src/http_client.c \
	$(CURDIR)/src/rom_hasher.c

TARGET_PLATFORM ?= host
TARGET_CC ?= gcc
TARGET_AR ?= ar
TARGET_STRIP ?= strip
TARGET_OPT ?= -O2

PLAT_BUILD := $(BUILD_DIR)/$(TARGET_PLATFORM)
RCHEEVOS_OBJS := $(patsubst $(RCHEEVOS_SRCDIR)/%.c,$(PLAT_BUILD)/rcheevos-obj/%.o,$(RCHEEVOS_SOURCES))
RCHEEVOS_LIB := $(PLAT_BUILD)/librcheevos.a
APP_OBJS := $(patsubst $(CURDIR)/src/%.c,$(PLAT_BUILD)/app-obj/%.o,$(APP_SOURCES))
APP_BIN := $(BUILD_DIR)/bin/$(TARGET_PLATFORM)/ra-cache

RCHEEVOS_CFLAGS := $(TARGET_OPT) -std=gnu99 -I$(RCHEEVOS_INCDIR) -I$(RCHEEVOS_SRCDIR) -DRC_DISABLE_LUA
APP_CFLAGS := $(TARGET_OPT) -std=gnu99 -Wall -Wextra -Werror -I$(RCHEEVOS_INCDIR) -I$(CURDIR)/src
APP_LDFLAGS := -lcrypto -lzip -lpthread -lm

.PHONY: all rcheevos host internal-build build package clean distclean $(foreach p,$(PLATFORMS),build-$(p))

all: build package

rcheevos: $(RCHEEVOS_DIR)/.checked_out

$(RCHEEVOS_DIR)/.checked_out:
	@mkdir -p $(BUILD_DIR)
	@if [ ! -d "$(RCHEEVOS_DIR)/.git" ]; then \
		echo "Cloning $(RCHEEVOS_REPO)..."; \
		git clone $(RCHEEVOS_REPO) $(RCHEEVOS_DIR); \
	fi
	@git -C $(RCHEEVOS_DIR) checkout -q $(RCHEEVOS_COMMIT)
	@touch $@

$(PLAT_BUILD)/rcheevos-obj/%.o: $(RCHEEVOS_SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(TARGET_CC) $(RCHEEVOS_CFLAGS) -c -o $@ $<

$(RCHEEVOS_LIB): $(RCHEEVOS_OBJS)
	@mkdir -p $(dir $@)
	$(TARGET_AR) rcs $@ $^

$(PLAT_BUILD)/app-obj/%.o: $(CURDIR)/src/%.c
	@mkdir -p $(dir $@)
	$(TARGET_CC) $(APP_CFLAGS) -c -o $@ $<

$(APP_BIN): $(APP_OBJS) $(RCHEEVOS_LIB)
	@mkdir -p $(dir $@)
	$(TARGET_CC) -o $@ $(APP_OBJS) $(RCHEEVOS_LIB) $(APP_LDFLAGS)

internal-build: $(APP_BIN)

host: rcheevos
	$(MAKE) internal-build TARGET_PLATFORM=host TARGET_CC=gcc TARGET_AR=ar

build: $(foreach p,$(PLATFORMS),build-$(p))

define BUILD_PLATFORM_RULE
build-$(1): rcheevos
	@if [ -z "$$(CONTAINER_RUNTIME)" ]; then \
		echo "Error: neither podman nor docker found in PATH"; exit 1; \
	fi
	@echo "Building ra-cache for $(1)..."
	$$(CONTAINER_RUNTIME) run --rm $$(CONTAINER_MOUNT_OPTS) \
		-v $$(CURDIR):/workspace:rw \
		ghcr.io/loveretro/$(1)-toolchain:latest \
		bash -euc '\
			cd /workspace && \
			make internal-build \
				TARGET_PLATFORM=$(1) \
				TARGET_CC=/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-gcc \
				TARGET_AR=/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-ar \
				TARGET_OPT="-O3 -mcpu=cortex-a53 -mtune=cortex-a53" && \
			/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-strip --strip-unneeded /workspace/build/bin/$(1)/ra-cache && \
			chmod 0755 /workspace/build/bin/$(1)/ra-cache \
		'
endef

$(foreach p,$(PLATFORMS),$(eval $(call BUILD_PLATFORM_RULE,$(p))))

package:
	@echo "Packaging $(PAK_NAME).pak..."
	@rm -rf $(BUILD_DIR)/staging $(DIST_DIR)
	@mkdir -p $(BUILD_DIR)/staging/bin $(DIST_DIR)
	@cp pak.json launch.sh README.md $(BUILD_DIR)/staging/
	@chmod 0755 $(BUILD_DIR)/staging/launch.sh
	@for p in $(PLATFORMS); do \
		if [ ! -x "$(BUILD_DIR)/bin/$$p/ra-cache" ]; then \
			echo "Error: missing binary for $$p; run 'make build' first"; exit 1; \
		fi; \
		mkdir -p "$(BUILD_DIR)/staging/bin/$$p"; \
		cp "$(BUILD_DIR)/bin/$$p/ra-cache" "$(BUILD_DIR)/staging/bin/$$p/ra-cache"; \
		mkdir -p "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/bin/$$p"; \
		cp pak.json launch.sh README.md "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/"; \
		chmod 0755 "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/launch.sh"; \
		cp "$(BUILD_DIR)/bin/$$p/ra-cache" "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/bin/$$p/ra-cache"; \
	done
	@cd $(BUILD_DIR)/staging && zip -r "$(DIST_DIR)/$(PAK_NAME).pak.zip" .
	@echo "Created $(DIST_DIR)/$(PAK_NAME).pak.zip"

clean:
	rm -rf $(BUILD_DIR)/host $(BUILD_DIR)/tg5040 $(BUILD_DIR)/h700 $(BUILD_DIR)/bin $(BUILD_DIR)/staging $(DIST_DIR)

distclean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
