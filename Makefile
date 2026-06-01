.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPRO)/wups/share/wups_rules

WUT_ROOT := $(DEVKITPRO)/wut
WUMS_ROOT := $(DEVKITPRO)/wums

TARGET		:=	HomebrewLoader
BUILD		:=	build
SOURCES		:=	src src/utils src/homebrew src/menu
DATA		:=	data
INCLUDES	:=	src src/menu

CFLAGS	:=	-g -Wall -O2 -ffunction-sections -DDEBUG \
			$(MACHDEP)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__ -D__WUPS__

CXXFLAGS	:= $(CFLAGS)

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map) -T$(WUMS_ROOT)/share/libmappedmemory.ld $(WUPSSPECS)

ifeq ($(DEBUG),1)
CXXFLAGS += -DDEBUG -g
CFLAGS += -DDEBUG -g
endif

CXXFLAGS += -O2
CFLAGS += -O2

LIBS	:= -lwups -lwut -lrpxloader -lmappedmemory

LIBDIRS	:= $(PORTLIBS) $(WUPS_ROOT) $(WUT_ROOT) $(WUMS_ROOT)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@$(shell [ ! -d $(BUILD) ] && mkdir -p $(BUILD))
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wps $(TARGET).elf

else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).wps

$(OUTPUT).wps	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

endif
