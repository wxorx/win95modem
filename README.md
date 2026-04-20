
git clone --recursive --branch v5.0.5 https://github.com/espressif/esp-idf.git esp-idf-v5.0.5-ppp

patch -p0 < ../esp-idf.patch
cd components/lwip/lwip
patch -p0 < ../../../../lwip.patch

./install.sh
. ./activate.sh
./build.sh


To exit the IDF Monitor, press "Ctrl+]" on your keyboard. If that doesn't work, you may need to try "Ctrl+AltGr+9" as an alternative.


qemu-system-x86_64 \
 -name win95 \
 -machine pc \
 -cpu pentium2 \
 -smp 1 \
 -m 128M \
 -rtc base=utc \
 -drive file=/Users/user/.vm/win95.qcow2,if=ide,index=0,media=disk,format=qcow2 \
 -vga vmware \
 -usb -device usb-tablet \
 -chardev serial,path=/dev/cu.usbserial-A5069RR4,id=serial1 \
 -device isa-serial,chardev=serial1,index=1

