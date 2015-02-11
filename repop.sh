#!/bin/bash

sudo service cachefilesd stop
sudo umount /mnt
sudo rm -f /atimes*
sudo rm -f /var/cache/xfscache/atimes*
sudo service cachefilesd start
sudo mount -t nfs4 -o fsc localhost:/home/jsnow/src/linux /mnt
md5sum /mnt/*
sudo service cachefilesd stop
#sudo ./cachefilesd -dd -c -n -s -F
