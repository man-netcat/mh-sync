#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/3ds_rules

TARGET      := mh-sync-3ds
BUILD       := build
SOURCES     := source ../common

APP_TITLE       := MH-Sync 3DS
APP_DESCRIPTION := Monster Hunter save sync client
APP_AUTHOR      := mh-sync

DEPSDIR     ?= $(BUILD)

INCLUDE     := -I$(CTRULIB)/include -I$(PORTLIBS)/include -I../common

CFLAGS      := -g -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef -Wcast-align -Wold-style-cast -Woverloaded-virtual -Wreorder -Wnon-virtual-dtor -Wformat=2 -Wswitch-enum -Wmaybe-uninitialized -Wno-psabi -O2
CFLAGS      += $(INCLUDE)

CXXFLAGS    := $(CFLAGS) -std=gnu++20

LIBS        := -lcitro2d -lcitro3d -lctru -lm

OUTDIR      := build
OUTPUT      := $(OUTDIR)/$(TARGET)

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -fno-rtti

CFLAGS   += $(ARCH)
CXXFLAGS += $(ARCH)
LDFLAGS  := -specs=3dsx.specs $(ARCH) -Wl,-Map,$(notdir $*.map) -L$(CTRULIB)/lib -L$(PORTLIBS)/lib

export LD := $(CXX)

#---------------------------------------------------------------------------------
# get revision number from git
ifneq ($(strip $(GIT_REV)),)
    CFLAGS += -DGIT_REV=\"$(GIT_REV)\"
endif

#---------------------------------------------------------------------------------
# list of all source files and objects
#---------------------------------------------------------------------------------
CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

OFILES    := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

#---------------------------------------------------------------------------------
# targets
#---------------------------------------------------------------------------------
.PHONY: all clean

all: $(OUTPUT).3dsx

$(OUTPUT).smdh:
	@mkdir -p $(OUTDIR)
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(CTRULIB)/default_icon.png $@

$(OUTPUT).3dsx: $(OFILES) $(OUTPUT).smdh
	@mkdir -p $(OUTDIR)
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBS) -o $(OUTPUT).elf
	@3dsxtool $(OUTPUT).elf $(OUTPUT).3dsx --smdh=$(OUTPUT).smdh
	@echo "built ... $(notdir $(OUTPUT)).3dsx"

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -rf $(BUILD) $(OUTDIR)
	@rm -f *.o *.d

#---------------------------------------------------------------------------------
%.o: %.cpp
	@mkdir -p $(BUILD)
	$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) -c $< -o $@

%.o: %.c
	@mkdir -p $(BUILD)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) -c $< -o $@

-include $(DEPSDIR)/*.d
