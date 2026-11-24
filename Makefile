SHELL := /bin/sh

BUILD := build
SRC := src
TOOLS := tools
KERNEL_SECTORS := 64

CC ?= gcc
CXX ?= g++
LD ?= ld
OBJCOPY ?= objcopy
NASM ?= nasm
XORRISO ?= xorriso

CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
	-mno-red-zone -Wall -Wextra -Werror -Os
CXXFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
	-mno-red-zone -Wall -Wextra -Werror -Wno-sign-compare -Os
LDFLAGS := -m elf_i386 -T $(SRC)/linker.ld

KERNEL_OBJECTS := $(BUILD)/kernel_entry.o $(BUILD)/kernel.o \
	$(BUILD)/browser_engine.o $(BUILD)/network_stack.o

.PHONY: all clean run package iso installer

all: $(BUILD)/gp-os.img package iso installer

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/iso/boot $(BUILD)/apps $(BUILD)/terminal

$(BUILD)/boot.bin: $(SRC)/boot.asm | $(BUILD)
	$(NASM) -f bin -DKERNEL_SECTORS=$(KERNEL_SECTORS) $< -o $@
	test "$$(wc -c < $@)" -eq 512

$(BUILD)/kernel_entry.o: $(SRC)/kernel_entry.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.o: $(SRC)/kernel.cpp $(SRC)/kernel.c $(SRC)/browser_engine.hpp \
	$(SRC)/network_stack.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/browser_engine.o: $(SRC)/browser_engine.cpp $(SRC)/browser_engine.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/network_stack.o: $(SRC)/network_stack.cpp $(SRC)/network_stack.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJECTS) $(SRC)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJECTS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	test "$$(wc -c < $@)" -le "$$(( $(KERNEL_SECTORS) * 512 ))"

$(BUILD)/gp-os.img: $(BUILD)/boot.bin $(BUILD)/kernel.bin
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BUILD)/boot.bin of=$@ conv=notrunc bs=512 seek=0 status=none
	dd if=$(BUILD)/kernel.bin of=$@ conv=notrunc bs=512 seek=1 status=none

package: $(BUILD)/gp-os.img $(TOOLS)/gp-pack.c
	$(CC) -std=c11 -Wall -Wextra -Werror $(TOOLS)/gp-pack.c -o $(BUILD)/gp-pack
	$(BUILD)/gp-pack $(BUILD)/apps/paint.gp "Paint" "paint"
	$(BUILD)/gp-pack $(BUILD)/apps/files.gp "Files" "files"
	$(BUILD)/gp-pack $(BUILD)/apps/calculator.gp "Calculator" "calculator"
	$(BUILD)/gp-pack $(BUILD)/apps/terminal.gp "Terminal" "terminal"
	$(BUILD)/gp-pack $(BUILD)/apps/settings.gp "Settings" "settings"
	$(BUILD)/gp-pack $(BUILD)/apps/browser.gp "Browser" "browser"
	$(BUILD)/gp-pack $(BUILD)/apps/snake.gp "Snake" "snake"
	$(BUILD)/gp-pack $(BUILD)/apps/shooter.gp "2D Shooter" "shooter"
	$(BUILD)/gp-pack $(BUILD)/apps/notepad.gp "Notepad" "notepad"
	cp terminal/welcome.trm $(BUILD)/terminal/welcome.trm

iso: $(BUILD)/gp-os.img | $(BUILD)
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot
	cp $(BUILD)/gp-os.img $(BUILD)/iso/boot/gp-os.img
	printf '%s\n' 'GitrexOS boot media — see /boot/gp-os.img' > $(BUILD)/iso/README.TXT
	$(XORRISO) -as mkisofs -quiet -V GITREXOS -b boot/gp-os.img \
		-c boot/boot.catalog -no-emul-boot -boot-load-size 2880 \
		-o $(BUILD)/gp-os.iso $(BUILD)/iso

installer: $(BUILD)/gp-os.img $(TOOLS)/installer.sh | $(BUILD)
	cp $(TOOLS)/installer.sh $(BUILD)/gp-os-installer.sh
	chmod +x $(BUILD)/gp-os-installer.sh

run: all
	qemu-system-i386 -m 32M -drive format=raw,file=$(BUILD)/gp-os.img \
		-device rtl8139,netdev=n0 -netdev user,id=n0

clean:
	rm -rf $(BUILD)