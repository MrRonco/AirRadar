# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Franco Raso
# Makefile — builds AirRadar's UI as a macOS app. See README.md.
#
# Plain make on purpose: the only thing you need installed is SDL2, and the
# LVGL source already on disk for the Arduino build is reused as-is. No cmake,
# no second copy of LVGL, no submodule.

LVGL    ?= $(HOME)/Documents/Arduino/libraries/lvgl
SRC      = ../firmware/AirRadar/src
BUILD    = build
BIN      = airradar-ui

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

# LV_CONF_PATH points LVGL at the firmware's lv_conf.h VERBATIM -- same colour
# depth, same LV_INV_BUF_SIZE, same font set as the device. It asks for two
# ESP-only symbols (ps_malloc, millis); shim/ supplies both, which is cheaper
# than a second lv_conf that would drift out of sync.
CPPFLAGS = -DLV_CONF_PATH=$(CURDIR)/../firmware/lv_conf.h \
           -I$(LVGL) -I$(LVGL)/src -Ishim -I$(SRC) $(SDL_CFLAGS)
# -MMD -MP so editing a shim header actually rebuilds what included it. Without
# it a header change silently keeps stale objects, which during UI iteration
# looks exactly like "my edit did nothing".
DEPFLAGS = -MMD -MP
WARN     = -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers
CFLAGS   = -O1 -g $(WARN) $(DEPFLAGS)
CXXFLAGS = -O1 -g -std=gnu++17 $(WARN) $(DEPFLAGS)
LDLIBS   = $(SDL_LIBS)

# Firmware sources that are pure UI or pure logic. Anything that touches the
# network, flash or the panel is faked in fakes.cpp instead.
FW_CXX = $(SRC)/ui/ui_nav.cpp $(SRC)/ui/ui_cards.cpp $(SRC)/ui/ui_scope.cpp \
         $(SRC)/ui/ui_help.cpp $(SRC)/ui/ui_settings.cpp $(SRC)/ui/theme.cpp \
         $(SRC)/ui/brandcolor.cpp $(SRC)/core/state.cpp $(SRC)/core/tracks.cpp
FW_C   = $(wildcard $(SRC)/assets/*.c)
LV_C   = $(shell find $(LVGL)/src -name '*.c')
OWN    = main.cpp fakes.cpp scenarios.cpp

OBJ = $(patsubst %,$(BUILD)/%.o,$(notdir $(OWN)) $(notdir $(FW_CXX)) $(notdir $(FW_C))) \
      $(patsubst $(LVGL)/%,$(BUILD)/lv/%.o,$(LV_C))

VPATH = .:$(SRC)/ui:$(SRC)/core:$(SRC)/assets

all: $(BIN)

$(BIN): $(OBJ)
	@echo "  LD  $@"
	@$(CXX) $(OBJ) $(LDLIBS) -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX $<"
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/lv/%.c.o: $(LVGL)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Render every scenario to a BMP without a display attached.
shots: $(BIN)
	@mkdir -p shots
	@for i in 0 1 2 3 4 5 6; do ./$(BIN) --shot shots/scn$$i.bmp --scenario $$i; done

clean:
	@rm -rf $(BUILD) $(BIN) shots shot.bmp

-include $(OBJ:.o=.d)

.PHONY: all clean shots
