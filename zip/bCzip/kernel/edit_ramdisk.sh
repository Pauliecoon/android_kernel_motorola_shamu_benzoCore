#!/sbin/sh
#ramdisk_gov_sed.sh by show-p1984
#Features:
#extracts ramdisk
#finds busbox in /system or sets default location if it cannot be found
#add init.d support if not already supported
#removes forced encryption
#disables mpdecision
#repacks the ramdisk

mkdir /tmp/ramdisk
cp /tmp/boot.img-ramdisk.gz /tmp/ramdisk/
cd /tmp/ramdisk/
gunzip -c /tmp/ramdisk/boot.img-ramdisk.gz | cpio -i
cd /

#remove cmdline parameters we do not want
#maxcpus=2 in this case, which limits smp activation to the first 2 cpus
echo $(cat /tmp/boot.img-cmdline | sed -e 's/maxcpus=[^ ]\+//')>/tmp/boot.img-cmdline

#Don't force encryption
if  grep -qr forceencrypt /tmp/ramdisk/fstab.shamu; then
   sed -i "s/forceencrypt/encryptable/" /tmp/ramdisk/fstab.shamu
fi

#add init.d support if not already supported
#this is no longer needed as the ramdisk now inserts our modules, but we will
#keep this here for user comfort, since having run-parts init.d support is a
#good idea anyway.
found=$(find /tmp/ramdisk/init.rc -type f | xargs grep -oh "run-parts /system/etc/init.d");
if [ "$found" != 'run-parts /system/etc/init.d' ]; then
        #find busybox in /system
        bblocation=$(find /system/ -name 'busybox')
        if [ -n "$bblocation" ] && [ -e "$bblocation" ] ; then
                echo "BUSYBOX FOUND!";
                #strip possible leading '.'
                bblocation=${bblocation#.};
        else
                echo "BUSYBOX NOT FOUND! init.d support will not work without busybox!";
                echo "Setting busybox location to /system/xbin/busybox! (install it and init.d will work)";
                #set default location since we couldn't find busybox
                bblocation="/system/xbin/busybox";
        fi
	#append the new lines for this option at the bottom
        echo "" >> /tmp/ramdisk/init.rc
        echo "service userinit $bblocation run-parts /system/etc/init.d" >> /tmp/ramdisk/init.rc
        echo "    oneshot" >> /tmp/ramdisk/init.rc
        echo "    class late_start" >> /tmp/ramdisk/init.rc
        echo "    user root" >> /tmp/ramdisk/init.rc
        echo "    group root" >> /tmp/ramdisk/init.rc
fi

#copy fstab
cp /tmp/fstab.shamu /tmp/ramdisk/fstab.shamu
chmod 750 /tmp/ramdisk/fstab.shamu

#copy custom init.shamu.power.rc
cp /tmp/init.shamu.power.rc /tmp/ramdisk/init.shamu.power.rc
chmod 750 /tmp/ramdisk/init.shamu.power.rc

#copy custom init.shamu.rc
cp /tmp/init.shamu.rc /tmp/ramdisk/init.shamu.rc
chmod 750 /tmp/ramdisk/init.shamu.rc

#copy init.benzo.rc
cp /tmp/init.benzo.rc /tmp/ramdisk/init.benzo.rc
chmod 750 /tmp/ramdisk/init.benzo.rc

#copy benzo-post-boot
mkdir /tmp/ramdisk/sbin
cp /tmp/benzo-post-boot.sh /tmp/ramdisk/sbin/benzo-post-boot.sh
chmod 750 /tmp/ramdisk/sbin/benzo-post-boot.sh

rm /tmp/ramdisk/boot.img-ramdisk.gz
rm /tmp/boot.img-ramdisk.gz
cd /tmp/ramdisk/
find . | cpio -o -H newc | gzip > ../boot.img-ramdisk.gz
cd /
rm -rf /tmp/ramdisk

