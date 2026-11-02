# Xbox 360 Wireless Gaming Receiver USB notes

These notes describe the USB protocol required by this payload. They were
cross-checked against `xbox360-controller-api`, FGG-XSense's PS5 transfer code,
and the packet conventions used by Linux `xpad`/xboxdrv.

## USB layout

The receiver is a vendor-specific composite device, not a HID gamepad.

- Known Microsoft VID: `045e`.
- Known PIDs include `0719`, `02a9`, and the observed clone/variant `0291`.
- A typical receiver has one configuration and eight interfaces.
- Controller radio slots are interfaces `0`, `2`, `4`, and `6`.
- Their interrupt endpoints are normally:

| Slot | Interface | Interrupt IN | Interrupt OUT |
|---:|---:|---:|---:|
| 1 | 0 | `0x81` | `0x01` |
| 2 | 2 | `0x83` | `0x03` |
| 3 | 4 | `0x85` | `0x05` |
| 4 | 6 | `0x87` | `0x07` |

The useful interfaces identify themselves as class `0xff`, subclass `0x5d`,
protocol `0x81`. Matching this signature and reading endpoint descriptors is
safer than hardcoding the table or accepting every device with Microsoft VID.
The remaining interfaces are companion functions and must not be claimed as
controller slots.

Interrupt-IN buffers are 32 bytes. Interrupt-OUT commands are 12 bytes.

## Incoming reports

### Connection state

```text
08 80 ...   controller connected
08 00 ...   controller disconnected
```

The USB interface itself always represents the same radio slot; the connection
message says whether a controller currently occupies that slot.

### Controller state

A controller report begins:

```text
00 01 xx f0 xx 13  BB BB LT RT LX LX LY LY RX RX RY RY ...
                   ^ state starts at byte 6
```

All 16-bit values are little-endian.

| Offset | Size | Meaning |
|---:|---:|---|
| 6 | 2 | button mask |
| 8 | 1 | left trigger, `0..255` |
| 9 | 1 | right trigger, `0..255` |
| 10 | 2 | left stick X, signed |
| 12 | 2 | left stick Y, signed |
| 14 | 2 | right stick X, signed |
| 16 | 2 | right stick Y, signed |

Button bits:

```text
0001 up       0002 down      0004 left       0008 right
0010 start    0020 back      0040 L3         0080 R3
0100 LB       0200 RB        0400 Guide
1000 A        2000 B         4000 X          8000 Y
```

Xbox Y axes grow upward. PlayStation Y axes grow downward, so Y must be
inverted during conversion.

## Outgoing commands

Every command is padded to 12 bytes.

### Mark controller ready

```text
00 00 02 80 00 00 00 00 00 00 00 00
```

Send this after a connection event. Without the slot-ready exchange, some
receivers/controllers do not begin normal operation reliably.

### Ring LED

```text
00 00 08 4P 00 00 00 00 00 00 00 00
```

`P=2..5` selects the four "blink, then stay on" player quadrants. Other low
nibble values select off, rotating, blinking, and related patterns.

### Rumble

```text
00 01 0f c0 00 BB SS 00 00 00 00 00
```

`BB` is the large motor and `SS` the small motor, both `0..255`. Sending zero
for both stops rumble.

## Transfer lifecycle on PS5

The PS5 does not expose per-endpoint device nodes for this use. The payload:

1. opens `/dev/ugenX.Y`;
2. reads the full configuration descriptor;
3. selects only `ff/5d/81` interfaces;
4. configures and claims those interfaces;
5. initializes all endpoint slots with `USB_FS_INIT`/`USB_FS_OPEN`;
6. keeps one interrupt-IN transfer pending per radio slot;
7. uses the endpoint index returned by `USB_FS_COMPLETE`—a completion may
   belong to any slot or to an OUT transfer;
8. queues OUT commands per slot so ready, LED, and rumble packets cannot
   overwrite one another while the endpoint is busy.

Receiver removal and controller removal are different events. Unplugging the
USB receiver invalidates all interfaces. Turning off one controller produces a
slot disconnect packet and must remove only that slot's virtual controller.

## Practical debugging

Capture and log at least the first 32 bytes from each slot before changing the
parser. Clone receivers sometimes use another PID, but a changed PID alone is
not a protocol difference. First compare interface class/subclass/protocol,
endpoint addresses, connection frames, and the `00 01 .. f0 .. 13` signature.

Do not run two payloads that claim the same receiver interfaces. They will
consume each other's completions and make a healthy controller look unstable.
