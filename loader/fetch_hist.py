#!/usr/bin/env python3
"""
Fetch one IEX HIST pcap feed file and decompress it.

IEX publishes a daily pcap capture of each market data feed on a T+1 basis.
The HIST index at https://iextrading.com/api/1.0/hist maps every trading day
to a list of records, one per feed, each carrying a download link and the
exact gzipped byte count.

Feeds:
  DPLS  DEEP+, order level, every displayed order   <- what the engine wants
  DEEP  aggregated size per price level
  TOPS  top of book plus last sale

Usage:
  python3 loader/fetch_hist.py                      #print this help and exit
  python3 loader/fetch_hist.py --feed DPLS          #DPLS for the default test day
  python3 loader/fetch_hist.py --date 20241224      #a specific day
  python3 loader/fetch_hist.py --latest             #the newest published day
  python3 loader/fetch_hist.py --feed DEEP          #a different feed
  python3 loader/fetch_hist.py --list               #show what a day has

The date defaults to DEFAULT_DATE below, the cheapest DPLS day worth testing against.
DPLS itself only exists from 20241001 on; DEEP and TOPS go back to 2016 and are smaller,
but they are aggregated rather than order level.

Files land in data/YYYYMMDD/ as YYYYMMDD_IEXTP1_<FEED><ver>.pcap alongside the .gz.
One folder per trading day, so a day's capture and everything derived from it stay together.

Despite the .pcap extension these are pcapng (magic 0x0a0d0d0a, SHB/IDB/EPB blocks,
microsecond timestamps), Ethernet/IPv4/UDP carrying IEX-TP on one multicast group.
A reader written against the classic 24-byte pcap global header will not open them.
"""

import argparse
import gzip
import json
import shutil
import sys
import time
import urllib.request
from pathlib import Path


HIST_URL = "https://iextrading.com/api/1.0/hist"
CHUNK = 1 << 20                              #1 MiB
DECOMPRESS_RATIO = 3                          #Rough gz -> pcap multiplier, for the disk check

#Cheapest real day to test on, 1.69 GB gz. DPLS only goes back to 20241001 (a 638 KB
#launch-day partial), and nearly every session since runs 4-12 GB. This one is a quiet
#pre-holiday Monday, and notably smaller than the 20241224 half day.
#
#Confirmed partial, see resources/MEMORY.md. The stream opens 07:17 ET, runs dense through
#roughly 10:25, collapses to heartbeats, then restarts from sequence 1 at 11:33 and carries
#almost nothing for the rest of the day. What you get is pre-market, the open auction, and
#the first ~55 minutes: about 50.9M messages. Plenty for M1 and M2, but it is not a full
#session, so anything claiming full-day replay needs a different date.
DEFAULT_DATE = "20241223"


#Pull the HIST index, for one date or for every date
def fetch_index(date=None):
    url = HIST_URL if date is None else f"{HIST_URL}?date={date}"

    with urllib.request.urlopen(url, timeout=30) as resp:
        return json.load(resp)


#Resolve to the record list for a single day, plus the date it resolved to
def fetch_day(date=None):

    #No date given, so grab the whole index and take the most recent key
    if date is None:
        index = fetch_index()
        if not index:
            sys.exit("HIST index came back empty")

        date = max(index)
        return date, index[date]

    records = fetch_index(date)
    if not records:
        sys.exit(f"No HIST records for {date} (weekend, holiday, or not yet published)")

    return date, records


#Find one feed's record within a day
def select_feed(records, date, feed):
    for r in records:
        if r["feed"].upper() == feed.upper():
            return r

    available = ", ".join(sorted(r["feed"] for r in records))
    sys.exit(f"No {feed} feed on {date} (available: {available})")


def human(nbytes):
    for unit, scale in (("GB", 1e9), ("MB", 1e6), ("KB", 1e3)):
        if nbytes >= scale:
            return f"{nbytes / scale:.2f} {unit}"

    return f"{nbytes} B"


#Warn before committing to a download that will not fit
def check_disk(outdir, gz_size):
    needed = gz_size * (1 + DECOMPRESS_RATIO)
    free = shutil.disk_usage(outdir).free

    print(f"Disk: {human(free)} free, need roughly {human(needed)} for .gz plus .pcap")

    if free < needed:
        sys.exit("Not enough free space. Delete the .gz after decompressing, or point --out elsewhere.")


#Stream a URL to disk, reporting progress on one rewritten line
def download(url, dest, expected_size):

    #Already have the whole thing
    if dest.exists() and dest.stat().st_size == expected_size:
        print(f"{dest.name} already complete, skipping download")
        return

    if dest.exists():
        print(f"{dest.name} exists but is {dest.stat().st_size} bytes, expected {expected_size}, refetching")

    tmp = dest.with_suffix(dest.suffix + ".part")
    got = 0
    started = time.monotonic()

    with urllib.request.urlopen(url, timeout=60) as resp, open(tmp, "wb") as out:
        while True:
            buf = resp.read(CHUNK)
            if not buf:
                break

            out.write(buf)
            got += len(buf)

            elapsed = time.monotonic() - started
            rate = got / elapsed / 1e6 if elapsed > 0 else 0
            pct = 100 * got / expected_size if expected_size else 0
            print(f"\r  {human(got)} / {human(expected_size)}  ({pct:5.1f}%)  {rate:6.1f} MB/s", end="", flush=True)

    print()

    #The index publishes an exact size, so a mismatch means a truncated transfer
    if expected_size and got != expected_size:
        tmp.unlink()
        sys.exit(f"Size mismatch: got {got} bytes, index said {expected_size}. Transfer was truncated.")

    tmp.rename(dest)


#Stream gunzip, since these files are far too large to hold in memory
def decompress(src, dest):
    if dest.exists():
        print(f"{dest.name} already exists, skipping decompress")
        return

    tmp = dest.with_suffix(dest.suffix + ".part")
    started = time.monotonic()
    written = 0

    with gzip.open(src, "rb") as fin, open(tmp, "wb") as fout:
        while True:
            buf = fin.read(CHUNK)
            if not buf:
                break

            fout.write(buf)
            written += len(buf)

            elapsed = time.monotonic() - started
            rate = written / elapsed / 1e6 if elapsed > 0 else 0
            print(f"\r  {human(written)} written  {rate:6.1f} MB/s", end="", flush=True)

    print()
    tmp.rename(dest)


def main():
    ap = argparse.ArgumentParser(description="Fetch and decompress an IEX HIST pcap feed file.")
    ap.add_argument("--date", default=DEFAULT_DATE,
                    help=f"Trading day as YYYYMMDD. Default {DEFAULT_DATE}. Pass --latest for the newest published day.")
    ap.add_argument("--latest", action="store_true", help="Use the most recent published day instead of the default date.")
    ap.add_argument("--feed", default="DPLS", help="Feed to fetch: DPLS, DEEP, or TOPS. Default DPLS.")
    ap.add_argument("--out", default="data", help="Parent output directory. A per-date folder is created inside it. Default data/.")
    ap.add_argument("--list", action="store_true", help="List the feeds available for the day and exit.")
    ap.add_argument("--keep-gz", action="store_true", help="Keep the .gz after decompressing.")

    #A bare invocation would silently start a multi-GB download, so show the help instead
    if len(sys.argv) == 1:
        ap.print_help()
        return

    args = ap.parse_args()

    date, records = fetch_day(None if args.latest else args.date)

    #Just report what the day holds
    if args.list:
        print(f"{date}:")
        for r in sorted(records, key=lambda r: r["feed"]):
            print(f"  {r['feed']:6} v{r['version']:5} {human(int(r['size'])):>10} gz")
        return

    record = select_feed(records, date, args.feed)
    gz_size = int(record["size"])

    #One folder per trading day, so the capture and every log derived from it stay together
    outdir = Path(args.out) / date
    outdir.mkdir(parents=True, exist_ok=True)

    #Name files the way IEX names them, so the feed version stays visible on disk
    stem = f"{date}_{record['protocol']}_{record['feed']}{record['version']}"
    gz_path = outdir / f"{stem}.pcap.gz"
    pcap_path = outdir / f"{stem}.pcap"

    print(f"{record['feed']} v{record['version']} for {date}, {human(gz_size)} gzipped")
    check_disk(outdir, gz_size)

    print(f"\nDownloading -> {gz_path}")
    download(record["link"], gz_path, gz_size)

    print(f"\nDecompressing -> {pcap_path}")
    decompress(gz_path, pcap_path)

    if not args.keep_gz:
        gz_path.unlink()
        print(f"Removed {gz_path.name} (pass --keep-gz to keep it)")

    print(f"\nDone: {pcap_path} ({human(pcap_path.stat().st_size)})")


if __name__ == "__main__":
    main()
