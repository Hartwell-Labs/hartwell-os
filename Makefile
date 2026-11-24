CC      := gcc
CFLAGS  := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mstackrealign -fno-asynchronous-unwind-tables \
           -Wall -Wextra -O2 -std=gnu99
LDFLAGS := -m32 -ffreestanding -nostdlib -no-pie -Wl,-T,src/linker.ld

OBJS := build/boot.o build/kernel.o

all: hartwell-os.bin

build:
	mkdir -p build

build/boot.o: src/boot.S | build
	$(CC) -m32 -c $< -o $@

build/kernel.o: src/kernel.c | build
	$(CC) $(CFLAGS) -c $< -o $@

hartwell-os.bin: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	@echo "built: $$(wc -c < $@) bytes"

clean:
	rm -rf build hartwell-os.bin

.PHONY: all clean
