The script is downloaded onto the ESP32-S3 board by using Arduino IDE, with ESP32 extension installed (or ESP-IDF loader)

On Linux, you might need to obtain correct permissions:

`sudo usermod -a -G dialout $USER`

and then re-start the session to be able to actually download the sketch onto the device with USB bootloader

Also, you might need to force the ESP32 into bootloader mode as follows:

- Press and hold the BOOT button on the XIAO.
- While holding BOOT, press and release the RESET button.
- Release the BOOT button.