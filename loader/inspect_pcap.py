#!/usr/bin/env python3
"""
Inspect an IEX HIST capture and decode it into order events.

The HIST files carry a .pcap extension but are actually pcapng: a section header
block, an interface description block, then one enhanced packet block per datagram.
Each datagram is Ethernet / IPv4 / UDP carrying IEX-TP, and each IEX-TP payload is a
run of length-prefixed messages. This walks all four layers.

Two tricks make it fast on a multi-GB file. pcapng writes every block's total length
at both ends, so the capture can be walked backwards from EOF to find the last packet
without reading the whole thing. And a block boundary can be found at any byte offset
by scanning for a plausible EPB header and checking that its trailing length agrees
and that the next block looks sane, which makes sampling across the file cheap.

Commands:
  summary   coverage: time range, sequence range, stream restarts, sampled gaps
  types     message type and length histogram over a window
  dump      decode messages to a readable log
  stream    replay as arriving packets, optionally paced like a live feed
  at        resync at a byte offset and show what is there

Captures live in a per-date folder written by fetch_hist.py. A bare --out filename
lands in that same folder, so logs stay with the capture they came from.

Every command takes a trading day, and reads the whole feed for it. Pass --feed to
pick DEEP or TOPS instead of DPLS, or hand it a .pcap path directly.

Usage:
  python3 loader/inspect_pcap.py summary 20241223
  python3 loader/inspect_pcap.py types   20241223 --at 0.55
  python3 loader/inspect_pcap.py dump    20241223 --count 200 --out day.log
  python3 loader/inspect_pcap.py stream  20241223 --speed 60
  python3 loader/inspect_pcap.py at      20241223 --offset 3000000000
  python3 loader/inspect_pcap.py summary 20241223 --feed DEEP

Field layouts below were derived from the capture itself, not from the DEEP+ spec.
They decode to correct symbols, prices and timestamps, but the flags bytes on M, L
and I are still unconfirmed. See resources/MEMORY.md.
"""

import argparse
import collections
import struct
import sys
from pathlib import Path

from iex_feed import (Capture, FeedReader, FeedError, messages, decode, format_event,
                      clock, human, KIND_NAMES, PRICE_SCALE, capture_for)


#Sample the capture at even intervals plus the true tail, and report coverage
def cmd_summary(cap, args):
    print(f"{cap.path.name}  {human(cap.size)}  linktype={cap.linktype}  tsresol={cap.tsresol}")

    head = next(cap.packets(), None)
    if head is None:
        sys.exit("No IEX-TP packets found")

    tail = cap.last_packets(1)
    tail = tail[0] if tail else head

    print(f"protocol 0x{head.protocol:04x}  channel {head.channel}  "
          f"session 0x{head.session:08x}  group {head.group}:{head.port}\n")

    print(f"{'offset':>16} {'time':>12} {'seq':>13} {'streamoff':>15}  types")

    prev = None
    restarts = []
    rows = []

    for k in range(args.samples):
        target = int(cap.size * k / args.samples)
        pos = cap.resync(target)
        if pos is None:
            continue

        window = list(cap.packets(pos, limit=args.window))
        if not window:
            continue

        counts = collections.Counter()
        for pkt in window:
            for m in messages(pkt.payload):
                counts[chr(m[0])] += 1

        p = window[0]
        rows.append(p)
        top = " ".join(f"{t}:{c}" for t, c in counts.most_common(5))
        print(f"{p.offset:>16,} {clock(p.ts_ns):%H:%M:%S} {p.seq:>13,} {p.stream_offset:>15,}  {top}")

        #A stream that goes backwards has restarted, which is a gap of the worst kind
        if prev is not None and p.stream_offset < prev.stream_offset:
            restarts.append((prev, p))

        prev = p

    #The last sample lands short of EOF, so the tail has to be compared too or a
    #restart in the final stretch goes unreported
    if prev is not None and tail.stream_offset < prev.stream_offset:
        restarts.append((prev, tail))

    print(f"\n{'(tail)':>16} {clock(tail.ts_ns):%H:%M:%S} {tail.seq:>13,} {tail.stream_offset:>15,}")

    print(f"\nspan      {clock(head.ts_ns):%H:%M:%S} .. {clock(tail.ts_ns):%H:%M:%S} ET")
    print(f"messages  {max(r.seq for r in rows) if rows else 0:,} (peak sequence seen)")

    if restarts:
        print(f"\n{len(restarts)} stream restart(s) detected between samples:")
        for before, after in restarts:
            print(f"  {clock(before.ts_ns):%H:%M:%S} seq {before.seq:,} off {before.stream_offset:,}"
                  f"  ->  {clock(after.ts_ns):%H:%M:%S} seq {after.seq:,} off {after.stream_offset:,}")
        print("\nThe capture is not one continuous stream. Anything replaying it must")
        print("either stop at the restart or treat the book as untrusted past that point.")


#Histogram message types and lengths over one window
def cmd_types(cap, args):
    pos = cap.resync(int(cap.size * args.at))
    if pos is None:
        sys.exit("Could not resync")

    counts = collections.Counter()
    npkt = nmsg = 0
    gaps = 0
    expect = None
    first = last = None

    for pkt in cap.packets(pos, limit=args.window):
        if first is None:
            first = pkt

        last = pkt

        if pkt.count:
            if expect is not None and pkt.seq != expect:
                gaps += 1

            expect = pkt.seq + pkt.count

        for m in messages(pkt.payload):
            counts[(chr(m[0]), len(m))] += 1
            nmsg += 1

        npkt += 1

    print(f"window {clock(first.ts_ns):%H:%M:%S} .. {clock(last.ts_ns):%H:%M:%S} ET "
          f"at offset {pos:,}")
    print(f"packets {npkt:,}  messages {nmsg:,}  msg/packet {nmsg / max(npkt, 1):.2f}  "
          f"sequence gaps {gaps}\n")

    print(f"{'type':>6} {'name':>9} {'len':>5} {'count':>10} {'share':>8}")
    for (t, l), c in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"{t!r:>6} {KIND_NAMES.get(t, '?'):>9} {l:>5} {c:>10,} {100 * c / max(nmsg, 1):7.2f}%")


#Decode messages to a readable log
def cmd_dump(cap, args):
    pos = cap.first_block if args.at is None else cap.resync(int(cap.size * args.at))
    if pos is None:
        sys.exit("Could not resync")

    want = set(args.kinds) if args.kinds else None

    #A bare filename lands in the capture's own date folder, not the working directory
    dest = None
    if args.out:
        dest = Path(args.out)
        if dest.parent == Path("."):
            dest = cap.path.parent / dest

    out = open(dest, "w") if dest else sys.stdout
    written = 0

    try:
        for pkt in cap.packets(pos):
            if written >= args.count:
                break

            for m in messages(pkt.payload):
                if written >= args.count:
                    break

                e = decode(m)

                if want and e.kind not in want:
                    continue

                if args.symbol and e.symbol != args.symbol:
                    continue

                print(format_event(e), file=out)
                written += 1

    finally:
        if dest:
            out.close()
            print(f"wrote {written:,} events to {dest}")


#Replay the capture as arriving packets, to look like a live feed
def cmd_stream(cap, args):
    #Whole feed by default. --tickers is a filter you opt into, not a preset.
    tickers = args.tickers.split(",") if args.tickers else None

    start = cap.resync(int(cap.size * args.at)) if args.at else None
    reader = FeedReader(cap)

    label = ",".join(tickers) if tickers else "all symbols"
    pace = f"{args.speed}x" if args.speed else "unpaced"
    print(f"streaming {label} at {pace}, ctrl-c to stop\n")

    batches = 0

    for b in reader.stream(speed=args.speed, start=start, max_events=args.count,
                           symbols=tickers):
        batches += 1

        #One line per datagram, so the wire grouping stays visible
        head = f"{clock(b.ts_ns):%H:%M:%S.%f} seq={b.seq:<12,} n={len(b.events)}"
        print(head)

        for e in b.events:
            print(f"    {format_event(e)}")

    print(f"\n{batches:,} packets, {reader.events_read:,} events, "
          f"{len(reader.gaps)} gap(s), {len(reader.restarts)} restart(s)")


#Show what sits at one byte offset
def cmd_at(cap, args):
    pos = cap.resync(args.offset)
    if pos is None:
        sys.exit(f"Could not resync at {args.offset:,}")

    print(f"resynced {args.offset:,} -> {pos:,}\n")

    for pkt in cap.packets(pos, limit=args.window):
        print(f"@{pkt.offset:,} {clock(pkt.ts_ns):%H:%M:%S.%f} seq={pkt.seq:,} "
              f"count={pkt.count} streamoff={pkt.stream_offset:,}")

        for m in messages(pkt.payload):
            print(f"    {format_event(decode(m))}")


def main():
    ap = argparse.ArgumentParser(description="Inspect an IEX HIST pcapng capture.")
    sub = ap.add_subparsers(dest="cmd")

    s = sub.add_parser("summary", help="coverage, sequence range, stream restarts")
    s.add_argument("day", help="trading day as YYYYMMDD, or a path to a .pcap")
    s.add_argument("--feed", default="DPLS", help="DPLS, DEEP or TOPS. Default DPLS.")
    s.add_argument("--samples", type=int, default=16, help="sample points across the file")
    s.add_argument("--window", type=int, default=400, help="packets read per sample")
    s.set_defaults(fn=cmd_summary)

    s = sub.add_parser("types", help="message type and length histogram")
    s.add_argument("day", help="trading day as YYYYMMDD, or a path to a .pcap")
    s.add_argument("--feed", default="DPLS", help="DPLS, DEEP or TOPS. Default DPLS.")
    s.add_argument("--at", type=float, default=0.55, help="position in the file, 0.0 to 1.0")
    s.add_argument("--window", type=int, default=60000, help="packets to read")
    s.set_defaults(fn=cmd_types)

    s = sub.add_parser("dump", help="decode messages to a readable log")
    s.add_argument("day", help="trading day as YYYYMMDD, or a path to a .pcap")
    s.add_argument("--feed", default="DPLS", help="DPLS, DEEP or TOPS. Default DPLS.")
    s.add_argument("--at", type=float, default=None, help="position in the file, 0.0 to 1.0")
    s.add_argument("--count", type=int, default=100, help="events to write")
    s.add_argument("--symbol", help="only this symbol")
    s.add_argument("--kinds", help="only these message types, e.g. aRML")
    s.add_argument("--out", help="write to a file instead of stdout")
    s.set_defaults(fn=cmd_dump)

    s = sub.add_parser("stream", help="replay as arriving packets, like a live feed")
    s.add_argument("day", help="trading day as YYYYMMDD, or a path to a .pcap")
    s.add_argument("--feed", default="DPLS", help="DPLS, DEEP or TOPS. Default DPLS.")
    s.add_argument("--speed", type=float, default=None,
                   help="replay pace. 1.0 is real time, 60 is a minute of market per second, "
                        "omit for as fast as possible.")
    s.add_argument("--tickers", default=None,
                   help="optional comma separated filter. Default is every symbol in the feed.")
    s.add_argument("--at", type=float, default=None, help="start position in the file, 0.0 to 1.0")
    s.add_argument("--count", type=int, default=200, help="stop after this many events")
    s.set_defaults(fn=cmd_stream)

    s = sub.add_parser("at", help="resync at a byte offset and show what is there")
    s.add_argument("day", help="trading day as YYYYMMDD, or a path to a .pcap")
    s.add_argument("--feed", default="DPLS", help="DPLS, DEEP or TOPS. Default DPLS.")
    s.add_argument("--offset", type=int, required=True)
    s.add_argument("--window", type=int, default=3, help="packets to show")
    s.set_defaults(fn=cmd_at)

    #A bare invocation should explain itself rather than do nothing
    if len(sys.argv) == 1:
        ap.print_help()
        return

    args = ap.parse_args()
    if not args.cmd:
        ap.print_help()
        return

    try:
        cap = Capture(capture_for(args.day, args.feed))

    except FeedError as err:
        sys.exit(str(err))

    try:
        args.fn(cap, args)

    finally:
        cap.close()


if __name__ == "__main__":
    main()
