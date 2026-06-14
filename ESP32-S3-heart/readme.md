Update: I decided to use ESP32-S3 to read the HR from the Polar H9 sensor as that's easier to wear. Plus, it will sample the data from just one analog ADXL335 and report all of it via the
I2C bus.
Raspberry Pi 5 will read the values from it on the shared I2C bus in its own time.

https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/

This is for Analog Devices HR sensor I might use in future, but it needs more wires, so more difficult to wear.
https://www.analog.com/media/en/technical-documentation/data-sheets/ad8232.pdf