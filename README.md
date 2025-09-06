# Pipecat ESP32 Client SDK - Bern Technologies

## 💻 Platform/Device Support

This SDK has been developed and tested only on a `esp32s3`.

## 📋 Pre-requisites

Clone this repository:

```
git clone --recursive https://github.com/japaneil/pipecat-esp32.git
```

Install the ESP-IDF toolchain following these
[instructions](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/linux-macos-setup.html).

After that, just open a terminal and load ESP-IDF tools:

```
source PATH_TO_ESP_IDF/export.sh
```
Or open the ESP-IDF CMD and cd into the `esp32-s3-box-3` directory.

We also need to set a few environment variables before we can build:

```
export WIFI_SSID=foo
export WIFI_PASSWORD=bar
export PIPECAT_SMALLWEBRTC_URL=URL (e.g. http://192.168.1.10:7860/api/offer)
```

where `WIFI_SSID` and `WIFI_PASSWORD` are just needed to connect the device to
the network. `PIPECAT_SMALLWEBRTC_URL` is the URL endpoint to connect to your
Pipecat bot.

## 🛠️ Build

Go inside the `esp32-s3-box-3` directory.

The first thing to do is to set the desired target, for example:

```
idf.py --preview set-target esp32s3
```

Then, just build:

```
idf.py build
```

## 🔌 Flash the device

```
idf.py -p /dev/ttyACM0 flash
```

where `/dev/ttyACM0` is the device where your ESP32 is connected. You can run
`sudo dmesg` to know the device on your system.

On Debian systems, you will want to add your user to the `dialout` group so you
don't need root access.

```
idf.py flash
```
