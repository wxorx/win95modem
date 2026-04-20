
git clone --recursive --branch v5.0.5 https://github.com/espressif/esp-idf.git esp-idf-v5.0.5-ppp

patch -p0 < ../esp-idf.patch
cd components/lwip/lwip
patch -p0 < ../../../../lwip.patch

./install.sh
. ./activate.sh
./build.sh
