# Notices and attribution

Xbox360PadToPS5

Copyright (C) 2026 iDobryak and contributors

This project is licensed under the GNU General Public License, version 3 only
(`GPL-3.0-only`). See `LICENSE` for the complete terms.

## Adapted code

The virtual-pad backend in `src/ps5_vpad.c` is adapted from:

- **Any-Pad PS5**, by elmonomalvad0<br>
  <https://codeberg.org/elmonomalva0/Any-Pad-ps5><br>
  Licensed under GPL-3.0-or-later.

It retains and modifies the credentialed `scePadVirtualDevice*`/MBus workflow,
virtual-device identification through `DEVICE_ADDED` events, user binding,
input insertion, PS-button support, and multi-controller lifecycle. The code
was modified in 2026 for an Xbox 360 USB receiver, a separate application
lifecycle, a direct-HID fallback, telemetry, and this project's state model.

The receiver transport in `src/ps5_receiver.c` and the direct-HID fallback in
`src/ps5_vpad.c` are adapted from:

- **FGG-XSense**, by FGG STORE<br>
  <https://github.com/FGGstore/FGG-XSense><br>
  Licensed under GPL-3.0.

They retain and modify the PS5 `USB_FS_*` transfer approach and `/dev/hid`
virtual-pad ioctl approach. The code was modified in 2026 for descriptor-based
Xbox 360 Wireless Gaming Receiver discovery, four independent radio slots,
libScePad/MBus operation, and clean payload lifecycle management.

## Protocol references

The Xbox 360 receiver protocol implementation was independently fitted to this
project and cross-checked against:

- **xbox360-controller-api**, by Koos du Preez<br>
  <https://github.com/kooscode/xbox360-controller-api><br>
  Licensed under GPL-2.0-or-later.
- Linux **xpad** driver<br>
  <https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c>.

These references informed the four-slot USB layout, connection and input report
formats, ready/LED/rumble messages, and packet conventions. Their authors and
contributors retain copyright in their respective works.

## No affiliation

This project is not affiliated with or endorsed by Sony Interactive
Entertainment or Microsoft. Product and company names belong to their
respective owners.
