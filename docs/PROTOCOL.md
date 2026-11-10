# Firmware Update Protocol — v1

The contract between the host uploader and the bootloader. Everything below is
what the bootloader and `firmware_sender.py` must agree on; chip-specific flash
mechanics are listed separately at the end.

**Status:** proposed, not yet implemented. Interface frozen once the team agrees.

---

## 1. Frame layout

One transfer is one frame, sent in a single direction (host → device).

| Field | Size | Type | Value |
|---|---|---|---|
| `magic` | 4 B | ASCII | `FWUP` = `46 57 55 50` |
| `length` | 4 B | `uint32` little-endian | payload size in bytes |
| `crc32` | 4 B | `uint32` little-endian | CRC-32 over the payload only |
| `payload` | `length` B | raw | contents of `Lab3_Application.bin` |

Header is **12 bytes**. No padding, no escaping — the payload is binary and is
read only by its declared length.

**Validation, in this order.** Reject the frame on the first failure:

1. `magic` must match exactly. Anything else means the host is not speaking this
   protocol (or we caught the stream mid-frame) — discard and re-arm.
2. `1 <= length <= 983040` (960 KB, the size of the application region). A zero
   or oversized length is rejected before a single page is erased. Note the
   region is 960 KB but the last usable byte is `0x080FFFFF`, so the ceiling is
   `0x080FFFFF - 0x08010000 + 1`.
3. `crc32` is the IEEE 802.3 polynomial (`0xEDB88320` reflected), the same one
   Python's `zlib.crc32()` and the STM32 CRC peripheral produce.

---

## 2. Sequence

```
host                                    bootloader
  |                                          |
  |                              reset, 10 s window opens
  |--- magic + length + crc --------------->|  header accepted
  |                                          |  erase 480 pages  (~10 s)
  |<--------------------- 'R' (0x52) --------|  ready for payload
  |--- payload, chunked -------------------->|  program double words
  |                                          |  recompute CRC from flash
  |<--------------------- 'K' (0x4B) --------|  verified, hold for button
  |    ...press B1...                        |  VTOR / MSP / branch
  |                                          v  application runs
```

On a CRC mismatch the bootloader replies `'E'` (0x45) instead of `'K'` and
**must not jump**. The flash region is left holding a bad image — that is
intentional, and it is why the bootloader never repeats the erase without a new
header.

## 3. Timing rules

| Rule | Value | Why |
|---|---|---|
| Entry window | **10 s** from reset | Long enough to start the script by hand, short enough that the board boots into an installed app on its own. |
| Inter-byte timeout during header or payload | **3 s** | Detects a host that died mid-transfer. The erase pause does not count — that happens before `'R'`. |
| Erase-to-ready budget | host waits **20 s** for `'R'` | 480 pages at roughly 20 ms each. |
| Hold after `'K'` | **indefinite** | The demo requires pressing B1 on camera; auto-jumping would beat the user to it. |

**The erase is not a hang.** USB CDC gives the device flow control for free: if
the bootloader stops reading the OUT endpoint, the host's `write()` blocks.
That is why a naive one-shot `write()` of the whole file *seems* to work — the
transfer simply stalls during the erase. Relying on that is fragile (pyserial's
`write_timeout` will fire), so the protocol puts the erase **before** the
payload and confirms it with `'R'`.

## 4. Host rules

- Locate the port by **VID:PID `0483:5740`** — not by `/dev/tty.usbmodem*` name.
  The ST-LINK's own VCP is `0483:374b` and will happily swallow your firmware.
- `115200 8N1`, no flow control. The baud rate is irrelevant to a CDC link but
  pyserial requires something.
- Write the payload in **1024-byte chunks**. Do not set a short `write_timeout`.
- Read the single-byte status with a real timeout, and treat "no byte" as
  failure rather than success.
- Exit non-zero on anything other than `'K'`.

## 5. LED language

Two colours, two owners — so the demo video is unambiguous:

| LED | Owner | Meaning |
|---|---|---|
| **LD2** (blue, PB7) | application | slow blink = original firmware, fast blink = updated |
| **LD3** (red, PB14) | bootloader | solid while erasing/programming, off when verified and waiting, rapid blink = CRC failure |

The bootloader must not touch LD2, or the "before" and "after" states blur.

---

## 6. Implementation notes for the flash side

Not part of the wire format, but whoever builds Requirement 2 needs these or the
erased region will be wrong:

- **Page numbers are bank-relative on this part.** `FLASH_BANK1_END` is
  `0x0807FFFF`; `IS_FLASH_PAGE` requires `< 256`. The 480-page application region
  is **bank 1 pages 32–255 (224 pages)** followed by **bank 2 pages 0–255
  (256 pages)**. A `for (page = 32; page < 512; ...)` loop fails the HAL assert
  and erases garbage.
- Page size is 2 KB (`FLASH_PAGE_SIZE == 0x800` for `STM32L496xx`).
- Flash programs in **aligned 8-byte double words**. Accumulate bytes in RAM and
  program whole double words only.
- **Pad the tail with `0xFF`** when the payload is not a multiple of 8, and pad
  only — never count the padding toward `length`.
- Unlock the flash controller before erase/program and lock it after. Never
  touch below `0x08010000`.
- The application image carries its own vector table at `0x08010000`; the
  handoff is `VTOR = 0x08010000`, `MSP = *(uint32_t*)0x08010000`,
  branch to `*(uint32_t*)0x08010004`.

## 7. Deliberately out of scope for v1

- Retransmission per chunk (Bonus C). `'K'`/`'E'` is a whole-image verdict; a
  packet protocol layers on top of this frame without changing the header.
- Encryption (Bonus A). Would wrap the payload and change `length` semantics.
- Multiple images / rollback. Single slot.

---

## 8. Change log

| Version | Date | Change |
|---|---|---|
| v1 | 2026-10-27 | Initial proposal: 12-byte header, ready/verdict bytes, 10 s window. |
