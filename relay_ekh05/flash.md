<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# Flashing the EKH05 relay

The MM6108-EKH05 has an on-board ST-LINK/V3. It gives you SWD for programming, a USB virtual
COM port for the console (the STM32U585 LPUART1 at 115200 8N1), and on most ST-LINK/V3
firmware a USB mass-storage drive for drag-and-drop programming. This page covers three
workflows. None of them installs anything on the Mac.

| Workflow | Host | Programs | Config store |
|---|---|---|---|
| [A. Drag and drop](#a-drag-and-drop-macos-no-tools) | macOS (or any OS) | `.bin` copied to the ST-LINK drive | reset to empty, or set from `config.hjson` with `bin-with-config` |
| [B. OpenOCD in Docker](#b-openocd-in-docker-linux-host-with-usb-passthrough) | Linux host (Docker can reach USB) | `.elf` over SWD | left as it is, written with `make config` |
| [C. Remote OpenOCD](#c-remote-openocd-config-store-from-the-mac) | board on a Linux machine, tools on the Mac | `.elf` over SWD | written over the network |

Build first (on any host with Docker):

```sh
relay_ekh05/build.sh --revert-patches    # build/relay_ekh05.{elf,bin,hex}, submodule left clean
```

Build only through `build.sh`: it applies the SDK patches before compiling, and the firmware
needs patch 0001. Every flashing step below uses the files it produced and never rebuilds.
`make flash` refuses to run if `build/relay_ekh05.elf` is missing, and any direct `make` that
would relink (for example `make bin` after `--revert-patches`) stops with an error while the
patches are not applied, unless you pass `ALLOW_UNPATCHED=1`.

## Flash layout (STM32U585, 2 MiB)

| Address | Size | Contents |
|---|---|---|
| `0x08000000` | 16 KiB | BOOTLOADER: an 8-byte reset-vector stub (the CPU boots here) |
| `0x08004000` | 16 KiB | CONFIG: mmconfig config store, two 8 KiB partitions |
| `0x08008000` | 1120 KiB | Application (vector table, code, MM6108 firmware, BCF) |
| `0x08120000` | 896 KiB | FILESYSTEM (unused by this firmware) |

The firmware links in the MM6108 firmware and the BCF (`bcf_mf08651_us.mbin`), and it has all
the lab defaults compiled in. **An empty or erased config store is a valid configuration.** The
SDK examples load the BCF from the config store instead, and that is the main reason they need
`program-configstore.py` before first use. This firmware does not.

## A. Drag and drop (macOS, no tools)

1. Connect the EKH05's ST-LINK USB port. A drive appears in Finder. On the board tested on
   2026-11-08 it was `/Volumes/STLINK_V3P`, holding only `DETAILS.TXT` (ST-LINK firmware
   `V3J8M3B5S1`), with USB ID `0483:374F` and the virtual COM port at
   `/dev/cu.usbmodem<location>03`.
   - If no drive appears on another board, check *System Information -> USB* for the ST-LINK
     product ID. `0x374E` and `0x374F` include mass storage, but `0x3754` ("no MSD") does not.
     ST's firmware updater can turn the drive on, but it is an install, so if there is no
     drive, use workflow B or C instead.
2. Copy the raw image without macOS metadata (`-X` skips extended attributes):

   ```sh
   cp -X relay_ekh05/build/relay_ekh05.bin /Volumes/<ST-LINK drive>/
   ```

   The ST-LINK programs the file at `0x08000000`, resets the target and remounts the drive. A
   `FAIL.TXT` file on the drive means programming failed: read it, then try again while holding
   the board's reset button during the copy. Finder may warn that the disk was not ejected
   properly. You can ignore that warning.
3. Watch the console (see [Serial console](#serial-console-macos)).

**What happens to the config store.** `relay_ekh05.bin` covers `0x08000000` up to the end of
the application, including the CONFIG region, which the build fills with `0xFF`. Dragging it
therefore **erases the config store**, and the firmware runs on its compiled-in defaults.

To flash with config store overrides, put them in `relay_ekh05/config.hjson`, then:

```sh
relay_ekh05/build.sh --revert-patches bin-with-config
cp -X relay_ekh05/build/relay_ekh05_with_config.bin /Volumes/<ST-LINK drive>/
```

`bin-with-config` runs the SDK's `program-configstore.py -o` (offline mode, no device needed)
to turn `config.hjson` into a config store partition. It writes that partition at offset
`0x4000` of the image, which is partition 0 at `0x08004000`. The keys it wrote are printed
during the build. With every key in `config.hjson` commented out, the partition is empty and
valid.

`relay_ekh05.hex` contains only the loaded sections, so it would leave the config store alone.
Some ST-LINK firmware versions accept Intel HEX on the drive as well as `.bin`. If yours writes
a `FAIL.TXT` for the `.hex`, use the `.bin`.

## B. OpenOCD in Docker (Linux host with USB passthrough)

Docker Desktop on macOS cannot pass USB devices into containers, so this workflow needs a Linux
host, for example a lab PC or a Raspberry Pi 4/5 (the image builds for arm64 too). The
`ekh05-builder:2.14.3` image contains xPack OpenOCD 0.12.0-2 and the Python packages that
`program-configstore.py` needs.

```sh
# from the repository root, board connected to this Linux machine
docker run --rm -it --privileged -v /dev/bus/usb:/dev/bus/usb -v "$PWD":/work \
    ekh05-builder:2.14.3 make -C relay_ekh05 flash
```

`make flash` programs the existing `build/relay_ekh05.elf` and does not rebuild it, so build
with `relay_ekh05/build.sh` first. It runs the same command as the SDK Getting Started guide:

```sh
openocd -c "set MM_PROGRAM 1" \
    -f third_party/mm-iot-sdk/framework/src/platforms/mm-mm6108-ekh05/openocd.cfg \
    -c "program relay_ekh05/build/relay_ekh05.elf verify reset exit"
```

Programming the ELF only writes the bootloader stub and the application sections, so the config
store is not touched.

Writing the config store over OpenOCD (`make config` starts OpenOCD, runs
`program-configstore.py -H localhost -p mm-mm6108-ekh05 -d write-json relay_ekh05/config.hjson`
and stops OpenOCD again):

```sh
docker run --rm -it --privileged -v /dev/bus/usb:/dev/bus/usb -v "$PWD":/work \
    ekh05-builder:2.14.3 make -C relay_ekh05 config
```

Other config store operations run the same way. Start OpenOCD in one container, then run the
tool in a second shell in the same container:

```sh
docker run --rm -it --privileged -v /dev/bus/usb:/dev/bus/usb -v "$PWD":/work \
    ekh05-builder:2.14.3 bash
# inside the container:
cd third_party/mm-iot-sdk/framework
openocd -c "set CONNECT_ASSERT_SRST 1" -f src/platforms/mm-mm6108-ekh05/openocd.cfg &
export PYTHONDONTWRITEBYTECODE=1     # keep __pycache__ out of the SDK submodule
./tools/platform/program-configstore.py -H localhost -p mm-mm6108-ekh05 -d                      # dump
./tools/platform/program-configstore.py -H localhost -p mm-mm6108-ekh05 write-string wlan.password 'new-secret'
./tools/platform/program-configstore.py -H localhost -p mm-mm6108-ekh05 delete-key wlan.password
./tools/platform/program-configstore.py -H localhost -p mm-mm6108-ekh05 erase
```

Troubleshooting:

- **The target cannot be reached.** The firmware uses tickless idle and stop mode. Add
  `-c "set CONNECT_ASSERT_SRST 1"` before `-f ...openocd.cfg`, which `make flash` already
  sets through `MM_PROGRAM`.
- **No ST-LINK found.** `openocd.cfg` only lists `hla_vid_pid 0x0483 0x374f`. If `lsusb` shows a
  different product ID (for example `0483:374e`), add `-c "hla_vid_pid 0x0483 0x374e"` after the
  `-f ...openocd.cfg` argument.
- Instead of `--privileged`, you can pass only the device with
  `--device /dev/bus/usb/<bus>/<dev>`.

## C. Remote OpenOCD (config store from the Mac)

`openocd.cfg` has `bindto 0.0.0.0`, so OpenOCD running on the Linux machine from workflow B
accepts remote connections (GDB 3333, Tcl 6666). The Mac can then program and configure the
board through Docker, without USB access:

```sh
# Linux machine with the board (leave running):
docker run --rm -it --privileged --network host -v /dev/bus/usb:/dev/bus/usb -v "$PWD":/work \
    ekh05-builder:2.14.3 openocd -c "set CONNECT_ASSERT_SRST 1" \
    -f third_party/mm-iot-sdk/framework/src/platforms/mm-mm6108-ekh05/openocd.cfg

# Mac, from the repository root:
docker run --rm -e PYTHONDONTWRITEBYTECODE=1 -v "$PWD":/work ekh05-builder:2.14.3 \
    python3 third_party/mm-iot-sdk/framework/tools/platform/program-configstore.py \
    -H <linux-host> -p mm-mm6108-ekh05 -d write-json relay_ekh05/config.hjson

# Mac: program the ELF through the remote OpenOCD's GDB server
docker run --rm -v "$PWD":/work ekh05-builder:2.14.3 arm-none-eabi-gdb -batch \
    -ex "target extended-remote <linux-host>:3333" -ex "monitor reset halt" \
    -ex "load" -ex "monitor reset run" relay_ekh05/build/relay_ekh05.elf
```

Only do this on a trusted lab network, because the OpenOCD ports have no authentication.

## Serial console (macOS)

The ST-LINK virtual COM port appears as `/dev/cu.usbmodem*`. Both of these tools ship with
macOS:

```sh
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodemXXXX 115200          # quit: Ctrl-A then K, then y
# or, logging to a file for later parsing:
cu -l /dev/cu.usbmodemXXXX -s 115200 | tee relay.log   # quit: ~.
```

After a reset, look for `EVENT,relay,...,ev=boot`. Note its `sta_mac`: that is the MAC behind
`192.168.50.2`. Then look for `ev=relay_status,state=repeater,...,depth=1` once node B is up, and
for `ev=ap_sta_join` when node A associates.
