| Supported Targets | ESP32-C3 |
| ----------------- | -------- |

# InfluxDB Thermometer

The InfluxDB thermometer is an ESP32C3 SoC with an MCP9808 thermometer.

* If necessary it opens a configuration on http://192.168.4.1 to set the Wifi credentials (SSID and password) with component elrebo-de/wifi-manager.
* It connects to Wifi with component elrebo-de/wifi-manager.
* If necessary it starts a configuration server on http://<ip-addr_of_esp_soc>/config to set the configuration parameters for the InfluxDB and for the temperature calibration.
* It synchronizes time with an NTP server with component elrebo-de/time_sync.
* It starts an I2C bus to connect with the MCP9808 with component elrebo-de/i2c_master.
* It measures the temperature with the MCP9808 with component elrebo-de/i2c_master.
* It sends the temperature to an InfluxDB server with component elrebo-de/InfluxDB-Client-for-ESP-IDF.
* It goes to DeepSleep for 15 minutes with component elrebo-de/deep_sleep.
* It wakes up after 15 minutes and starts the process again.

## How to use the program

### Hardware Required

* One of these ESP32-C3 SoCs:
  * WAVESHARE_ESP32_C3_ZERO
  * ESP32C3_SUPERMINI
  * ESP32C3_72X40OLED
* An MCP9808 thermometer board.

### Build and Flash

Run `idf.py set-target esp32c3` to set the build target.

Run `idf.py menuconfig` to set the SoC. Go to `Target SoC Board Configuration -> Select Target SoC Board` and select the right SoC Board for your build. Save your changes.

Run `idf.py build` to build the project.

Run `idf.py -b 115200 flash monitor` to flash and monitor the project.

(To exit the serial monitor, type ``Ctrl-]`` or `Ctrl-Option-6`on a Mac.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

### Configure the Project

#### menuconfig
The only configuration needed for compilation is to select the Target SoC Board in menuconfig (see above).

#### Wi-Fi connection
The parameters for the Wi-Fi connection are set in a captive portal.

When the system is started, the program looks for Wi-Fi credentials in the non-volatile flash storage (nvsflash). 
If there are no credentials found, configuration mode is started. 

In configuration mode, the system creates a Wi-Fi access point on IP address 192.178.4.1. 
Look for a WLAN named "ESP32-xxxx" (where xxxx is specific for your SoC) and connect to it.

Now you choose the right WLAN from a list of available WLANs and enter the corresponding password.
WLAN name and password are now stored in nvsflash storage.

#### Other configuration parameters

The parameters

* for connecting the InfluxDB,
* for setting the SCL and SDA pins for the thermometer device and
* for calibrating the thermometer

are stored in nvsflash storage as well.

They have to be set on a web page presented by a HTTP server on the ESP SoC.

## Example Output

((tbc))

```

((tbc))

```

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We will get back to you soon.
