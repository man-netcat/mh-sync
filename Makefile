#---------------------------------------------------------------------------------
# MH-Sync unified Makefile
# Builds 3DS and Switch targets via Docker (recommended) or native devkitPro.
#---------------------------------------------------------------------------------

.PHONY: all 3ds switch clean

all: 3ds switch

#-------------------------------------------------------------------------------
# 3DS target
#-------------------------------------------------------------------------------
3ds:
	@echo "Building 3DS target..."
ifdef DEVKITARM
	$(MAKE) -C 3ds
else
	docker run --rm -v $$(pwd):/project -w /project/3ds \
		devkitpro/devkitarm make
endif
	@echo ""

#-------------------------------------------------------------------------------
# Switch target
#-------------------------------------------------------------------------------
switch:
	@echo "Building Switch target..."
ifdef DEVKITPRO
	$(MAKE) -C switch
else
	docker run --rm -v $$(pwd):/project -w /project/switch \
		devkitpro/devkita64 make
endif
	@echo ""

#-------------------------------------------------------------------------------
# Clean (via docker since build artifacts are owned by root)
#-------------------------------------------------------------------------------
clean:
	docker run --rm -v $$(pwd):/project devkitpro/devkitarm \
		sh -c "rm -rf /project/3ds/build /project/switch/build"
	@echo "clean ..."
