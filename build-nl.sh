#!/bin/bash
# Build the Dutch TamaPoke and flash ONLY the app partition (keeps the save in
# NVS and the sprites on the SD card).
#   bash build-nl.sh            # build + flash /dev/ttyACM0
#   bash build-nl.sh --no-flash # build only
set -e
cd "$(dirname "$0")"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB"
URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"

if ! arduino-cli core list | grep -q "^esp32:esp32 *3\.3\.12"; then
  arduino-cli core update-index --additional-urls "$URL"
  arduino-cli core install esp32:esp32@3.3.12 --additional-urls "$URL" ||
    arduino-cli core install esp32:esp32 --additional-urls "$URL"
fi
for lib in "GFX Library for Arduino" SensorLib XPowersLib; do
  arduino-cli lib list | grep -q "^${lib// /_}\|^$lib" || arduino-cli lib install "$lib"
done

arduino-cli compile --fqbn "$FQBN" --output-dir build-nl .
ls -la build-nl/TamaPoke.ino.bin

[ "$1" = "--no-flash" ] && exit 0
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0x10000 build-nl/TamaPoke.ino.bin ||
  ~/.arduino15/packages/esp32/tools/esptool_py/*/esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0x10000 build-nl/TamaPoke.ino.bin
