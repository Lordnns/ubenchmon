CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -fPIC -march=native -I include
LDFLAGS := -shared
VERSION ?= 1.0.0

SRC     := src/utils.c src/setup.c src/monitor.c src/verify.c
OBJ     := $(SRC:.c=.o)
TARGET  := libubenchmon.so
STATIC  := libubenchmon.a

.PHONY: all clean install deb

all: $(TARGET) $(STATIC)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(STATIC): $(OBJ)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET) $(STATIC) *.deb
	rm -rf ubenchmon_deb

install: $(TARGET) $(STATIC)
	install -d /usr/local/lib /usr/local/include
	install -m 644 $(TARGET) /usr/local/lib/
	install -m 644 $(STATIC) /usr/local/lib/
	install -m 644 include/ubenchmon.h /usr/local/include/
	ldconfig

deb: all
	mkdir -p ubenchmon_deb/usr/local/lib
	mkdir -p ubenchmon_deb/usr/local/include
	mkdir -p ubenchmon_deb/usr/local/bin
	mkdir -p ubenchmon_deb/DEBIAN
	cp $(TARGET) ubenchmon_deb/usr/local/lib/
	cp $(STATIC) ubenchmon_deb/usr/local/lib/
	cp include/ubenchmon.h ubenchmon_deb/usr/local/include/
	-cp tui/target/release/ubenchmon ubenchmon_deb/usr/local/bin/ 2>/dev/null || true
	echo "Package: ubenchmon\nVersion: $(VERSION)\nArchitecture: amd64\nMaintainer: Lordnns\nDescription: Latency-sensitive benchmark monitor library and TUI\n" > ubenchmon_deb/DEBIAN/control
	dpkg-deb --build ubenchmon_deb ubenchmon.deb
	rm -rf ubenchmon_deb
