# fq with MinGW gcc (MSYS2). For MSVC, run build.bat.
#   make         build/fq.exe
#   make test    unit tests, then the end-to-end tests

CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic
SRC    = main.c walk.c match.c regex.c out.c

# -municode: the entry point is wmain, so arguments arrive as UTF-16
build/fq.exe: $(SRC) fq.h
	@mkdir -p build
	$(CC) $(CFLAGS) -municode -o $@ $(SRC)

build/unit.exe: test/unit.c match.c regex.c fq.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. -o $@ test/unit.c match.c regex.c

test: build/fq.exe build/unit.exe
	./build/unit.exe
	powershell -NoProfile -ExecutionPolicy Bypass -File test/run.ps1 build/fq.exe

clean:
	rm -rf build

.PHONY: test clean
