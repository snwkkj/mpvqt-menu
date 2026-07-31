CXX ?= g++
LD ?= ld
OBJCOPY ?= objcopy
PKG_CONFIG ?= pkg-config

BUILD_DIR := build
DIST_DIR := dist

# mpv configuration directory for the user running make.
# Override it when needed, for example:
# make install MPV_SCRIPTS_DIR=/custom/path/mpv/scripts
ifeq ($(strip $(XDG_CONFIG_HOME)),)
MPV_CONFIG_DIR ?= $(HOME)/.config/mpv
else
MPV_CONFIG_DIR ?= $(XDG_CONFIG_HOME)/mpv
endif
MPV_SCRIPTS_DIR ?= $(MPV_CONFIG_DIR)/scripts

CXXFLAGS := -std=c++17 -O2 -fPIC
QT_CORE_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt6Core mpv)
QT_CORE_LIBS := $(shell $(PKG_CONFIG) --libs Qt6Core)
QT_WIDGETS_FLAGS := $(shell $(PKG_CONFIG) --cflags --libs Qt6Widgets)

.PHONY: all install clean

all: $(DIST_DIR)/menu.so $(DIST_DIR)/menu.lua

$(BUILD_DIR) $(DIST_DIR):
	mkdir -p $@

$(BUILD_DIR)/menu-helper: src/helper_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pie $< -o $@ $(QT_WIDGETS_FLAGS)

$(BUILD_DIR)/helper_blob.o: $(BUILD_DIR)/menu-helper
	cd $(BUILD_DIR) && $(LD) -r -b binary menu-helper -o helper_blob.o
	$(OBJCOPY) --rename-section .data=.rodata,alloc,load,readonly,data,contents $@

$(BUILD_DIR)/menu.o: src/menu.cpp src/menu.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(QT_CORE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/plugin.o: src/plugin.cpp src/plugin.h src/menu.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DMPV_CPLUGIN_DYNAMIC_SYM $(QT_CORE_CFLAGS) -c $< -o $@

$(DIST_DIR)/menu.so: $(BUILD_DIR)/menu.o $(BUILD_DIR)/plugin.o $(BUILD_DIR)/helper_blob.o | $(DIST_DIR)
	$(CXX) -shared $^ -o $@ $(QT_CORE_LIBS) -pthread -Wl,-z,noexecstack

$(DIST_DIR)/menu.lua: src/lua/menu.lua | $(DIST_DIR)
	cp $< $@

install: all
	install -d "$(MPV_SCRIPTS_DIR)"
	install -m 755 "$(DIST_DIR)/menu.so" "$(MPV_SCRIPTS_DIR)/menu.so"
	install -m 644 "$(DIST_DIR)/menu.lua" "$(MPV_SCRIPTS_DIR)/menu.lua"

clean:
	rm -f $(BUILD_DIR)/menu-helper \
	      $(BUILD_DIR)/helper_blob.o \
	      $(BUILD_DIR)/menu.o \
	      $(BUILD_DIR)/plugin.o \
	      $(DIST_DIR)/menu.so \
	      $(DIST_DIR)/menu.lua
