#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET      := mh-sync-switch
BUILD       := build
SOURCES     := source ../common
INCLUDES    := include ../common
ROMFS       := romfs

APP_TITLE       := MH-Sync Switch
APP_DESCRIPTION := Monster Hunter save sync server
APP_AUTHOR      := man-netcat

DEPSDIR     ?= $(BUILD)

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

INCLUDE     := -I$(LIBNX)/include -Iinclude -I../common

CFLAGS      := -g -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef -Wcast-align -Wold-style-cast -Woverloaded-virtual -Wreorder -Wnon-virtual-dtor -Wformat=2 -Wswitch-enum -Wmaybe-uninitialized -O2 -ffunction-sections $(ARCH) $(INCLUDE)
CXXFLAGS    := $(CFLAGS) -std=gnu++20 -fno-rtti
ASFLAGS     := -g $(ARCH)

-include $(BUILD)/*.d

LIBS        := -lnx -lm

OUTDIR      := build
OUTPUT      := $(OUTDIR)/$(TARGET)

LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -L$(LIBNX)/lib

# get revision number from git
ifneq ($(strip $(GIT_REV)),)
    CFLAGS += -DGIT_REV=\"$(GIT_REV)\"
endif

#---------------------------------------------------------------------------------
# list of all source files and objects
#---------------------------------------------------------------------------------
CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

$(shell mkdir -p $(BUILD))
SFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

OFILES    := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

#---------------------------------------------------------------------------------
.PHONY: all clean

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OFILES)
	@mkdir -p $(OUTDIR)
	@$(CXX) $(LDFLAGS) $(OFILES) $(LIBS) -o $(OUTPUT).elf
	@$(PREFIX)strip $(OUTPUT).elf
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_DESCRIPTION)" $(OUTPUT).nacp
	@elf2nro $(OUTPUT).elf $(OUTPUT).nro --nacp=$(OUTPUT).nacp
	@echo "built ... $(notdir $(OUTPUT)).nro"

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(OUTDIR)
	@rm -f *.o *.d
