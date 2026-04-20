
git clone --recursive --branch v5.0.5 https://github.com/espressif/esp-idf.git esp-idf-v5.0.5-ppp

patch -p0 < ../esp-idf.patch
cd components/lwip/lwip
patch -p0 < ../../../../lwip.patch

./install.sh
. ./activate.sh
./build.sh


To exit the IDF Monitor, press "Ctrl+]" on your keyboard. If that doesn't work, you may need to try "Ctrl+AltGr+9" as an alternative.
