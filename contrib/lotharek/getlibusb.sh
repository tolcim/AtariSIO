#!/bin/sh

# try to get information about libftdi installation

if [ $# -ne 1 ] ; then
	echo "usage: $0 --cflags|--libs"
	exit 1
fi

FLAGS=$(pkg-config $1 libusb-1.0 2>/dev/null)
if [ $? -eq 0 ] ; then
	echo "$FLAGS"
	exit 0
fi

# fall back to libftdi if no pkg-config
case "$1" in
  "--cflags")
    echo "-I/usr/include/libusb-1.0"
    ;;
  "--libs")
    echo "-lusb-1.0"
    ;;
  *)
    exit 1
    ;;
esac
