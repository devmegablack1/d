# Uses ">" instead of a TAB for recipes, so copy/paste can't break it.
.RECIPEPREFIX := >

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set)
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set)
endif

LIBNDS := $(DEVKITPRO)/libnds
CALICO := $(DEVKITPRO)/calico
CC     := $(DEVKITARM)/bin/arm-none-eabi-gcc
TARGET := mp3player

# libnds 2.x (calico) and older libnds keep these in different places
SPECS    := $(firstword $(wildcard $(CALICO)/share/ds9.specs) ds_arm9.specs)
ARM7ELF  := $(firstword $(wildcard $(CALICO)/bin/ds7_maxmod.elf $(LIBNDS)/default.elf $(CALICO)/bin/ds7*.elf))
NDSTOOL  := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/ndstool) ndstool)

ARCH    := -mthumb -mthumb-interwork
CFLAGS  := -O2 -Wall -march=armv5te -mtune=arm946e-s -fomit-frame-pointer \
           -ffunction-sections -fdata-sections $(ARCH) -D__NDS__ -DARM9 \
           -I$(LIBNDS)/include -I$(CALICO)/include
LDFLAGS := -specs=$(SPECS) $(ARCH) -Wl,--gc-sections \
           -L$(LIBNDS)/lib -L$(CALICO)/lib
LIBS    := -lfat -lmm9 -lnds9

all: $(TARGET).nds

$(TARGET).elf: main.c minimp3.h
> $(CC) $(CFLAGS) main.c $(LDFLAGS) $(LIBS) -o $@

$(TARGET).nds: $(TARGET).elf icon.bmp
ifeq ($(strip $(ARM7ELF)),)
> $(error No ARM7 binary found - check the "Show toolchain" step in the log)
endif
> $(NDSTOOL) -c $@ -9 $< -7 $(ARM7ELF) -b icon.bmp "MP3 Player;Made by koi;"

clean:
> rm -f $(TARGET).elf $(TARGET).nds

.PHONY: all clean
