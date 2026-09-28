#---------------------------------------------------------------------------------
# Level Devil -- Nintendo Switch homebrew wrapper port
#
# Runs the game's own Android ARM64 libraries (liblime.so, libApplicationMain.so)
# on the Switch. Contains NO game code or assets: stage them from an APK you own,
# see README.md.
#
# Requires devkitA64 and these devkitPro packages:
#   (dkp-)pacman -S switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau \
#                  switch-libpng switch-zlib
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment, e.g. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := leveldevil
BUILD       := build
SOURCES     := source
INCLUDES    := source

APP_TITLE   := Level Devil
APP_AUTHOR  := ChanseyIsTheBest, tommaso (Level Devil fork)
APP_VERSION := 1.0.0

# Optional; without icon.jpg elf2nro uses the libnx default. Not committed (game artwork).
APP_ICON    := $(wildcard $(TOPDIR)/icon.jpg)

#---------------------------------------------------------------------------------
# Fail early, and name the packages, if the portlibs are missing.
#---------------------------------------------------------------------------------
REQUIRED_HEADERS := $(PORTLIBS)/include/SDL2/SDL.h $(PORTLIBS)/include/EGL/egl.h \
                    $(PORTLIBS)/include/png.h
MISSING := $(foreach h,$(REQUIRED_HEADERS),$(if $(wildcard $(h)),,$(h)))
ifneq ($(strip $(MISSING)),)
$(warning Missing devkitPro portlibs headers: $(MISSING))
$(warning Install them with:  dkp-pacman -S switch-sdl2 switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib)
$(warning (on Windows, run pacman -S ... in the devkitPro MSYS2 shell))
$(error missing portlibs, see above)
endif

#---------------------------------------------------------------------------------
# Code generation
#---------------------------------------------------------------------------------
# -mtp=soft: the loaded modules never touch TPIDR_EL0 themselves, but libnx's
# TLS model requires it for everything linked into the NRO.
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -O2 -ffunction-sections -fno-strict-aliasing $(ARCH) $(DEFINES)
CFLAGS   += $(INCLUDE) -D__SWITCH__

# Mistakes that compile but corrupt values on AArch64 (a pointer truncated to
# an implicit int, a pointer passed as an integer) are errors, not warnings.
CFLAGS   += -Werror=implicit-function-declaration -Werror=implicit-int \
            -Werror=int-conversion -Werror=incompatible-pointer-types \
            -Werror=return-type

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# mesa is partly C++, so the link goes through the C++ driver (LD below).
LIBS     := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lpng -lz -lnx -lm

LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD       := $(CXX)
export OFILES   := $(SFILES:.s=.o) $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS  := $(OFILES:.o=.d)

NROFLAGS := --nacp=$(OUTPUT).nacp
ifneq ($(strip $(APP_ICON)),)
NROFLAGS += --icon=$(APP_ICON)
endif

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(APP_ICON)
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
