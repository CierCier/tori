# Kernel build system
#
# Usage (via top-level Makefile):
#   make kernel
#
# Standalone:
#   make -f kernel/Makefile
#   qemu-system-x86_64 -kernel build/boot/kernel.elf ...

CC        ?= clang
CXX       ?= clang++
LD        ?= clang++
AS        ?= clang
NM        ?= llvm-nm

KERNEL_DIR         := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
KERNEL_OUTPUT_DIR  ?= $(KERNEL_DIR)/build
SRC_DIR            := $(KERNEL_DIR)/src
BIN_DIR            := $(KERNEL_OUTPUT_DIR)/boot
OBJ_DIR            := $(KERNEL_OUTPUT_DIR)/obj
GEN_DIR            := $(KERNEL_OUTPUT_DIR)/gen

INCLUDES := -I$(KERNEL_DIR) \
            -I$(KERNEL_DIR)/include \
            -I$(KERNEL_DIR)/../shared/include \
            -I$(KERNEL_DIR)/../third_party/limine-protocol/include

COMMON_FLAGS := --target=x86_64-unknown-none \
                -ffreestanding -fno-builtin -fno-PIC \
                -fno-stack-protector -mno-red-zone \
                -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
                -mcmodel=kernel \
                -fasynchronous-unwind-tables -fno-omit-frame-pointer \
                -g

CXXFLAGS := $(COMMON_FLAGS) -std=c++20 -fno-exceptions -fno-rtti
ASFLAGS  := --target=x86_64-unknown-none
LDFLAGS  := --target=x86_64-unknown-none \
            -fuse-ld=lld -nostdlib -static \
            -Wl,-z,max-page-size=0x1000 \
            -Wl,--eh-frame-hdr \
            -Wl,-T,$(KERNEL_DIR)/targets/x86_64-limine/linker.ld

# Generated ISR stubs (256 explicit entries, no macro expansion needed)
GEN_STUBS := $(GEN_DIR)/isr_stubs.S

CPP_SOURCES := $(shell find $(SRC_DIR) -name '*.cpp' | LC_ALL=C sort)
ASM_SOURCES := $(shell find $(SRC_DIR) -name '*.S'   | LC_ALL=C sort)

CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(CPP_SOURCES))
ASM_OBJS := $(patsubst $(SRC_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
GEN_OBJS := $(OBJ_DIR)/gen/isr_stubs.o
ALL_OBJS := $(CPP_OBJS) $(ASM_OBJS) $(GEN_OBJS)
DEPS     := $(ALL_OBJS:.o=.d)

KERNEL_ELF    := $(BIN_DIR)/kernel.elf
KERNEL_ELF_TMP := $(BIN_DIR)/.kernel.elf.tmp
SYMTAB_SRC    := $(GEN_DIR)/symbol_table.cpp
SYMTAB_OBJ    := $(OBJ_DIR)/gen/symbol_table.o

# Exception vectors that push an error code
ERR_VECTORS := 8 10 11 12 13 14 17 21

.PHONY: all clean

all: $(KERNEL_ELF)

# First pass: link without symbol table
$(KERNEL_ELF_TMP): $(ALL_OBJS) $(KERNEL_DIR)/targets/x86_64-limine/linker.ld | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)

# Generate symbol table from first-pass ELF
$(SYMTAB_SRC): $(KERNEL_ELF_TMP) | $(GEN_DIR)
	@echo '  GEN     $(notdir $(SYMTAB_SRC))'
	@{ \
	    echo '#include <stdint.h>'; \
	    echo '#include <stddef.h>'; \
	    echo 'struct __attribute__((packed)) _sym { uint64_t a; const char* n; };'; \
	    echo 'extern "C" _sym __kernel_symtab_start[] = {'; \
	    $(NM) -n '$<' 2>/dev/null | awk '$$2 ~ /^[TtDdWw]$$/ && $$3 != "" { gsub(/"/, "\\\"", $$3); printf "    {0x%s, \"%s\"},\n", $$1, $$3 }'; \
	    echo '};'; \
	    echo 'extern "C" size_t __kernel_symtab_count = sizeof(__kernel_symtab_start) / sizeof(__kernel_symtab_start[0]);'; \
	} > $@

# Compile symbol table
$(SYMTAB_OBJ): $(SYMTAB_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

# Final link with symbol table
$(KERNEL_ELF): $(ALL_OBJS) $(SYMTAB_OBJ) $(KERNEL_DIR)/targets/x86_64-limine/linker.ld | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(SYMTAB_OBJ)

.SECONDARY: $(KERNEL_ELF_TMP)

# Generate ISR stubs: 256 individual stubs + address table
$(GEN_STUBS): | $(GEN_DIR)
	@echo '  GEN     $(GEN_STUBS)'
	@ERR="$(ERR_VECTORS)"; \
	{ \
	    echo '.intel_syntax noprefix'; \
	    echo '.section .text'; \
	    i=0; \
	    while [ $$i -lt 256 ]; do \
	        has_err=0; \
	        for e in $$ERR; do \
	            if [ $$i -eq $$e ]; then has_err=1; break; fi; \
	        done; \
	        echo "  .p2align 4"; \
	        echo "  .global isr_stub_$$i"; \
	        echo "isr_stub_$$i:"; \
	        if [ $$has_err -eq 0 ]; then echo "  push 0"; fi; \
	        echo "  push $$i"; \
	        echo "  jmp  isr_common_handler"; \
	        i=$$((i + 1)); \
	    done; \
	    echo "  .section .data"; \
	    echo "  .global isr_stub_table"; \
	    echo "isr_stub_table:"; \
	    i=0; \
	    while [ $$i -lt 256 ]; do \
	        echo "  .quad isr_stub_$$i"; \
	        i=$$((i + 1)); \
	    done; \
	} > $@

# Compile generated assembly
$(OBJ_DIR)/gen/isr_stubs.o: $(GEN_STUBS) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

# Compile C++
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

# Compile assembly
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

# Directory creation
$(BIN_DIR) $(OBJ_DIR) $(GEN_DIR):
	@mkdir -p $@

clean:
	rm -rf $(KERNEL_OUTPUT_DIR)

-include $(DEPS)
