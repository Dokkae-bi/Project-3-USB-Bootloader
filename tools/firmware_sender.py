#!/usr/bin/env python3
"""Send an application image to the Project 3 USB bootloader.

Protocol: docs/PROTOCOL.md - 12-byte header (magic, length, crc32), then the
payload, with 'R' after the erase and 'K' / 'E' as the verdict.

The bootloader only listens for 10 s after reset, so start this right after
pressing the reset button (or use --wait and start it first).

Needs pyserial:  python3 -m pip install -r tools/requirements.txt

Examples:
    python3 tools/firmware_sender.py --dry-run Lab3_Application/build/Debug/Lab3_Application.bin
    python3 tools/firmware_sender.py Lab3_Application/build/Debug/Lab3_Application.bin
    python3 tools/firmware_sender.py --wait 30 Lab3_Application/build/Debug/Lab3_Application.bin

Exit codes:
    0  image verified in flash ('K')
    1  board reported a CRC failure ('E'); nothing was launched
    2  port, timeout or protocol error; the board state is unknown
"""

import argparse
import os
import struct
import sys
import time
import zlib

MAGIC = b"FWUP"
HEADER_LEN = 12
APP_BYTES = 0x100000 - 0x10000        # 960 KB application region
CHUNK = 1024
STATUS_READY = b"R"
STATUS_OK = b"K"
STATUS_FAIL = b"E"

BOOTLOADER_VID = 0x0483
BOOTLOADER_PID = 0x5740               # CubeMX default CDC device
STLINK_PID = 0x374B                   # its virtual COM port - never this one

EXIT_OK = 0
EXIT_CRC_FAIL = 1
EXIT_ERROR = 2


class SenderError(Exception):
    """Anything that stops the transfer other than a clean 'E' verdict."""


def build_frame(payload):
    """Header + payload as one bytes object. Length and CRC cover the payload."""
    if not payload:
        raise ValueError("image is empty")
    if len(payload) > APP_BYTES:
        raise ValueError(
            "image is %d bytes, application region is %d" % (len(payload), APP_BYTES)
        )
    header = MAGIC + struct.pack("<II", len(payload), zlib.crc32(payload) & 0xFFFFFFFF)
    return header + payload


def _comports():
    try:
        from serial.tools import list_ports
    except ImportError:
        raise SenderError(
            "pyserial is not installed: python3 -m pip install -r tools/requirements.txt"
        )
    return list(list_ports.comports())


def find_port(explicit=None):
    """Prefer the bootloader's own CDC device over the ST-LINK's COM port."""
    if explicit:
        return explicit

    candidates = _comports()
    boot = [p for p in candidates
            if p.vid == BOOTLOADER_VID and p.pid == BOOTLOADER_PID]
    if boot:
        return boot[0].device

    stlink = [p for p in candidates
              if p.vid == BOOTLOADER_VID and p.pid == STLINK_PID]
    if stlink:
        raise SenderError(
            "found only the ST-LINK's COM port (%s), not the bootloader.\n"
            "The bootloader enumerates as 0483:5740 on the board's USB OTG port"
            % stlink[0].device
        )

    raise SenderError(
        "no bootloader found. Ports seen: %s"
        % (", ".join("%s [%04x:%04x]" % (p.device, p.vid or 0, p.pid or 0)
                     for p in candidates) or "none")
    )


def wait_for_port(explicit, wait_s):
    """find_port(), retried for up to wait_s seconds so the script can be
    started before the board is reset."""
    deadline = time.time() + wait_s
    while True:
        try:
            return find_port(explicit)
        except SenderError:
            if time.time() >= deadline:
                raise
            time.sleep(0.25)


def wait_for(ser, want, timeout, what):
    """Block until one of the status bytes in `want` arrives."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if b:
            if b in want:
                return b
            raise SenderError("unexpected status byte %r while waiting for %s"
                              % (b, what))
    raise SenderError("no %s from the board after %.0f s" % (what, timeout))


def send(frame, port, erase_timeout):
    """Run one transfer and return the verdict byte ('K' or 'E')."""
    import serial

    length = len(frame) - HEADER_LEN

    # Baud is meaningless for a CDC link but pyserial insists on one. No
    # write timeout: if the ring fills, the device NAKs and write() blocks,
    # which is the flow control this protocol depends on.
    try:
        with serial.Serial(port=port, baudrate=115200, timeout=0.5,
                           write_timeout=None) as ser:
            time.sleep(0.2)
            ser.reset_input_buffer()

            print("\nsending header, waiting for erase...")
            ser.write(frame[:HEADER_LEN])
            ser.flush()
            wait_for(ser, STATUS_READY, erase_timeout, "'R' (ready)")

            print("erased. streaming %d bytes in %d-byte chunks..." % (length, CHUNK))
            sent = 0
            started = time.time()
            while sent < length:
                n = min(CHUNK, length - sent)
                ser.write(frame[HEADER_LEN + sent:HEADER_LEN + sent + n])
                sent += n
                print("\r  %d / %d bytes (%.0f%%)" % (sent, length, 100.0 * sent / length),
                      end="", flush=True)
            ser.flush()
            print("\n  streamed in %.1f s" % (time.time() - started))

            print("waiting for the board to verify...")
            return wait_for(ser, (STATUS_OK, STATUS_FAIL), 30.0, "verdict")
    except serial.SerialException as exc:
        raise SenderError(
            "serial link failed (%s). Was the cable pulled or the board reset?\n"
            "The flash may hold a partial image; reset the board and send again." % exc
        )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="path to Lab3_Application.bin")
    ap.add_argument("--port", help="serial device, if auto-detect picks wrong")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the header and stop; no serial port touched")
    ap.add_argument("--erase-timeout", type=float, default=20.0,
                    help="seconds to wait for 'R' (default 20)")
    ap.add_argument("--wait", type=float, default=0.0, metavar="SECONDS",
                    help="keep looking for the bootloader's port for this long "
                         "(default 0: look once)")
    args = ap.parse_args(argv)

    try:
        with open(args.image, "rb") as fh:
            payload = fh.read()
        frame = build_frame(payload)
    except (OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_ERROR

    length, crc = struct.unpack("<II", frame[4:HEADER_LEN])

    print("image   : %s" % os.path.basename(args.image))
    print("size    : %d bytes" % length)
    print("crc32   : 0x%08X" % crc)
    print("header  : %s" % frame[:HEADER_LEN].hex(" "))

    if args.dry_run:
        print("\ndry run: frame built, nothing sent")
        return EXIT_OK

    try:
        port = wait_for_port(args.port, args.wait)
        print("port    : %s" % port)
        verdict = send(frame, port, args.erase_timeout)
    except SenderError as exc:
        print("\nerror: %s" % exc, file=sys.stderr)
        return EXIT_ERROR

    if verdict == STATUS_FAIL:
        print("\nFAIL: the board erased and programmed, but the CRC read back "
              "from flash does not match.\nNothing was launched.")
        return EXIT_CRC_FAIL

    print("\nOK: image verified in flash.")
    print("Press the board's blue User button (B1) to launch it.")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
