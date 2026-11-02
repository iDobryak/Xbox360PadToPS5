# PS5 Xbox 360 Wireless Receiver bridge

PS5 payload that reads up to four Xbox 360 controllers through an **Xbox 360
Wireless Gaming Receiver** and registers each connected controller as a virtual
PlayStation pad (DualSense class) for the system UI and games.

The implementation targets the receiver's older Xbox 360 USB protocol. It is
not a GIP/Xbox One driver.

## Status

Implemented:

- discovery by the receiver interface signature `FF/5D/81` (so compatible
  clones are not excluded only because of their VID/PID);
- descriptor-based discovery of every slot's real interrupt IN/OUT endpoints;
- up to four independently connected/disconnected wireless controllers;
- receiver inquiry and 32-byte radio packet parsing;
- buttons, d-pad, sticks and analog triggers;
- Xbox Guide mapped to the PS system button through the libScePad/MBus backend;
- one dynamically created virtual PS5 controller per connected Xbox pad;
- cleanup of virtual devices on disconnect or payload exit;
- host-side protocol/mapping tests;
- logging to `/data/ps5-xb360/receiver.log`;
- downloading the complete persistent log from the web interface without
  loading the whole file into payload memory;
- live USB interval, payload latency, InsertData latency, malformed packet,
  delivery failure, and estimated-gap telemetry per receiver slot;
- a 10-second packet trace exported as
  `/data/ps5-xb360/telemetry.csv` and downloadable from the web interface;
- lightweight status and control page, preferring `http://PS5-IP:8096/`.

The receiver packet layout and USB lifecycle are documented in
[`docs/receiver-protocol.md`](docs/receiver-protocol.md).

## Web interface

The payload tries ports `8096` through `8105` and uses the first free one. The
actual port is shown in the PS5 notification and log, and is written to
`/data/ps5-xb360/receiver.port`. Open `http://<PS5 address>:<port>/` from a
device on the same network. The page
shows the payload PID and uptime, receiver identity, four controller slots,
report/error counters, and the last 64 log lines. It can resend receiver
inquiry commands, press PS for any connected slot, download the complete
persistent log, or stop the payload cleanly. The server is a single
nonblocking socket with an in-memory page and ring log; it has no filesystem
polling, WebSocket, external library, or background thread.

### Latency and packet telemetry

Each slot displays four interval percentiles (`p50/p95/p99/max`) in
microseconds:

- **interval** — time between valid Xbox input reports returned by the USB
  receiver read loop;
- **bridge** — time from that USB read timestamp through parsing/mapping to the
  return from the virtual-pad submission;
- **InsertData** — only the virtual-pad submission call itself.

The counters distinguish received USB packets, valid input reports, reports
delivered to PS5, reports temporarily coalesced while the virtual controller is
being prepared, malformed packets, and failed virtual-pad submissions.
`Предполагаемые пропуски` estimates missing input periods when an interval is
much larger than the learned normal interval. The Xbox 360 wireless input
envelope does not provide a usable sequence number, so this is an estimate:
radio loss, receiver-side loss, USB scheduling delay, and a controller that
simply stopped sending cannot be distinguished with certainty.

Press **Записать 10 секунд**, exercise the controls, wait for export to finish,
then press **Скачать CSV**. The CSV contains one row per input report with
relative timestamp, slot, packet length, input interval, bridge latency,
InsertData latency, and delivery result. Capture storage is allocated only
during recording (8192 fixed-size rows, about 160 KiB); export is performed in
small chunks from the normal loop. Continuous statistics use fixed histograms
and do not grow with runtime. These measurements end when the payload returns
from the PS5 input API; they cannot measure when a game samples the resulting
state or display latency.

Initial hardware validation on firmware **13.60** has confirmed discovery of
the `045E:0291` receiver, all four radio endpoints, controller pairing, and
creation of a virtual DualSense device. Wider game compatibility and slots 2–4
still need hardware coverage. That first validation used the direct `/dev/hid`
backend; the new libScePad/MBus PS-button path still needs validation on 13.60.
The USB path is adapted from the PS5-proven `USB_FS_*` approach in FGG-XSense,
and the system virtual-pad path follows AnyPad.

## Controller mapping

| Xbox 360 | PlayStation |
|---|---|
| A / B / X / Y | Cross / Circle / Square / Triangle |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2, analog plus digital threshold |
| Back / Start | Create / Options |
| Xbox Guide | PS |
| Left/right stick click | L3 / R3 |
| D-pad and sticks | D-pad and sticks |

The primary virtual-pad backend uses `scePadVirtualDevice*`, identifies the new
MBus device, and binds it to a signed-in user. This is the path that can deliver
the system PS button. If its libraries, credentials, user binding, or device
identification are unavailable, the payload falls back to the previously tested
direct `/dev/hid` backend; normal controls continue to work there, but PS is
unavailable and the reason is written to the log. Xbox 360 pads also have no
touchpad, motion sensors, microphone or adaptive triggers.

Hold **Back + Start for two seconds** to stop the payload cleanly.
Launching the payload again also requests a clean shutdown of the running copy,
waits for it to release USB and the virtual pads, and then starts the new copy.
Versions built before this stop-request mechanism still require Payload Manager
stop/kill or one console reboot.

Payload loaders commonly stage uploads under the generic process name
`payload.elf`; the PS5 SDK used here does not export `setproctitle`, so the
running-process list may not preserve `ps5-xb360-receiver.elf`. Current builds
write their PID into `/data/ps5-xb360/receiver.lock`, allowing the exact
`payload.elf` entry to be identified.

## Build the PS5 payload

Install [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), then:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

On macOS the Makefile automatically detects Homebrew `llvm`/`llvm@18`. Set
`LLVM_CONFIG=/custom/path/llvm-config` explicitly when using another LLVM.

The output is `ps5-xb360-receiver.elf`. Send it to a payload loader on port
9021 with:

```sh
make send PS5_HOST=192.168.x.x
```

Suggested first hardware test:

1. Start with no other USB-controller payload running.
2. Connect the receiver and launch the ELF.
3. Pair one controller and verify the notification and system UI input.
4. Verify that the log says `libScePad/MBus` and later `PS enabled`.
5. Test both the Xbox Guide button and the slot's **Press PS** web button.
6. Start a 10-second telemetry recording, move the sticks and press buttons,
   then download the CSV and the complete log from the web page.
7. Confirm `failed` and `malformed` remain zero. A few `coalesced` reports are
   expected only while a new virtual controller is being prepared.
8. Add controllers in slots 2–4 one at a time.

If MBus initialization, identification, or user binding fails, the log records
the exact stage and switches that slot to the direct backend. This preserves
normal controls but cannot provide PS until the system backend issue is fixed.

## Host tests

```sh
cmake -S . -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

On macOS, if the active Command Line Tools SDK is inconsistent, select the
full Xcode toolchain or pass its compiler and SDK explicitly.

## Architecture

```text
/dev/ugenX.Y
  └─ USB_FS_* transfers for FF/5D/81 interfaces (1 interface per radio slot)
       └─ Xbox 360 wireless envelope + ordinary 0x00/0x14 input report
            └─ normalized x360_state_t
                 └─ DualSense-class button/axis mapping
                      └─ libScePad virtual device + MBus user binding
                           └─ `/dev/hid` fallback when unavailable
                           └─ PS5 system UI and games
```

The protocol core (`x360_receiver.c`, `dualsense_mapper.c`, `bridge.c`) is kept
independent from PS5 headers and tested on the host. PS5-specific USB and
virtual-pad code lives in `ps5_receiver.c` and `ps5_vpad.c`.

## Licence and attribution

This project is distributed under **GNU GPL version 3 only**
(`GPL-3.0-only`). See [LICENSE](LICENSE) for the complete licence text and
[NOTICE.md](NOTICE.md) for attribution and modification notices.

Parts of the implementation are adapted from GPL-covered projects:

- [FGG-XSense](https://github.com/FGGstore/FGG-XSense) by FGG STORE: PS5
  `USB_FS_*` transfer lifecycle and direct `/dev/hid` virtual-pad backend.
- [Any-Pad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) by
  elmonomalvad0: credentialed libScePad/MBus virtual devices, user binding,
  device identification, PS-button injection, and multi-controller lifecycle.

Protocol information was also cross-checked against:

- Linux [`xpad`](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c):
  Xbox 360 Wireless Receiver packet conventions;
- [xbox360-controller-api](https://github.com/kooscode/xbox360-controller-api)
  by Koos du Preez: four-slot interface/endpoint layout, connection reports,
  ready, LED, rumble, and controller-state packet offsets.

The local `reference/` directory used during development is intentionally not
part of this repository. Obtain those projects from their upstream links so
their history and licence notices remain authoritative.

If you distribute an ELF or another binary, provide the complete corresponding
source for that exact version, including its build scripts, preserve the
copyright and licence notices, and document your modifications as required by
GPLv3.
