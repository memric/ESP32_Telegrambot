ESP-IDF Telegram Bot
====================

Telegram bot project based on ESP-IDF. Simple echo bot implementation with markup test.

ESP32 Devboard can be used for this project.

### Set up and configure the project

* Install and setup ESP-IDF. Run project configuration:

```
idf.py set-target esp32
idf.py menuconfig
```

* Set WiFi SSID & Password in *Project configuration* section.

* Set Telegram bot Token.

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py build
```

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Get Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.