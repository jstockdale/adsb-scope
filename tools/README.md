# tools/ – for the PC that the device is plugged into

| file | what it does | needs |
| --- | --- | --- |
| `adsb-fastlog.py` | logs the console to a file at a speed negotiated with the device (921600 by default; firmware before coh4 is logged at 115200). `--probe B` runs the device's speed-measurement diagnostic. | Python 3.6+, Linux or macOS |
| `flash-adsb-scope.sh` | flashes a firmware kit with esptool: reads the device's partition table first and keeps the NVS (settings, Meshtastic identity) whenever the layout allows; `--dry-run` shows what it would do | bash, esptool (any 4.x or 5.x, also ESP-IDF's) |

Both check what they need before they touch the port, and say what is
missing. `docs/UPLINK.md` section 9.8 has the console-speed protocol.

```sh
python3 adsb-fastlog.py --port /dev/ttyACM0            # Ctrl+C to stop
bash flash-adsb-scope.sh --port /dev/ttyACM0 --dry-run
```
