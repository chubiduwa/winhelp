ZIG_VERSION := 0.15.2

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  ZIG_OS := macos
else ifeq ($(UNAME_S),Linux)
  ZIG_OS := linux
else
  ZIG_OS := windows
endif

ifeq ($(UNAME_M),arm64)
  ZIG_ARCH := aarch64
else ifeq ($(UNAME_M),aarch64)
  ZIG_ARCH := aarch64
else
  ZIG_ARCH := x86_64
endif

ZIG_PLATFORM := $(ZIG_ARCH)-$(ZIG_OS)

ifeq ($(ZIG_OS),windows)
  ZIG_ARCHIVE := zig-$(ZIG_PLATFORM)-$(ZIG_VERSION).zip
  ZIG_EXTRACT := unzip -q
else
  ZIG_ARCHIVE := zig-$(ZIG_PLATFORM)-$(ZIG_VERSION).tar.xz
  ZIG_EXTRACT := tar xJf
endif

ZIG_URL := https://ziglang.org/download/$(ZIG_VERSION)/$(ZIG_ARCHIVE)
ZIG_DIR := zig-sdk
ZIG := $(ZIG_DIR)/zig

# Build settings
SRC := $(wildcard src/wasm/*.c) $(wildcard src/wasm/wmf/*.c) $(wildcard src/wasm/vendor/*.c)
TTF := $(wildcard fonts/*.ttf)
WOFF2 := $(patsubst fonts/%.ttf,fonts/woff2/%.woff2,$(TTF))
DIST := dist
OUT := $(DIST)/hlp.wasm

STATIC := src/index.html src/main.js src/style.css src/robots.txt

.PHONY: all clean setup serve watch fonts assets

all: $(OUT) assets

# Download and extract zig toolchain
$(ZIG):
	@echo "Downloading Zig $(ZIG_VERSION) for $(ZIG_PLATFORM)..."
	curl -fSL -o $(ZIG_ARCHIVE) $(ZIG_URL)
	$(ZIG_EXTRACT) $(ZIG_ARCHIVE)
	mv zig-$(ZIG_PLATFORM)-$(ZIG_VERSION) $(ZIG_DIR)
	rm -f $(ZIG_ARCHIVE)
	@echo "Zig installed to $(ZIG_DIR)/"

setup: $(ZIG)

# Build WASM and copy static assets into dist/
$(OUT): $(ZIG) $(SRC)
	@mkdir -p $(DIST)
	$(ZIG) cc \
		-target wasm32-freestanding \
		-O2 \
		-Isrc/wasm -Isrc/wasm/vendor -Isrc/wasm/vendor/libc \
		-o $(OUT) \
		$(SRC) \
		-Wl,--no-entry -Wl,--import-memory

assets: $(STATIC)
	@mkdir -p $(DIST) $(DIST)/fonts $(DIST)/icons
	@cp $(STATIC) $(DIST)/
	@cp fonts/woff2/*.woff2 $(DIST)/fonts/ 2>/dev/null || true
	@cp src/icons/*.png $(DIST)/icons/ 2>/dev/null || true

# Convert TTF fonts to WOFF2
fonts: $(WOFF2)

fonts/woff2/%.woff2: fonts/%.ttf
	@command -v woff2_compress >/dev/null 2>&1 || { echo "Error: woff2_compress not found. Install with: brew install woff2 (macOS) or apt install woff2 (Linux)"; exit 1; }
	@mkdir -p fonts/woff2
	woff2_compress $<
	@mv fonts/$*.woff2 $@

# Local dev server with auto-rebuild
serve: $(OUT)
	@echo "Serving at http://localhost:8000 (rebuilds on source changes)"
	@$(MAKE) watch 2>/dev/null &
	@python3 -m http.server 8000 -d $(DIST)

# Watch for source changes and rebuild
watch: $(ZIG)
ifeq ($(UNAME_S),Darwin)
	@command -v fswatch >/dev/null 2>&1 || { echo "Warning: fswatch not found, auto-rebuild disabled. Install with: brew install fswatch"; exit 0; }
	@fswatch -o src/ | while read; do $(MAKE) $(OUT) assets; done
else
	@command -v inotifywait >/dev/null 2>&1 || { echo "Warning: inotifywait not found, auto-rebuild disabled. Install with: apt install inotify-tools"; exit 0; }
	@while true; do inotifywait -qre modify src/; $(MAKE) $(OUT) assets; done
endif

clean:
	rm -rf $(DIST) $(HOME)/.cache/zig

distclean: clean
	rm -rf $(ZIG_DIR)
