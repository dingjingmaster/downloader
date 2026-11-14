.PHONY: all packages build test rootfs emerge deb rpm install-gentoo clean distclean

osArch                   := $(shell uname -m)
CC                       ?= gcc
CXX                      ?= g++
STRIP                    ?= strip
JOBS                     ?= $(shell nproc 2>/dev/null || echo 4)
PACK_DIR                 := $(abspath .)
PROJECT_DIR              := $(abspath ..)
BUILD_DIR                := $(PACK_DIR)/build/downloader
VERSION                  := $(shell awk '/^project/ { print $$3; exit }' $(PROJECT_DIR)/CMakeLists.txt)
INSTALL_PREFIX           := /opt/downloader

ifeq ($(osArch),x86_64)
osArch                    = amd64
else ifeq ($(osArch),aarch64)
osArch                    = arm64
endif

ifeq ($(filter $(osArch),amd64 arm64),)
$(error "Invalid arch: $(osArch)")
endif

ifeq ($(strip $(VERSION)),)
$(error "Cannot parse project version from $(PROJECT_DIR)/CMakeLists.txt")
endif

$(info "Current arch: $(osArch)")
$(info "Current version: $(VERSION)")

all: packages
	@echo "Build all packages!"

build:
	@echo Arch: ${osArch}, CC: ${CC}, CXX: ${CXX}
	@cmake -S "$(PROJECT_DIR)" -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$(INSTALL_PREFIX)" \
		-DCMAKE_C_COMPILER="$(CC)" \
		-DCMAKE_CXX_COMPILER="$(CXX)" \
		-DDOWNLOADER_BUILD_TESTS=OFF
	@cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"

test:
	@# Test targets keep assertions enabled through -UNDEBUG in CMakeLists.txt.
	@cmake -S "$(PROJECT_DIR)" -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$(INSTALL_PREFIX)" \
		-DCMAKE_C_COMPILER="$(CC)" \
		-DCMAKE_CXX_COMPILER="$(CXX)" \
		-DDOWNLOADER_BUILD_TESTS=ON
	@cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"
	@ctest --test-dir "$(BUILD_DIR)" --output-on-failure

rootfs: build
	@STRIP="$(STRIP)" sh src/build-rootfs.sh "$(osArch)" "$(VERSION)"

packages: emerge deb rpm

emerge: rootfs
	@sh src/build-emerge.sh "$(osArch)" "$(VERSION)"

install-gentoo:
	@sh tools/install-gentoo.sh

deb: rootfs
	@sh src/build-deb.sh "$(osArch)" "$(VERSION)"

rpm: rootfs
	@sh src/build-rpm.sh "$(osArch)" "$(VERSION)"

clean:
	@rm -rf "$(PACK_DIR)/build"

distclean: clean
	@rm -rf "$(PACK_DIR)/dist"
