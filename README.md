This MCU build is a work in progress.  Cheat functionality works as intended (some Gameshark codes online are flaky and don't work), but still have some visual glitches (only when the OSD is open).  Also, with the cheats, sometimes switching between games with a reset button on a flash cart messes with functionality, so it's worth making a habit of turning a code OFF in the OSD before switching in case that has anything to do with it.

I encourage anyone using this firmware update to keep this in mind and PLEASE let me know issues that may arise as I'm the only one working on this and definitely use a "happy path" method to get things working that could use extensive bug testing.

I PROMISE I won't be offended at all and want as much feedback as I can get to hopefully improve the build for anyone using it.

Added functionality:

Cheats tab in OSD
Dynamic Palette Switch between palettes in GB and warmness in GBC
BLE integration (basic to connect to Companion App for searching/syncing cheats and eventually other stuff)
Achievements Tab (mostly visual, but WILL still sync a linked profile (image/points) if loaded in the Companion App)



# Chromatic MCU
This repository houses the ModRetro Chromatic's MCU design files.

## Toolchain Setup
The project relies on the EspressIF IoT Development Framework (IDF) for development and debugging. Please set up the ESP32 according to the instructions at https://idf.espressif.com/.

### Windows
Please run the official ESP32 installer.

### Linux
The Ubuntu 24.04 steps are summarized below:
```bash
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
cd ~/path/to/clone/esp-idf
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf

# Installs to the default ~/.espressif path. See IDF_TOOLS_PATH for an alternative.
./install.sh esp32

# Update your path for your current session so that the `idf.py` tool can be accessed.
source ./export.sh
```

To permanently update your environment path, add the following to your shell startup script (e.g. `~/.bashrc`):
```
alias get_idf='. $HOME/esp-idf/export.sh'
```

where `$HOME/esp-idf` is where you cloned the ESP IDF toolchain. Anytime you wish to develop, run `get_idf` in your terminal first. EspressIF does not recommend always running `export.sh` for every terminal. This may have unintended side effects.

## Build, Flashing, and Debugging

Run `idf.py -p PORT build flash monitor` to build, flash and monitor the project. The default baud rate is `115200`.

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

Each of these steps can be issued individually (build, flash, and monitor).

## Serial Interface
The Chromatic FPGA supports a composite USB device consisting of several device classes. One of them is a serial interface used for flashing and debugging. The setup steps depend on your operating system.

### Windows
A serial port for the ESP32 should already appear in the Device Manager as a COM device.
You can also see this by issuing `modes` within a command prompt.

### Linux
Ensure you are part of the `dialout` group. You will need to log out and log back in after issuing the following command:
```bash
sudo gpasswd --add $USER dialout
```

If everything is working, you should see `/dev/ttyACM0` appear as the communications port to the MCU.

