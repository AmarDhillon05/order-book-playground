#!/usr/bin/env python3
"""
Export a decoded IEX capture to CSV, so the C++ engine has a fixed, standardized input.

The point of exporting is that every run of every storage backend then reads the exact
same bytes. spec.md section 4 requires the input held constant across variants, and MR-3
requires every figure be reproducible from a documented command on a documented file.
The .meta.json written next to each CSV records both.

Everything stays an exact integer. Prices are in ticks of 1/10000 of a dollar, never a
decimal string, because parsing 136.8600 back through a float and rescaling can land on
1368599.9999. Timestamps are nanoseconds since epoch. An empty column means the message
type does not carry that field, not zero.

Columns:
  seq        per-message sequence number, contiguous unless the feed dropped
  ts_ns      exchange timestamp, nanoseconds since epoch
  kind       raw message type character: a R M L T for DPLS, 8 5 for DEEP
  symbol     8-character symbol, trailing spaces stripped
  side       B or S, empty where the message has no side
  order_id   empty on trade reports and on DEEP levels
  size       shares. On a DEEP level, 0 means the level was removed
  price      integer ticks, 1/10000 USD
  flags      raw flags byte as an integer
  xid        trailing id on an execution, or the trade id on a trade report

Everything downstream of here is ticker-specific, so --ticker also decides where the
export lands. IEX HIST has no per-ticker download - it serves whole-day captures - so
the filter happens here rather than at fetch time.

  data/20241223/NVDA/dpls.csv     one ticker's orders
  data/20241223/NVDA/deep.csv     the same ticker's levels, its oracle
  data/20241223/all/open_2m.csv   no --ticker, so cross-sectional

Takes a trading day, not a path. The whole DPLS feed is the default; --ticker is a
filter you opt into.

Usage:
  python3 loader/export_csv.py 20241223                        whole DPLS day
  python3 loader/export_csv.py 20241223 --feed DEEP            whole DEEP day
  python3 loader/export_csv.py 20241223 --ticker NVDA          one ticker
  python3 loader/export_csv.py 20241223 --at 0.05 --count 2000000 --out open_2m.csv
"""

import argparse
import json
import re
import sys
import time
from pathlib import Path

from iex_feed import Capture, FeedReader, FeedError, capture_for, human, clock, PRICE_SCALE


HEADER = "seq,ts_ns,kind,symbol,side,order_id,size,price,flags,xid"

#Rows are joined and written in batches. One write per row is dominated by call overhead
#at 50M rows, and holding the whole export in memory is not an option at this size.
BATCH = 65536


#Pull the feed name back out of IEX's own filename, 20241223_IEXTP1_DPLS1.0.pcap -> dpls
def feed_name(path):
    m = re.match(r"\d{8}_[A-Z0-9]+_([A-Z]+)", path.stem)

    return m.group(1).lower() if m else path.stem.lower()


#One folder per ticker, since everything downstream of here is ticker-specific. Exports
#with no ticker filter are cross-sectional and land in all/ instead.
def output_path(cap, ticker, out):
    root = cap.path.parent / (ticker.upper() if ticker else "all")

    if out:
        name = Path(out)
        return name if name.parent != Path(".") else root / name

    return root / f"{feed_name(cap.path)}.csv"


def row(e):
    return (f"{e.seq},"
            f"{e.ts_ns if e.ts_ns is not None else ''},"
            f"{e.kind},"
            f"{e.symbol or ''},"
            f"{e.side or ''},"
            f"{e.order_id if e.order_id is not None else ''},"
            f"{e.size if e.size is not None else ''},"
            f"{e.price if e.price is not None else ''},"
            f"{e.flags},"
            f"{e.trade_id if e.trade_id is not None else ''}")


def main():
    ap = argparse.ArgumentParser(description="Export a decoded IEX capture to CSV.")
    ap.add_argument("day", help="trading day as YYYYMMDD, or a path to a .pcap")
    ap.add_argument("--feed", default="DPLS", help="DPLS, DEEP or TOPS. Default DPLS.")
    ap.add_argument("--ticker", "--symbol", dest="ticker",
                    help="only this ticker. Also decides where the export lands: data/<date>/<TICKER>/")
    ap.add_argument("--out", help="override the output filename. Defaults to <feed>.csv in the ticker folder.")
    ap.add_argument("--at", type=float, default=None, help="start position in the file, 0.0 to 1.0")
    ap.add_argument("--count", type=int, default=None, help="stop after this many events")
    ap.add_argument("--kinds", help="only these message type characters, e.g. aRMLT")
    ap.add_argument("--through-restart", action="store_true",
                    help="keep exporting past a stream restart. Off by default, because the "
                         "sequence numbers begin again at 1 and the two streams cannot share a book.")

    if len(sys.argv) == 1:
        ap.print_help()
        return

    args = ap.parse_args()

    try:
        cap = Capture(capture_for(args.day, args.feed))

    except FeedError as err:
        sys.exit(str(err))

    dest = output_path(cap, args.ticker, args.out)
    dest.parent.mkdir(parents=True, exist_ok=True)

    start = None
    if args.at is not None:
        start = cap.resync(int(cap.size * args.at))
        if start is None:
            sys.exit(f"Could not resync at {args.at}")

    kinds = set(args.kinds) if args.kinds else None
    reader = FeedReader(cap)

    print(f"{cap.path.name} -> {dest}")
    started = time.monotonic()
    written = 0
    batch = []

    #reader.first/last track what was decoded, which includes the event that trips the
    #restart stop and is then thrown away. Meta has to describe the file, not the scan.
    first_row = last_row = None

    with open(dest, "w") as out:
        out.write(HEADER + "\n")

        for e in reader.events(start=start, max_events=args.count,
                               kinds=kinds, symbol=args.ticker):

            #Past a restart the sequence numbers begin again at 1 and the book is
            #meaningless. A test fixture must not silently contain both streams.
            if reader.restarts and not args.through_restart:
                print(f"\r  stopping at stream restart after {written + len(batch):,} rows"
                      f"{' ' * 20}")
                break

            if first_row is None:
                first_row = e

            last_row = e
            batch.append(row(e))

            if len(batch) >= BATCH:
                out.write("\n".join(batch) + "\n")
                written += len(batch)
                batch.clear()

                elapsed = time.monotonic() - started
                rate = written / elapsed if elapsed > 0 else 0
                print(f"\r  {written:,} rows  {rate / 1000:.0f}k rows/s", end="", flush=True)

        if batch:
            out.write("\n".join(batch) + "\n")
            written += len(batch)

    print(f"\r  {written:,} rows written{' ' * 20}")

    elapsed = time.monotonic() - started
    size = dest.stat().st_size

    #Provenance, so a number produced from this file can be traced back to a command
    meta = {
        "source": str(cap.path),
        "source_bytes": cap.size,
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "command": " ".join(["python3"] + sys.argv),
        "rows": written,
        "bytes": size,
        "filters": {
            "start_offset": start,
            "at": args.at,
            "count": args.count,
            "ticker": args.ticker,
            "kinds": args.kinds,
        },
        "packets_read": reader.packets_read,
        "events_matched": reader.events_read,
        "first_seq": first_row.seq if first_row else None,
        "last_seq": last_row.seq if last_row else None,
        "first_ts_ns": first_row.ts_ns if first_row else None,
        "last_ts_ns": last_row.ts_ns if last_row else None,
        "stopped_at_restart": bool(reader.restarts and not args.through_restart),
        "price_scale": PRICE_SCALE,
        "gaps": [g._asdict() for g in reader.gaps],
        "restarts": [r._asdict() for r in reader.restarts],
    }

    meta_path = dest.with_suffix(".meta.json")
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")

    print(f"  {human(size)} in {elapsed:.1f}s")

    if first_row:
        print(f"  seq {first_row.seq:,} .. {last_row.seq:,}")
        print(f"  {clock(first_row.ts_ns):%H:%M:%S} .. {clock(last_row.ts_ns):%H:%M:%S} ET")

    #FR-6 says detection is not optional, so a dirty export says so loudly
    if reader.gaps or reader.restarts:
        print(f"  WARNING {len(reader.gaps)} gap(s), {len(reader.restarts)} restart(s) "
              f"- see {meta_path.name}")
    else:
        print(f"  no gaps, no restarts")

    print(f"  meta -> {meta_path}")
    cap.close()


if __name__ == "__main__":
    main()
