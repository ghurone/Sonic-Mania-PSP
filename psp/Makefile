# Sonic Mania (RSDKv5 decompilation) for PSP
#
# Build from this directory with the PSP toolchain on PATH, e.g. (PowerShell):
#   $env:PATH = "C:\pspdev\bin;C:\msys64\usr\bin;C:\msys64\ucrt64\bin;$env:PATH"
#   make -j8
#
# Only relative paths are used on purpose: the workspace path contains non-ASCII
# characters which the native Windows toolchain cannot receive from msys make.

TARGET = SonicMania
# (plain "make" builds everything: the dependency lines below would otherwise be the default goal)
.DEFAULT_GOAL := all

DECOMP = ../SonicManiaDecomp
RSDK   = $(DECOMP)/dependencies/RSDKv5
ENG    = $(RSDK)/RSDKv5
DEPS   = $(RSDK)/dependencies/all
GAME   = $(DECOMP)/SonicMania

ENGINE_SRCS = \
	$(ENG)/main.cpp \
	$(ENG)/RSDK/Core/RetroEngine.cpp \
	$(ENG)/RSDK/Core/Math.cpp \
	$(ENG)/RSDK/Core/Reader.cpp \
	$(ENG)/RSDK/Core/Link.cpp \
	$(ENG)/RSDK/Dev/Debug.cpp \
	$(ENG)/RSDK/Storage/Storage.cpp \
	$(ENG)/RSDK/Storage/Text.cpp \
	$(ENG)/RSDK/Graphics/Drawing.cpp \
	$(ENG)/RSDK/Graphics/Scene3D.cpp \
	$(ENG)/RSDK/Graphics/Animation.cpp \
	$(ENG)/RSDK/Graphics/Sprite.cpp \
	$(ENG)/RSDK/Graphics/Palette.cpp \
	$(ENG)/RSDK/Graphics/Video.cpp \
	$(ENG)/RSDK/Audio/Audio.cpp \
	$(ENG)/RSDK/Input/Input.cpp \
	$(ENG)/RSDK/Scene/Scene.cpp \
	$(ENG)/RSDK/Scene/Collision.cpp \
	$(ENG)/RSDK/Scene/Object.cpp \
	$(ENG)/RSDK/Scene/Objects/DefaultObject.cpp \
	$(ENG)/RSDK/Scene/Objects/DevOutput.cpp \
	$(ENG)/RSDK/User/Core/UserAchievements.cpp \
	$(ENG)/RSDK/User/Core/UserCore.cpp \
	$(ENG)/RSDK/User/Core/UserLeaderboards.cpp \
	$(ENG)/RSDK/User/Core/UserPresence.cpp \
	$(ENG)/RSDK/User/Core/UserStats.cpp \
	$(ENG)/RSDK/User/Core/UserStorage.cpp \
	$(DEPS)/tinyxml2/tinyxml2.cpp \
	$(DEPS)/iniparser/iniparser.cpp \
	$(DEPS)/iniparser/dictionary.cpp

ENGINE_CSRCS = $(DEPS)/miniz/miniz.c

GAME_CSRCS = $(GAME)/Game.c $(GAME)/Objects/All.c

OBJS = $(ENGINE_SRCS:.cpp=.o) $(ENGINE_CSRCS:.c=.o) $(GAME_CSRCS:.c=.o)

# The PSP render device is selected through an include in Drawing.cpp rather
# than compiled as a standalone translation unit, so make the dependency clear.
$(ENG)/RSDK/Graphics/Drawing.o: $(ENG)/RSDK/Graphics/PSP/PSPRenderDevice.cpp
$(ENG)/RSDK/Input/Input.o: $(ENG)/RSDK/Input/PSP/PSPInputDevice.cpp
$(ENG)/RSDK/Scene/Scene.o: $(ENG)/RSDK/Scene/PSP/PSPTileRows.hpp $(ENG)/RSDK/Scene/PSP/PSPHScrollReference.inl $(ENG)/RSDK/Scene/PSP/PSPHScrollTextured.inl $(ENG)/RSDK/Scene/Scene.hpp
$(ENG)/RSDK/Scene/Scene.o: $(ENG)/RSDK/Scene/PSP/PSPHScrollHybrid.inl $(ENG)/RSDK/Scene/PSP/PSPBasicRows.inl $(ENG)/RSDK/Scene/PSP/PSPRotozoomGroups.inl $(ENG)/RSDK/Scene/PSP/PSPTitleReference.inl $(ENG)/RSDK/Scene/PSP/PSPTitleCompare.inl
$(ENG)/RSDK/Scene/Object.o: $(ENG)/RSDK/Scene/Scene.hpp

PSP_HSCROLL_OPT ?= 1
# 1: re-time the layer routines in-game (extra layer redraws: visible hitches), for diagnostics only
PSP_TILE_BENCH ?= 0
# 1: decode every GIF twice (fast + original decoder) and log mismatches, for diagnostics only
PSP_GIF_VERIFY ?= 0
# 1: time CPU drawing into VRAM vs RAM at startup, for diagnostics only
PSP_VRAM_BENCH ?= 0
# memory pool sizes for measuring builds, e.g. PSP_POOLS="-DPSP_STG_KB=17408 -DPSP_TMP_KB=3072" (see RetroEngine.hpp)
PSP_POOLS ?=
# RELEASE=1: the build to hand out (GitHub releases): the Plus DLC off, as the RSDKv5 license requires of any
# pre-built executable, and no XMB icon/background/sound (they would be the game's art and music)
RELEASE ?= 0
ifeq ($(RELEASE),1)
AUTOBUILD = 1
else
AUTOBUILD = 0
endif

DEFS = -DRETRO_REVISION=2 -DRETRO_USE_MOD_LOADER=0 -DRETRO_STANDALONE=0 -DRETRO_DISABLE_LOG=0 \
       -DGAME_INCLUDE_EDITOR=0 -DMANIA_PREPLUS=0 -DGAME_VERSION=6 -DRSDK_AUTOBUILD=$(AUTOBUILD) \
       -DRETRO_DEV_EXTRA=\"PSP\" -DMINIZ_NO_TIME -DTINYXML2_NO_EXCEPTIONS \
       -DPSP_HSCROLL_OPT=$(PSP_HSCROLL_OPT) -DPSP_TILE_BENCH=$(PSP_TILE_BENCH) -DPSP_GIF_VERIFY=$(PSP_GIF_VERIFY) -DPSP_VRAM_BENCH=$(PSP_VRAM_BENCH) $(PSP_POOLS)

INCDIR = $(ENG) $(DEPS) $(DEPS)/tinyxml2 $(DEPS)/iniparser $(GAME) $(GAME)/Objects

OPT ?= -O2

# BUILD_PRX=1: relocatable module, needed to run under PSPLink (which refuses static ELFs). The PSP loader pairs each
# %hi relocation with the %lo that follows it, so keep them adjacent (explicit relocs let GCC hoist and spill a %hi).
ifeq ($(BUILD_PRX),1)
PRX_CFLAGS = -mno-explicit-relocs
endif

CFLAGS   = $(OPT) -G0 -Wall -Wno-unused -Wno-sign-compare -Wno-format -Wno-narrowing -Wno-write-strings \
           -fno-strict-aliasing -fomit-frame-pointer -ffast-math -mno-check-zero-division $(PRX_CFLAGS) $(DEFS)
CXXFLAGS = -fno-exceptions -fno-rtti -fpermissive
ASFLAGS  = $(CFLAGS)

LIBS = -lstdc++ -lpspatrac3 -lpspjpeg -lpsputility -lpspgu -lpspaudio -lpsppower -lpsprtc -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Sonic Mania
# icon, background and menu sound of the XMB: used when present in this folder (they are not distributed: they
# would be the game's art and music)
ifeq ($(RELEASE),1)
PSP_EBOOT_ICON  = NULL
PSP_EBOOT_PIC1  = NULL
PSP_EBOOT_SND0  = NULL
else
PSP_EBOOT_ICON  = $(if $(wildcard ICON0.PNG),ICON0.PNG,NULL)
PSP_EBOOT_PIC1  = $(if $(wildcard PIC1.PNG),PIC1.PNG,NULL)
PSP_EBOOT_SND0  = $(if $(wildcard SND0.AT3),SND0.AT3,NULL)
endif
# PSP-3000 target: request the expanded memory profile understood by PPSSPP.
PSP_LARGE_MEMORY = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
