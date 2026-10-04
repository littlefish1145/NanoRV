"""Grow the Debian guest disk image offline under WSL; keeps users, packages and files.

Extends the image file, grows the root partition into the new tail, then grows
the ext4 filesystem. The image must not be attached to a running VM.
"""
import argparse, json, os, pathlib, re, subprocess, sys

SECTOR = 512


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def parse_size(text):
    match = re.fullmatch(r"(\d+)([kKmMgGtT]?)", text.strip())
    if not match:
        raise ValueError(f"invalid size: {text}")
    scale = {"": 1, "k": 1024, "m": 1024 ** 2, "g": 1024 ** 3, "t": 1024 ** 4}
    return int(match.group(1)) * scale[match.group(2).lower()]


def partition_start_sectors(image):
    table = json.loads(run("sfdisk", "--json", str(image)).stdout)["partitiontable"]
    if table.get("label") != "gpt":
        raise ValueError("expected a GPT image")
    partitions = table.get("partitions", [])
    root = None
    for candidate in partitions:
        node = str(candidate.get("node", ""))
        number = re.search(r"(\d+)$", node)
        name = str(candidate.get("name", ""))
        if (number and number.group(1) == "1") or name.lower() == "root":
            root = candidate
            break
    if root is None:
        raise ValueError("no root partition 1 found")
    return int(root["start"]), int(root["size"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--disk", default="assets/disk.raw")
    parser.add_argument("--size", default="10G")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    image = pathlib.Path(args.disk)
    if not image.is_file():
        # Also accept repo-relative paths such as assets/disk.raw.
        image = root / args.disk
    if not image.is_file():
        raise SystemExit(f"disk image missing: {args.disk}")
    target = parse_size(args.size)
    current = image.stat().st_size
    if target < current:
        raise SystemExit(f"refusing to shrink {image} from {current} to {target} bytes")

    start, size = partition_start_sectors(image)
    print(f"before: image {current / 1024 ** 3:.2f} GiB, partition 1 start {start} size {size * SECTOR / 1024 ** 3:.2f} GiB")

    if target > current:
        with image.open("r+b") as file:
            file.truncate(target)
        print(f"extended image file to {target / 1024 ** 3:.2f} GiB")

    # sfdisk -N rewrites the partition table entry only, so the payload stays
    # untouched; ", +" moves the end of partition 1 to the last disk sector.
    run("sfdisk", "--force", "-N", "1", str(image), input=", +\n")

    total_sectors = target // SECTOR
    start, size = partition_start_sectors(image)
    grown_end = start + size
    if grown_end < total_sectors - 34:
        print(f"warning: partition 1 ends at {grown_end}, disk holds {total_sectors} sectors")

    loop = run("losetup", "--find", "--show", "--partscan", str(image)).stdout.strip()
    try:
        partition = next(
            candidate
            for candidate in (f"{loop}p1", f"{loop}1")
            if os.path.exists(candidate)
        )
        # The partition is not mounted here, so a forced check is safe.
        subprocess.run(["e2fsck", "-f", "-y", partition], check=True, text=True)
        subprocess.run(["resize2fs", partition], check=True, text=True)
        subprocess.run(["e2fsck", "-f", "-y", partition], check=True, text=True)
    finally:
        run("losetup", "--detach", loop)

    start, size = partition_start_sectors(image)
    print(f"after: image {image.stat().st_size / 1024 ** 3:.2f} GiB, partition 1 {(start + size) * SECTOR / 1024 ** 3:.2f} GiB")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(f"command failed: {' '.join(error.cmd)}: {(error.stderr or '').strip()}")
