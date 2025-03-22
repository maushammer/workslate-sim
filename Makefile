#
# Usage:
#
# make all      -- makes all
# make web      -- makes webasm version only (uses cheerp/clang for a compiler)
# make term     -- makes version to run in terminal (uses gcc for a compiler)
#


SRC_DIR         := src
GCC_BUILD_DIR   := build/gcc
WASM_BUILD_DIR  := build/wasm
WEBPAGE         := build/webpage
EXE             := $(GCC_BUILD_DIR)/workslate
WASM_EXE        := $(WASM_BUILD_DIR)/workslate.js

GCC_FLAGS  := -Wall -Wshadow -Wextra -Wno-unused-parameter -O2 -g
WASM_FLAGS := -Wall -Wshadow -Wextra -Wno-unused-parameter -Wno-deprecated -target cheerp-wasm -O2 -g -DWASM
# We define the WASM macro so our code can use #if for it.
#
# Note: we can't mix clang++ and clang ... compiles well, but crashes when we try a function
#       call across that barrier.

SRCS := $(shell find $(SRC_DIR) -name '*.c')
GCC_OBJS := $(subst $(SRC_DIR), $(GCC_BUILD_DIR), $(SRCS:.c=.o))
WASM_OBJS := $(subst $(SRC_DIR), $(WASM_BUILD_DIR), $(SRCS:.c=.wasm)) $(WASM_BUILD_DIR)/u15.wasm $(WASM_BUILD_DIR)/u16.wasm 
# ^ to do - add cpp

#all : $(GCC_OBJS) $(EXE)
all : $(EXE) $(WEBPAGE)
term : $(EXE)
web : $(WEBPAGE)

#----- making executables ---------
$(EXE) : $(GCC_OBJS) | $(GCC_BUILD_DIR)
	@echo "------ Make gcc $(EXE) ------"
	@rm -f $(EXE)
	gcc $(GCC_FLAGS) -o $(EXE) $(GCC_OBJS)

$(GCC_BUILD_DIR)/%.o : $(SRC_DIR)/%.c | $(GCC_BUILD_DIR)
	@echo "------ Make gcc $(@) ------"
	@rm -f $@
	gcc $(GCC_FLAGS) -c -o $@ $<

$(WASM_BUILD_DIR)/%.wasm : $(SRC_DIR)/%.c | $(WASM_BUILD_DIR)
	@echo "------ Make wasm $(@) ------"
	@rm -f $@
	/Applications/cheerp/bin/clang++ $(WASM_FLAGS) -c -o $@ $<

# U15 and U16 are normally loaded from the resource file, but I couldn't get files working
# in WASM, so they are built in to the source code instead:
# resource_u15_bin[]
$(WASM_BUILD_DIR)/u15.wasm : resource/u15.bin | $(WASM_BUILD_DIR)
	@echo "------ Make wasm $(@) rom file ------"
	@rm -f $@
	xxd --include resource/u15.bin > $(WASM_BUILD_DIR)/u15.c
	/Applications/cheerp/bin/clang++ $(WASM_FLAGS) -c -o $@ $(WASM_BUILD_DIR)/u15.c

$(WASM_BUILD_DIR)/u16.wasm : resource/u16.bin | $(WASM_BUILD_DIR)
	@echo "------ Make wasm $(@) rom file ------"
	@rm -f $@
	xxd --include resource/u16.bin > $(WASM_BUILD_DIR)/u16.c
	/Applications/cheerp/bin/clang++ $(WASM_FLAGS) -c -o $@ $(WASM_BUILD_DIR)/u16.c

$(WASM_EXE) : $(WASM_OBJS) | $(WASM_BUILD_DIR)
	/Applications/cheerp/bin/clang++ $(WASM_FLAGS) -cheerp-pretty-code -cheerp-sourcemap=$(WASM_BUILD_DIR)/workslate.js.map -o $(WASM_EXE) src/workslate-wasm.cpp $(WASM_OBJS)

#----- making directories and relases ---------
# TODO: no need for rom files u15, u16
# TODO: remove .js.map file for production
$(GCC_BUILD_DIR):
	mkdir -p $(GCC_BUILD_DIR)
$(WASM_BUILD_DIR):
	mkdir -p $(WASM_BUILD_DIR)
$(WEBPAGE): $(WASM_EXE)
	mkdir -p $(WEBPAGE)
	cp resource/* $(WEBPAGE)
	cp $(WASM_EXE) $(WEBPAGE)
	cp $(subst .js,.wasm, $(WASM_EXE)) $(WEBPAGE)
	echo $(WASM_EXE)
	cp $(subst .js,.js.map, $(WASM_EXE)) $(WEBPAGE)
#clean up for publish
	rm $(WEBPAGE)/u15.bin
	rm $(WEBPAGE)/u16.bin
	rm $(WEBPAGE)/workslate_facts
	rm $(WEBPAGE)/workslate.js.map



-include $(GCC_BUILD_DIR)/*.d

clean:
	rm -rf $(GCC_BUILD_DIR) $(WASM_BUILD_DIR) $(WEBPAGE)
	rm -f register_access_log.txt

