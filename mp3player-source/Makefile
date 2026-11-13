ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM (install devkitPro with the nds-dev group)")
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO")
endif

LIBNDS := $(DEVKITPRO)/libnds
CC     := $(DEVKITARM)/bin/arm-none-eabi-gcc
TARGET := mp3player

ARCH    := -mthumb -mthumb-interwork
CFLAGS  := -O2 -Wall -march=armv5te -mtune=arm946e-s -fomit-frame-pointer \
           -ffunction-sections -fdata-sections $(ARCH) -DARM9 \
           -I$(LIBNDS)/include
LDFLAGS := -specs=ds_arm9.specs $(ARCH) -Wl,--gc-sections \
           -L$(LIBNDS)/lib
LIBS    := -lfat -lmm9 -lnds9

all: $(TARGET).nds

$(TARGET).elf: main.c minimp3.h
	$(CC) $(CFLAGS) main.c $(LDFLAGS) $(LIBS) -o $@

# default.elf is libnds' stock ARM7 core, which includes maxmod
$(TARGET).nds: $(TARGET).elf icon.bmp
	$(DEVKITPRO)/tools/bin/ndstool -c $@ -9 $< -7 $(LIBNDS)/default.elf \
		-b icon.bmp "MP3 Player;Made by koi;"

clean:
	rm -f $(TARGET).elf $(TARGET).nds

.PHONY: all clean
