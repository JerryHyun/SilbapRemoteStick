#!/usr/bin/env python3
"""Repartition and format an SD card for SilbapRemoteStick.

The firmware expects a plain MBR card with two FAT32 LBA partitions:

    partition 1  type 0x0C  LBA 2048   16 GiB exactly  label SILBAPSTICK
    partition 2  type 0x0C  LBA 34816  the rest        label SILBAP

Partition 1 is the volume the embroidery machine sees over USB MSC and the
web UI shows as /sd.  Partition 2 is kept for archival use.

This script writes to a real block device.  It refuses to do anything unless
the target is a block device, nothing is mounted from it or its partitions,
the card is at least 20 GiB, and --yes was given explicitly.

Usage:
    python3 tools/mkcard.py --dry-run /dev/sdX   # show the plan, write nothing
    python3 tools/mkcard.py --yes /dev/sdX       # repartition and format
"""

import argparse
import os
import stat
import struct
import subprocess
import sys
import time

SECTOR_SIZE = 512
GIB = 1024 ** 3

PART1_START = 2048
PART1_SECTORS = 16 * GIB // SECTOR_SIZE  # 33554432, exactly 16 GiB
PART2_START = PART1_START + PART1_SECTORS
MIN_CARD_BYTES = 20 * GIB
TYPE_FAT32_LBA = 0x0C
LABEL_MACHINE = "SILBAPSTICK"
LABEL_ARCHIVE = "SILBAP"
SECTORS_PER_CLUSTER = 64  # 32 KiB clusters at 512-byte sectors
MBR_SIGNATURE = 0x55AA
PART_ENTRY_OFFSET = 446

EXIT_REFUSED = 1
EXIT_FAILED = 2


class Refused(Exception):
    """A safety check said no. Nothing has been written."""


def build_mbr(card_sectors):
    """Return the 512-byte MBR for a card of card_sectors 512-byte sectors.

    Pure: no I/O, no globals.  Entry 1 is the fixed 16 GiB machine volume,
    entry 2 starts right after it and covers whatever is left.
    """
    if card_sectors <= PART2_START:
        raise ValueError(
            "card of %d sectors is too small for the 16 GiB volume plus a "
            "second partition" % card_sectors
        )

    part2_sectors = card_sectors - PART2_START
    if part2_sectors > 0xFFFFFFFF:
        raise ValueError("partition 2 would not fit a 32-bit sector count")

    mbr = bytearray(SECTOR_SIZE)
    _put_entry(mbr, 0, TYPE_FAT32_LBA, PART1_START, PART1_SECTORS)
    _put_entry(mbr, 1, TYPE_FAT32_LBA, PART2_START, part2_sectors)
    mbr[510] = MBR_SIGNATURE >> 8  # 0x55
    mbr[511] = MBR_SIGNATURE & 0xFF  # 0xAA
    return bytes(mbr)


def _put_entry(mbr, index, ptype, first_lba, sectors):
    """Write one 16-byte partition entry.  CHS fields are left at zero."""
    off = PART_ENTRY_OFFSET + 16 * index
    if not 0 < first_lba <= 0xFFFFFFFF:
        raise ValueError("first LBA out of range: %r" % (first_lba,))
    if not 0 < sectors <= 0xFFFFFFFF:
        raise ValueError("sector count out of range: %r" % (sectors,))
    mbr[off + 0] = 0x00  # status: not bootable
    mbr[off + 1] = 0x00  # CHS first, head
    mbr[off + 2] = 0x00  # CHS first, sector/cylinder
    mbr[off + 3] = 0x00
    mbr[off + 4] = ptype
    mbr[off + 5] = 0x00  # CHS last, head
    mbr[off + 6] = 0x00
    mbr[off + 7] = 0x00
    struct.pack_into("<I", mbr, off + 8, first_lba)
    struct.pack_into("<I", mbr, off + 12, sectors)


def decode_mbr(mbr):
    """Decode the partition table into a list of dicts, for printing."""
    entries = []
    for index in range(4):
        off = PART_ENTRY_OFFSET + 16 * index
        ptype = mbr[off + 4]
        first_lba, sectors = struct.unpack_from("<II", mbr, off + 8)
        entries.append(
            {
                "index": index + 1,
                "type": ptype,
                "first_lba": first_lba,
                "sectors": sectors,
                "bytes": sectors * SECTOR_SIZE,
            }
        )
    return entries, struct.unpack_from(">H", mbr, 510)[0]


def plan(card_bytes):
    """Return the intended layout for a card of card_bytes bytes."""
    card_sectors = card_bytes // SECTOR_SIZE
    mbr = build_mbr(card_sectors)
    entries, signature = decode_mbr(mbr)
    return {
        "card_bytes": card_bytes,
        "card_sectors": card_sectors,
        "mbr": mbr,
        "entries": entries,
        "signature": signature,
    }


def human(nbytes):
    return "%.2f GiB (%d bytes)" % (nbytes / GIB, nbytes)


def describe(layout):
    lines = []
    lines.append("card:        %s, %d sectors of %d bytes"
                 % (human(layout["card_bytes"]), layout["card_sectors"], SECTOR_SIZE))
    lines.append("MBR:         512 bytes at LBA 0, signature 0x%04X"
                 % layout["signature"])
    for entry in layout["entries"][:2]:
        label = LABEL_MACHINE if entry["index"] == 1 else LABEL_ARCHIVE
        lines.append(
            "partition %d: type 0x%02X  start LBA %-10d  %-10d sectors  %-24s"
            "  label %s"
            % (
                entry["index"],
                entry["type"],
                entry["first_lba"],
                entry["sectors"],
                human(entry["bytes"]),
                label,
            )
        )
    lines.append(
        "sectors/cluster: %d (32 KiB);  filesystem: FAT32;  sector size: 512"
        % SECTORS_PER_CLUSTER
    )
    return "\n".join(lines)


# --------------------------------------------------------------------------
# host helpers


def run(argv):
    return subprocess.run(
        argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )


def card_bytes(path):
    r = run(["blockdev", "--getsize64", path])
    if r.returncode == 0 and r.stdout.strip().isdigit():
        return int(r.stdout.strip())
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        return os.lseek(fd, 0, os.SEEK_END)
    finally:
        os.close(fd)


def partition_node(dev, index):
    """Return /dev/sdb1, /dev/mmcblk0p1, /dev/nvme0n1p1, ..."""
    real = os.path.realpath(dev)
    if real[-1].isdigit():
        return "%sp%d" % (real, index)
    return "%s%d" % (real, index)


def mountpoints_of(dev):
    """Every mountpoint reachable from dev, as lsblk and findmnt see them."""
    found = []
    r = run(["findmnt", "-S", dev, "-o", "TARGET", "-n"])
    if r.returncode == 0:
        found.extend(line.strip() for line in r.stdout.splitlines() if line.strip())
    r = run(["lsblk", "-no", "MOUNTPOINT", dev])
    if r.returncode == 0:
        found.extend(line.strip() for line in r.stdout.splitlines() if line.strip())
    return found


def check_refusals(dev):
    """Raise Refused unless dev is a safe, unused, big-enough card."""
    try:
        st = os.stat(dev)
    except OSError as exc:
        raise Refused("cannot stat %s: %s" % (dev, exc.strerror or exc))

    if not stat.S_ISBLK(st.st_mode):
        raise Refused(
            "%s is not a block device (mode 0o%o); refusing to write"
            % (dev, stat.S_IMODE(st.st_mode))
        )

    real = os.path.realpath(dev)
    for target in [real, partition_node(real, 1), partition_node(real, 2)]:
        for mountpoint in mountpoints_of(target):
            raise Refused("%s is mounted at %s; unmount it first" % (target, mountpoint))

    size = card_bytes(real)
    if size < MIN_CARD_BYTES:
        raise Refused(
            "card is %s, smaller than the required %s"
            % (human(size), human(MIN_CARD_BYTES))
        )
    return real, size


def mkfs_supports_offset():
    r = run(["mkfs.fat", "--help"])
    return "--offset" in (r.stdout + r.stderr)


def write_at(fd, data, offset):
    view = memoryview(data)
    while view:
        written = os.pwrite(fd, view, offset)
        if written <= 0:
            raise OSError("short write at offset %d" % offset)
        view = view[written:]
        offset += written


def wipe_boot_sector(path, lba):
    fd = os.open(path, os.O_WRONLY | os.O_CLOEXEC)
    try:
        write_at(fd, b"\0" * SECTOR_SIZE, lba * SECTOR_SIZE)
        os.fsync(fd)
    finally:
        os.close(fd)


def format_partition(dev, index, first_lba, sectors, label, use_offset):
    """Format one partition.

    Prefer the kernel's own partition node ("/dev/sda1"), because mkfs.fat then gets the size and the
    start from the partition table itself. Passing --offset with the whole device makes mkfs.fat see
    every partition on the disk and refuse ("Partitions or virtual mappings on device ... , not making
    filesystem"), and its block-count guess comes out as the size of the LAST partition, which is how
    the first attempt failed. The loop fallback stays for kernels that have not published the node yet.
    """
    blocks = sectors // 2  # mkfs.fat BLOCKS are 1024-byte blocks
    base = [
        "mkfs.fat",
        "-F", "32",
        "-n", label,
        "-s", str(SECTORS_PER_CLUSTER),
        "-S", str(SECTOR_SIZE),
    ]
    part = "%s%d" % (dev, index)
    if os.path.exists(part):
        return base + [part], None

    loop = None
    r = run([
        "losetup", "--find", "--show",
        "--offset", str(first_lba * SECTOR_SIZE),
        "--sizelimit", str(sectors * SECTOR_SIZE),
        dev,
    ])
    if r.returncode != 0:
        raise OSError("losetup failed: %s" % (r.stderr.strip() or r.stdout.strip()))
    loop = r.stdout.strip()
    argv = base + [loop]
    return argv, loop


def reread_partition_table(dev):
    for argv in (["partprobe", dev], ["blockdev", "--rereadpt", dev]):
        run(argv)


def blkid_label(dev):
    r = run(["blkid", "-p", "-o", "value", "-s", "LABEL", dev])
    if r.returncode == 0:
        return r.stdout.strip()
    return ""


def verify(dev, layout):
    """Re-read the MBR and the labels; raise OSError if the card is not right."""
    fd = os.open(dev, os.O_RDONLY | os.O_CLOEXEC)
    try:
        mbr = os.pread(fd, SECTOR_SIZE, 0)
    finally:
        os.close(fd)

    entries, signature = decode_mbr(mbr)
    if signature != MBR_SIGNATURE:
        raise OSError("MBR signature is 0x%04X, expected 0x55AA" % signature)

    first = entries[0]
    if first["type"] != TYPE_FAT32_LBA:
        raise OSError("partition 1 type is 0x%02X, expected 0x0C" % first["type"])
    if first["first_lba"] != PART1_START:
        raise OSError("partition 1 starts at LBA %d, expected %d"
                      % (first["first_lba"], PART1_START))
    if first["sectors"] != PART1_SECTORS:
        raise OSError("partition 1 is %d sectors, expected %d"
                      % (first["sectors"], PART1_SECTORS))
    if entries[1]["first_lba"] != PART2_START:
        raise OSError("partition 2 starts at LBA %d, expected %d"
                      % (entries[1]["first_lba"], PART2_START))

    node1 = partition_node(dev, 1)
    label = ""
    for _ in range(10):
        label = blkid_label(node1)
        if label:
            break
        time.sleep(0.5)
    if label != LABEL_MACHINE:
        raise OSError("blkid reports label %r on %s, expected %r"
                      % (label or None, node1, LABEL_MACHINE))

    node2 = partition_node(dev, 2)
    label2 = ""
    for _ in range(10):
        label2 = blkid_label(node2)
        if label2:
            break
        time.sleep(0.5)
    return label, label2


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Repartition and format an SD card for SilbapRemoteStick "
                    "(16 GiB SILBAPSTICK at LBA 2048, remainder SILBAP).",
        epilog="This writes to a real block device and destroys its contents.",
    )
    parser.add_argument("device", help="block device of the card, e.g. /dev/sdb")
    parser.add_argument("--yes", action="store_true",
                        help="required to actually write; without it the plan "
                             "is printed and nothing is touched")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the layout and the decoded MBR, write nothing")
    args = parser.parse_args(argv)

    try:
        dev, size = check_refusals(args.device)
    except Refused as exc:
        sys.stderr.write("mkcard: refusing: %s\n" % exc)
        return EXIT_REFUSED

    layout = plan(size)
    print(describe(layout))

    if args.dry_run:
        print("\ndry run: nothing written.")
        return 0

    if not args.yes:
        print("\nrefusing to write without --yes. re-run with --yes to apply.")
        return EXIT_REFUSED

    use_offset = mkfs_supports_offset()
    print("\nwriting MBR to %s ..." % dev, flush=True)
    try:
        fd = os.open(dev, os.O_WRONLY | os.O_CLOEXEC)
        try:
            write_at(fd, layout["mbr"], 0)
            os.fsync(fd)
        finally:
            os.close(fd)

        # Clear stale boot sectors so mkfs does not inherit an old filesystem.
        print("zeroing partition boot sectors ...", flush=True)
        wipe_boot_sector(dev, PART1_START)
        wipe_boot_sector(dev, PART2_START)

        for index, label in ((1, LABEL_MACHINE), (2, LABEL_ARCHIVE)):
            entry = layout["entries"][index - 1]
            part = partition_node(dev, index)
            print("mkfs.fat %s on %s (LBA %d, %d sectors) ..."
                  % (label, part, entry["first_lba"], entry["sectors"]), flush=True)
            fmt_argv, loop = format_partition(
                dev, index, entry["first_lba"], entry["sectors"], label, use_offset
            )
            try:
                r = run(fmt_argv)
            finally:
                if loop:
                    run(["losetup", "-d", loop])
            if r.returncode != 0:
                raise OSError("mkfs.fat failed (%d): %s"
                              % (r.returncode,
                                 (r.stderr or r.stdout).strip()))
            for line in (r.stdout + r.stderr).strip().splitlines():
                print("  " + line)

        print("re-reading partition table ...", flush=True)
        reread_partition_table(dev)

        label1, label2 = verify(dev, layout)
    except OSError as exc:
        sys.stderr.write("mkcard: failed: %s\n" % exc)
        return EXIT_FAILED

    print("done: %s is %s, %s is %s"
          % (partition_node(dev, 1), label1, partition_node(dev, 2), label2 or "(no label)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
