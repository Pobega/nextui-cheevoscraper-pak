PAK_NAME ?= CheevoScraper
PLATFORMS ?= tg5040 h700

PUSH_PLATFORM ?= tg5040
PUSH_SDCARD_PATH ?= /mnt/SDCARD

RCHEEVOS_REPO ?= https://github.com/RetroAchievements/rcheevos.git
RCHEEVOS_COMMIT ?= 40d916de00fe757bab40fb4db41a7912193a48e3

LIBCHDR_REPO ?= https://github.com/rtissera/libchdr.git
LIBCHDR_COMMIT ?= 47d794151b2b64f6aba60304e6480f4f5a2bc24b

CONTAINER_RUNTIME ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null || (command -v flatpak-spawn >/dev/null 2>&1 && echo "flatpak-spawn --host podman"))
ifneq (,$(findstring podman,$(CONTAINER_RUNTIME)))
CONTAINER_MOUNT_OPTS ?= --security-opt label=disable
else
CONTAINER_MOUNT_OPTS ?=
endif

BUILD_DIR := $(CURDIR)/build
RCHEEVOS_DIR := $(BUILD_DIR)/rcheevos
LIBCHDR_DIR := $(BUILD_DIR)/libchdr
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

GO_VERSION ?= 1.24.3
GO_DIR := $(BUILD_DIR)/go
GO_BIN := $(GO_DIR)/bin/go
GO_CACHE := $(BUILD_DIR)/gocache
GO_MODCACHE := $(BUILD_DIR)/gomodcache

SDL2_GFX_VERSION ?= 1.0.4
SDL2_GFX_DIR := $(BUILD_DIR)/sdl2_gfx

APP_SOURCES := \
	$(CURDIR)/src/main.c \
	$(CURDIR)/src/cache_writer.c \
	$(CURDIR)/src/chd_reader.c \
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
LIBCHDR_BUILD := $(PLAT_BUILD)/libchdr-build
LIBCHDR_PREFIX := $(PLAT_BUILD)/libchdr-install
LIBCHDR_LIB := $(LIBCHDR_PREFIX)/lib/libchdr.a
APP_OBJS := $(patsubst $(CURDIR)/src/%.c,$(PLAT_BUILD)/app-obj/%.o,$(APP_SOURCES))
APP_BIN := $(BUILD_DIR)/bin/$(TARGET_PLATFORM)/ra-cache

RCHEEVOS_CFLAGS := $(TARGET_OPT) -std=gnu99 -I$(RCHEEVOS_INCDIR) -I$(RCHEEVOS_SRCDIR) -DRC_DISABLE_LUA
APP_CFLAGS := $(TARGET_OPT) -std=gnu99 -Wall -Wextra -Werror -I$(RCHEEVOS_INCDIR) -I$(LIBCHDR_DIR)/include -I$(CURDIR)/src
APP_LDFLAGS := -L$(LIBCHDR_PREFIX)/lib -lcrypto -lzip -lchdr -lchdr-lzma -lzstd -lz -llz4 -lpthread -lm

LIBCHDR_CMAKE_ARGS := \
	-DBUILD_SHARED_LIBS=OFF \
	-DINSTALL_STATIC_LIBS=ON \
	-DCHDR_WANT_TESTS=OFF \
	-DWITH_SYSTEM_ZLIB=ON \
	-DWITH_SYSTEM_ZSTD=OFF \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_INSTALL_PREFIX=$(LIBCHDR_PREFIX) \
	-DCMAKE_C_FLAGS="$(TARGET_OPT)"
ifdef CMAKE_TOOLCHAIN_FILE
LIBCHDR_CMAKE_ARGS += -DCMAKE_TOOLCHAIN_FILE=$(CMAKE_TOOLCHAIN_FILE)
endif

.PHONY: all rcheevos libchdr host internal-build test build package push deploy clean distclean $(foreach p,$(PLATFORMS),build-$(p))

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

libchdr: $(LIBCHDR_DIR)/.checked_out

$(LIBCHDR_DIR)/.checked_out:
	@mkdir -p $(BUILD_DIR)
	@if [ ! -d "$(LIBCHDR_DIR)/.git" ]; then \
		echo "Cloning $(LIBCHDR_REPO)..."; \
		git clone $(LIBCHDR_REPO) $(LIBCHDR_DIR); \
	fi
	@git -C $(LIBCHDR_DIR) checkout -q $(LIBCHDR_COMMIT)
	@touch $@

$(LIBCHDR_LIB): $(LIBCHDR_DIR)/.checked_out
	@mkdir -p $(LIBCHDR_BUILD)
	cmake -S $(LIBCHDR_DIR) -B $(LIBCHDR_BUILD) $(LIBCHDR_CMAKE_ARGS)
	cmake --build $(LIBCHDR_BUILD) -j$$(nproc)
	cmake --install $(LIBCHDR_BUILD)
	ln -sf libchdr-static.a $@

$(PLAT_BUILD)/rcheevos-obj/%.o: $(RCHEEVOS_SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(TARGET_CC) $(RCHEEVOS_CFLAGS) -c -o $@ $<

$(RCHEEVOS_LIB): $(RCHEEVOS_OBJS)
	@mkdir -p $(dir $@)
	$(TARGET_AR) rcs $@ $^

$(PLAT_BUILD)/app-obj/%.o: $(CURDIR)/src/%.c
	@mkdir -p $(dir $@)
	$(TARGET_CC) $(APP_CFLAGS) -c -o $@ $<

$(APP_BIN): $(APP_OBJS) $(RCHEEVOS_LIB) $(LIBCHDR_LIB)
	@mkdir -p $(dir $@)
	$(TARGET_CC) -o $@ $(APP_OBJS) $(RCHEEVOS_LIB) $(APP_LDFLAGS)

internal-build: $(APP_BIN)

host: rcheevos libchdr
	$(MAKE) internal-build TARGET_PLATFORM=host TARGET_CC=gcc TARGET_AR=ar

TEST_BIN := $(BUILD_DIR)/host/test_unit

test: rcheevos
	$(MAKE) $(BUILD_DIR)/host/librcheevos.a TARGET_PLATFORM=host TARGET_CC=gcc TARGET_AR=ar
	@mkdir -p $(dir $(TEST_BIN))
	gcc -O2 -std=gnu99 -Wall -Wextra -Werror -DCACHE_WRITER_NO_CRYPTO \
		-I$(RCHEEVOS_INCDIR) -I$(CURDIR)/src \
		$(CURDIR)/tests/test_unit.c $(CURDIR)/src/cache_writer.c \
		$(BUILD_DIR)/host/librcheevos.a -lm -lpthread -o $(TEST_BIN)
	$(TEST_BIN)

go-toolchain: $(GO_BIN)

$(GO_BIN):
	@mkdir -p $(BUILD_DIR)
	@echo "Downloading Go $(GO_VERSION)..."
	@curl -f -sSL -o $(BUILD_DIR)/go.tar.gz "https://go.dev/dl/go$(GO_VERSION).linux-amd64.tar.gz"
	@rm -rf $(GO_DIR)
	@tar -C $(BUILD_DIR) -xzf $(BUILD_DIR)/go.tar.gz
	@rm -f $(BUILD_DIR)/go.tar.gz

sdl2-gfx: $(SDL2_GFX_DIR)/.extracted

$(SDL2_GFX_DIR)/.extracted:
	@mkdir -p $(SDL2_GFX_DIR)
	@echo "Downloading SDL2_gfx $(SDL2_GFX_VERSION)..."
	@curl -f -sSL -o $(BUILD_DIR)/sdl2_gfx.tar.gz "https://www.ferzkopp.net/Software/SDL2_gfx/SDL2_gfx-$(SDL2_GFX_VERSION).tar.gz"
	@tar -C $(SDL2_GFX_DIR) --strip-components=1 -xzf $(BUILD_DIR)/sdl2_gfx.tar.gz
	@rm -f $(BUILD_DIR)/sdl2_gfx.tar.gz
	@touch $@

build: $(foreach p,$(PLATFORMS),build-$(p))

define BUILD_PLATFORM_RULE
build-$(1): rcheevos libchdr go-toolchain sdl2-gfx
	@if [ -z "$$(CONTAINER_RUNTIME)" ]; then \
		echo "Error: neither podman nor docker found in PATH"; exit 1; \
	fi
	@echo "Building ra-cache and cheevoscraper-ui for $(1)..."
	@mkdir -p $(GO_CACHE) $(GO_MODCACHE) $(BUILD_DIR)/bin/$(1) $(BUILD_DIR)/$(1)/sdl2_gfx $(BUILD_DIR)/$(1)/pkgconfig
	@printf 'Name: SDL2_ttf\nDescription: SDL2_ttf\nVersion: 2.0.15\nLibs: -lSDL2_ttf\nCflags: -I/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/include/SDL2\n' > $(BUILD_DIR)/$(1)/pkgconfig/SDL2_ttf.pc
	@printf 'Name: SDL2_image\nDescription: SDL2_image\nVersion: 2.0.5\nLibs: -lSDL2_image\nCflags: -I/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/include/SDL2\n' > $(BUILD_DIR)/$(1)/pkgconfig/SDL2_image.pc
	@printf 'Name: SDL2_gfx\nDescription: SDL2_gfx\nVersion: $(SDL2_GFX_VERSION)\nLibs: -L/workspace/build/$(1)/sdl2_gfx -lSDL2_gfx\nCflags: -I/workspace/build/sdl2_gfx\n' > $(BUILD_DIR)/$(1)/pkgconfig/SDL2_gfx.pc
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
			chmod 0755 /workspace/build/bin/$(1)/ra-cache && \
			if [ ! -f /workspace/build/$(1)/sdl2_gfx/libSDL2_gfx.a ]; then \
				for src in SDL2_framerate.c SDL2_gfxPrimitives.c SDL2_imageFilter.c SDL2_rotozoom.c; do \
					/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-gcc -O3 -mcpu=cortex-a53 -mtune=cortex-a53 -I/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/include/SDL2 -I/workspace/build/sdl2_gfx -c -o /workspace/build/$(1)/sdl2_gfx/$$$$src.o /workspace/build/sdl2_gfx/$$$$src; \
				done; \
				/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-ar rcs /workspace/build/$(1)/sdl2_gfx/libSDL2_gfx.a /workspace/build/$(1)/sdl2_gfx/*.o; \
			fi && \
			cd /workspace/ui && \
			export PATH="/workspace/build/go/bin:$$$$PATH" && \
			export GOCACHE=/workspace/build/gocache && \
			export GOMODCACHE=/workspace/build/gomodcache && \
			export PKG_CONFIG_PATH="/workspace/build/$(1)/pkgconfig:/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/lib/pkgconfig" && \
			/workspace/build/go/bin/go get github.com/BrandonKowalski/gabagool/v2@main && \
			/workspace/build/go/bin/go mod tidy && \
			CGO_ENABLED=1 GOOS=linux GOARCH=arm64 \
			CC=/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-gcc \
			CGO_CFLAGS="-I/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/include/SDL2 -I/workspace/build/sdl2_gfx" \
			CGO_LDFLAGS="-L/workspace/build/$(1)/sdl2_gfx -lSDL2_gfx -lm" \
			/workspace/build/go/bin/go build -trimpath -ldflags="-s -w" -o /workspace/build/bin/$(1)/cheevoscraper-ui . && \
			chmod 0755 /workspace/build/bin/$(1)/cheevoscraper-ui \
		'
endef

$(foreach p,$(PLATFORMS),$(eval $(call BUILD_PLATFORM_RULE,$(p))))

package:
	@echo "Packaging $(PAK_NAME).pak..."
	@rm -rf $(BUILD_DIR)/staging $(DIST_DIR)
	@mkdir -p $(BUILD_DIR)/staging/bin $(DIST_DIR)
	@cp pak.json launch.sh README.md LICENSE $(BUILD_DIR)/staging/
	@chmod 0755 $(BUILD_DIR)/staging/launch.sh
	@for p in $(PLATFORMS); do \
		for b in ra-cache cheevoscraper-ui; do \
			if [ ! -x "$(BUILD_DIR)/bin/$$p/$$b" ]; then \
				echo "Error: missing binary $$b for $$p; run 'make build' first"; exit 1; \
			fi; \
		done; \
		mkdir -p "$(BUILD_DIR)/staging/bin/$$p"; \
		cp "$(BUILD_DIR)/bin/$$p/ra-cache" "$(BUILD_DIR)/bin/$$p/cheevoscraper-ui" "$(BUILD_DIR)/staging/bin/$$p/"; \
		mkdir -p "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/bin/$$p"; \
		cp pak.json launch.sh README.md LICENSE "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/"; \
		chmod 0755 "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/launch.sh"; \
		cp "$(BUILD_DIR)/bin/$$p/ra-cache" "$(BUILD_DIR)/bin/$$p/cheevoscraper-ui" "$(DIST_DIR)/Tools/$$p/$(PAK_NAME).pak/bin/$$p/"; \
	done
	@cd $(BUILD_DIR)/staging && zip -r "$(DIST_DIR)/$(PAK_NAME).pak.zip" .
	@echo "Created $(DIST_DIR)/$(PAK_NAME).pak.zip"

push: all
	@if ! command -v adb >/dev/null 2>&1; then \
		echo "Error: adb not found in PATH"; exit 1; \
	fi
	adb push "$(DIST_DIR)/Tools/$(PUSH_PLATFORM)/$(PAK_NAME).pak/." "$(PUSH_SDCARD_PATH)/Tools/$(PUSH_PLATFORM)/$(PAK_NAME).pak"

deploy: push

clean:
	rm -rf $(BUILD_DIR)/host $(BUILD_DIR)/tg5040 $(BUILD_DIR)/h700 $(BUILD_DIR)/bin $(BUILD_DIR)/staging $(DIST_DIR)

distclean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
