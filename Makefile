# lennys-benchmark - GPU + CPU benchmark
#
# Build:   make
# Run:     ./bin/lennys-benchmark
# Install: sudo make install

CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++20 -Wall -Wextra -Wno-unused-parameter -pthread -Isrc
LDFLAGS  ?= -pthread
LDLIBS   ?= -lepoxy -lX11 -lGL -ldl

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET  := bin/lennys-benchmark
SRCS    := $(wildcard src/*.cpp)
OBJS    := $(SRCS:.cpp=.o)
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean run install uninstall info icon deb appimage xbps packages

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "  built -> $@"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/lennys-benchmark
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 644 packaging/lennys-benchmark.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps
	install -m 644 packaging/icon/lennys-benchmark.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 packaging/lennys-benchmark.1 $(DESTDIR)$(PREFIX)/share/man/man1/
	@echo "  installed -> $(PREFIX)/bin/lennys-benchmark"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lennys-benchmark
	rm -f $(DESTDIR)$(PREFIX)/share/applications/lennys-benchmark.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/lennys-benchmark.png
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/lennys-benchmark.1

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
	@rmdir bin 2>/dev/null || true

info:
	@echo "compiler : $(CXX)"
	@echo "flags    : $(CXXFLAGS)"
	@echo "libs     : $(LDLIBS)"
	@echo "target   : $(TARGET)"

# ---- distribution packages ------------------------------------------------
# These rebuild a portable binary (no -march=native) inside packaging/build/.
icon:
	python3 packaging/make-icon.py

deb:
	bash packaging/build-deb.sh

appimage:
	bash packaging/build-appimage.sh

xbps:
	bash packaging/build-xbps.sh

packages:
	bash packaging/build-all.sh
