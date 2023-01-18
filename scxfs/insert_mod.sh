#!/bin/sh

make -j64
make install
sudo modprobe -v scxfs

#sudo mount /dev/md127 /mnt/test
sudo mount -v -t scxfs /dev/md127 /mnt/test
