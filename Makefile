# Copyright (C) 2020 github.com/aramg

DATA_DIR = data
BUILD_DIR = build
ARCH = $(shell getconf LONG_BIT)

RM       = rm
CXX      = g++
CXXFLAGS = -std=c++11 -x c++ -Wall -fPIC
INCLUDES = -Isrc/
LDD_DIRS =
LDD_LIBS = -lobs-frontend-api
LDD_FLAG =
LIB_DLL  = $(BUILD_DIR)/droidcam-obs.so
SRC      = $(shell ls src/*.c src/sys/unix/*.c)

ifeq ($(OS),Windows_NT)
lol:
	@exit
endif

.PHONY: run clean
all: $(LIB_DLL)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
## LINUX ##
	INCLUDES += -I/usr/include/obs
	LDD_LIBS += -lobs
	LDD_FLAG += -shared

	PLUGIN_INSTALL_DIR = ~/.config/obs-studio/plugins
	PLUGIN_BIN_DIR     = $(BUILD_DIR)/bin/$(ARCH)bit

run:
	rm ~/.config/obs-studio/logs/* && obs
## LINUX ##
endif

ifeq ($(UNAME),Darwin)
## MACOS ##
	CXXFLAGS += -dead_strip
	INCLUDES += -I/usr/local/opt/qt5/include
	INCLUDES += -I/usr/local/opt/qt5/include/QtCore
	INCLUDES += -I/usr/local/opt/qt5/include/QtWidgets
	INCLUDES += -I../ffmpeg
	INCLUDES += -I../obs-studio-24.0.2/UI
	INCLUDES += -I../obs-studio-24.0.2/libobs
	LDD_DIRS += -L/Applications/OBS.app/Contents/Resources/bin
	LDD_LIBS += -lobs.0 -lavcodec.58 -lavformat.58 -lavutil.56
	LDD_FLAG += -bundle

run:
	rm ~/Library/ApplicationSupport/obs-studio/logs/* && /Applications/OBS.app/Contents/MacOS/OBS
## MACOS ##
endif

$(LIB_DLL): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDD_DIRS) $(LDD_LIBS) $(LDD_FLAG) $^ -o $@

clean:
	$(RM) $(BUILD_DIR)/*.o $(BUILD_DIR)/*.so

adbz:
	gcc -o /tmp/adbz src/test/adbz.c

test: adbz
	g++ -DTEST -otest -Isrc/ -Isrc/test/ src/net.c src/command.c src/sys/unix/cmd.c src/test/main.c
