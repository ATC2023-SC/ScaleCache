#!/bin/sh

echo 'unmounting /mnt/test ....'
sudo umount -v /mnt/test

sudo modprobe -r -v scxfs

echo 'ScaleCache-xfs removed.'
