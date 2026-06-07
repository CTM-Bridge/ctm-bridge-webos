# CTM Bridge Test

Native webOS POC app for the CTM TCP bridge.

The package also installs `ctm_usbipd_webos`, a separate pure USB/IP exporter
for devices physically plugged into the TV. It does not use the CTM TCP bridge.

Philosophy:

- webOS owns only the physical HID point: hidraw open/read/write, raw feature get/set execution requested by Windows, and local output pacing.
- Windows CTM owns the map runtime and all USB-to-BT report translation.
- The TCP protocol carries complete raw HID reports plus small metadata.
- The app grabs matching `/dev/input/event*` nodes with `EVIOCGRAB` so the DS5 does not also drive webOS UI controls such as volume.
- Controller input runs on its own hidraw reader thread with a 1 ms poll cadence and drains available reports. For the current latency test, exact duplicate reports are counted but still sent to Windows; output/audio pacing and feature requests stay on the main bridge loop.

Default runtime:

```sh
ctm_bridge_test
```

Built-in defaults are `host=192.168.0.200`, `port=48055`, Sony vendor `vid=0x054c`, known Sony controller products, `bt_address=a0:fa:9c:26:ac:d4`, and `any_hid=0` for the current POC network. Stale config `any_hid` is ignored, LG vendor `0x005d` is rejected, Luna `device/getStatus` is used for DualSense addresses, and Bluetooth `stopSniff` is sent before/after opening the controller. A small SDL status UI shows connection state, controller path, evdev grab count, 5-second input profiler rates, feature failures, output failures, and paced queue depth.

Config can also be supplied through:

```text
CTM_BRIDGE_HOST
CTM_BRIDGE_PORT
CTM_BRIDGE_HID
CTM_BRIDGE_BT_ADDRESS
CTM_BRIDGE_ANY_HID
```

or `/tmp/ctm-bridge-test.conf` / app-directory `test.conf`:

```ini
host=192.168.1.23
port=48055
path=/dev/hidraw2
bt_address=a0:fa:9c:26:ac:d4
vid=0x054c
pid=0x0ce6
any_hid=0
```

Build:

```sh
scripts/build_ipk_macos.sh
```

Windows 11 build:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_ipk_windows.ps1
```

Preferred Windows-hosted build through WSL:

```powershell
wsl -- bash -lc "cd '/mnt/d/Work/CMU/ctm-bridge-test-webos' && WORK_DIR=/tmp/ctm-bridge-test-webos-build SDK=\$HOME/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot bash ./scripts/build_ipk_macos.sh"
```

The Windows script auto-installs missing `cmake`/`ninja` with Chocolatey when needed. Use `-NoInstallPrereqs` to disable that behavior.
It also auto-detects the Beanviser-bundled `ares-package.cmd` when Beanviser is next to this project in the workspace.

If the webOS SDK/NDK is not in a standard location, pass paths explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_ipk_windows.ps1 `
  -AresPackage "C:\webOS_TV_SDK\CLI\bin\ares-package.cmd" `
  -ToolchainFile "C:\webos-sdk\arm-webos-linux-gnueabi_sdk-buildroot\share\buildroot\toolchainfile.cmake"
```

If you have a local webOS SDK installer, the script can run it and re-detect the CLI/toolchain:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_ipk_windows.ps1 `
  -WebOsSdkInstaller "C:\path\to\webOS_TV_SDK_Installer.exe"
```

Required macOS/Linux host tools: `cmake`, `cpack`, `ares-package`, `rsync`, `pkg-config`, SDL2, SDL2_ttf, and the webOS SDK toolchain.

Required Windows host tools: `cmake`, `cpack`, `ninja`, `ares-package`, SDL2/SDL2_ttf for the target toolchain, and the webOS native SDK/NDK toolchain.
Beanviser includes the webOS Ares packaging CLI, but not the native SDK/NDK compiler toolchain.

Pure USB/IP exporter:

```sh
/media/developer/apps/usr/palm/applications/com.local.ctmbridge/bin/ctm_usbipd_webos --list
/media/developer/apps/usr/palm/applications/com.local.ctmbridge/bin/ctm_usbipd_webos --port 3240
```

From Windows, attach a listed TV bus id:

```powershell
& "C:\Program Files\USBip\usbip.exe" list -r <tv-ip>
& "C:\Program Files\USBip\usbip.exe" attach -r <tv-ip> -b <busid>
```
