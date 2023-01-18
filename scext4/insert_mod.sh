#!/bin/sh

make -j64
make install
sudo modprobe -v jbd3
sudo modprobe -v scext4
sudo mount -v -t scext4 /dev/md127 /mnt/test
