# SPDX-License-Identifier: GPL-2.0-only
# Reproducible bare-metal MIPS32 (Taroko) build for RTL9617C PE firmware.

CLANG       ?= clang
LLD         ?= $(shell command -v ld.lld-18 || command -v ld.lld)
OBJCOPY     ?= $(shell command -v llvm-objcopy-18 || command -v llvm-objcopy)
OBJDUMP     ?= $(shell command -v llvm-objdump-18 || command -v llvm-objdump)

CFLAGS := --target=mipsel-unknown-none-elf -march=mips1 -mabi=32 \
          -msoft-float -mno-abicalls -fno-pic -G0 -Os \
          -mno-check-zero-division \
          -ffreestanding -fno-builtin -fno-stack-protector \
          -Wall -Wextra -Werror

ASFLAGS := --target=mipsel-unknown-none-elf -march=mips1 -mabi=32 \
           -msoft-float -mno-abicalls -fno-pic

TARGET_BIN := pe-offload.bin
TARGET_ELF := pe-offload.elf

all: $(TARGET_BIN)

pe-entry.o: pe-entry.S
	$(CLANG) $(ASFLAGS) -c $< -o $@

pe-main.o: pe-main.c rtl9617c-pe-abi.h
	$(CLANG) $(CFLAGS) -c $< -o $@

$(TARGET_ELF): pe-entry.o pe-main.o pe.ld
	$(LLD) -T pe.ld --gc-sections -o $@ pe-entry.o pe-main.o

$(TARGET_BIN): $(TARGET_ELF)
	@if $(OBJDUMP) -d $< | grep -qE "\b(mul|movn|movz|lwl|lwr|swl|swr|ll|sc|clz|clo|seb|seh|teq|tne|tlt|tltu|tge|tgeu)\b"; then \
		echo "ERROR: non-Taroko MIPS-I instruction emitted!"; \
		$(OBJDUMP) -d $< | grep -E "\b(mul|movn|movz|lwl|lwr|swl|swr|ll|sc|clz|clo|seb|seh|teq|tne|tlt|tltu|tge|tgeu)\b"; \
		exit 1; \
	fi
	$(OBJCOPY) -O binary $< $@
	@sz=$$(stat -c%s $@); echo "Built $@ ($${sz} bytes, max 32768 IMEM)"
	@test $$(stat -c%s $@) -le 32768

disasm: $(TARGET_ELF)
	$(OBJDUMP) -d $<

clean:
	rm -f pe-entry.o pe-main.o $(TARGET_ELF) $(TARGET_BIN)

.PHONY: all disasm clean
