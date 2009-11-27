########################################################################
# AtariSIO Makefile
# (c) 2002-2008 by Matthias Reichl <hias@horus.com>
# http://www.horus.com/~hias/atari/
########################################################################

########################################################################
# use kernel 2.6 build system (comment out if you use kernel 2.2/2.4)
########################################################################

USE_KBUILD_26 = 1

########################################################################
# location of your kernel source (header) files
########################################################################

KDIR = /lib/modules/$(shell uname -r)/build
#KDIR = /usr/src/linux
#KDIR = /data/hias/laptop/linux-2.2.19

########################################################################
# installation directory for the kernel module atarisio.o/atarisio.ko
# (lib/modules/$KERNELVERSION/misc)
# please note: some RedHat versions ship with a nonstandard version.h
# file which causes grep to fail in getting the version information.
# In this case, uncomment either the 'uname' line below or set the
# version directory by hand.
########################################################################

VERSION = $(shell ./getver.sh "${KDIR}")
#VERSION = $(shell uname -r)
#VERSION = 2.4.25

MDIR = /lib/modules/$(VERSION)/misc

########################################################################
# installation directory
# The executables will be installed in INST_DIR/bin,
# the header file atarisio.h will be installed in INST_DIR/include,
########################################################################

INST_DIR = /usr/local
#INST_DIR = /usr/local/atarisio
#INST_DIR = /opt/hias

########################################################################
# set compile and link options for using the ncurses library
# the wrapper script uses "ncurses5-config" if available
########################################################################

NCURSES_CFLAGS=$(shell ./getcurses.sh --cflags)
NCURSES_LDFLAGS=$(shell ./getcurses.sh --libs)


########################################################################
# if you don't have zlib installed on your system, please comment out
# the following two lines. With zlib enabled you will be able to
# directly load and save gzip-compressed image files (eg file.atr.gz)
########################################################################

ZLIB_CFLAGS=-DUSE_ZLIB
ZLIB_LDFLAGS=-lz


########################################################################
# ATP support:
# if you don't want ATP support, comment out the following line
########################################################################

ENABLE_ATP=1

########################################################################
# all in one:
# link all programs together into a single executable (like busybox)
########################################################################

#ALL_IN_ONE=1

########################################################################
# gcc version for compiling the user space apps
# Usually you don't need to change this
########################################################################

CC ?= gcc
CXX ?= g++
#CC = gcc-3.3
#CXX = g++-3.3
#CC = /usr/local/gcc-3.2.2/bin/gcc
#CXX = /usr/local/gcc-3.2.2/bin/g++


########################################################################
# gcc version to use for compiling the kernel module
# The (major) version number must match the version that was used
# when compiling the linux kernel, otherwise your system may crash!
# To find out which gcc version was used to compile the kernel
# just do a "cat /proc/version"
########################################################################

KERNEL_CC ?= $(CC)
#KERNEL_CC = gcc
#KERNEL_CC = gcc-2.95
#KERNEL_CC = kgcc

########################################################################
# don't change anything below here
########################################################################

MODFLAGS = -Wstrict-prototypes -Wall -O2 -DMODULE -D__KERNEL__ -I$(KERNEL_INCLUDES)

CFLAGS = -g -W -Wall -DATARISIO_DEBUG 
#CFLAGS = -O3 -W -Wall
CXXFLAGS = $(CFLAGS)

LDFLAGS = -g
#LDFLAGS = -g -static

export KERNEL_CC MODFLAGS KDIR MDIR USE_KBUILD_26
export CC CXX CFLAGS CXXFLAGS LDFLAGS
export INST_DIR
export ENABLE_ATP ALL_IN_ONE
export ZLIB_CFLAGS ZLIB_LDFLAGS
export NCURSES_CFLAGS NCURSES_LDFLAGS

all:
	@echo "CC = $(CC) CXX=$(CXX) KERNEL_CC=$(KERNEL_CC)"
	$(MAKE) -C driver
	$(MAKE) -C tools

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tools clean

allclean:
	$(MAKE) -C driver allclean
	$(MAKE) -C tools allclean

backup:
	tar zcf bak/SIO-`date '+%y%m%d-%H%M'`.tgz \
	driver/*.c driver/*.h driver/Makefile \
	tools/*.h tools/*.cpp tools/Makefile tools/.depend \
	tools/6502/Makefile tools/6502/*.c tools/6502/*.h \
	tools/6502/*.src tools/6502/*.inc tools/6502/*.bin \
	windll/Makefile windll/libatarisio.cpp windll/libatarisio.h \
	windll/buildlib.bat windll/aconv.c \
	Makefile atarisio-ttyS* Changelog README* INSTALL* LICENSE getver.sh

install:
	$(MAKE) -C driver install
	$(MAKE) -C tools install

devices:
	mknod -m 660 /dev/atarisio0 c 10 240
	mknod -m 660 /dev/atarisio1 c 10 241
	mknod -m 660 /dev/atarisio2 c 10 242
	mknod -m 660 /dev/atarisio3 c 10 243

uninstall:
	$(MAKE) -C driver uninstall
	$(MAKE) -C tools uninstall
	rm -f /dev/atarisio*

dep:
	$(MAKE) -C driver dep
	$(MAKE) -C tools dep

