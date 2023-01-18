#!/bin/sh

echo 'unmounting /mnt/test ....'
sudo umount -v /mnt/test

sudo modprobe -r -v scext4
sudo modprobe -r -v jbd3

echo 'ScaleCache ext4 removed.'
