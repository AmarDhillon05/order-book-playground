#!/usr/bin/env python3
"""
Reader for IEX HIST captures. Turns a pcapng file into a stream of decoded events.

This is the parsing layer, with no printing in it, so the CLI in inspect_pcap.py, the
CSV exporter, and the reference book can all share one decode. There are four layers
between the file and an event:

  pcapng block -> Ethernet / IPv4 / UDP -> IEX-TP header -> length-prefixed messages

The IEX-TP header is the only place sequence numbers live, so gap and restart detection
has to happen here. FeedReader assigns every message its own sequence number, counting
up from the packet's first_seq, which makes a gap visible as a break in the event stream
rather than something a caller has to reconstruct.

Field layouts were originally derived from the captures themselves, then checked field
by field against the IEX DEEP+ Specification v1.04. Every offset, length and flag value
matches. See resources/MEMORY.md.
"""

import collections
import datetime
import mmap
import struct
import time
from pathlib import Path


SHB = 0x0A0D0D0A                             #Section header block
IDB = 0x00000001                             #Interface description block
EPB = 0x00000006                             #Enhanced packet block

ETH_LEN = 14
UDP_LEN = 8
IEXTP_LEN = 40

PRICE_SCALE = 10000                          #Prices are fixed point, 4 decimals
ET = datetime.timezone(datetime.timedelta(hours=-5))

#The pcapng packet timestamps run exactly 37.000 s ahead of the exchange timestamps
#carried inside the messages, constant to the millisecond over the whole capture and
#over three hours of it. That is TAI, not drift: TAI - UTC has been exactly 37 s since
#January 2017, and capture hosts disciplined by a PTP grandmaster commonly log TAI.
#Subtracting it here puts packet arrival and matching engine time on one clock, so the
#two are directly comparable and nothing downstream has to know.
TAI_UTC_OFFSET_NS = 37_000_000_000

#Second byte of an add order carries the side, as the ASCII digits IEX uses elsewhere
SIDE_BUY = 0x38                              #'8'
SIDE_SELL = 0x35                             #'5'


class FeedError(Exception):
    pass


def clock(ts_ns):
    return datetime.datetime.fromtimestamp(ts_ns / 1e9, datetime.timezone.utc).astimezone(ET)


def human(nbytes):
    for unit, scale in (("GB", 1e9), ("MB", 1e6), ("KB", 1e3)):
        if nbytes >= scale:
            return f"{nbytes / scale:.2f} {unit}"

    return f"{nbytes} B"


#One IEX-TP transport header, the envelope around a run of messages
Packet = collections.namedtuple(
    "Packet",
    "offset blocklen ts_ns protocol channel session count seq stream_offset payload group port"
)


class Capture:

    def __init__(self, path):
        self.path = Path(path)
        self.fh = open(self.path, "rb")
        self.mm = mmap.mmap(self.fh.fileno(), 0, access=mmap.ACCESS_READ)
        self.size = len(self.mm)
        self.tsresol = 1e-6
        self.linktype = None
        self.first_block = None

        self._read_prologue()


    def close(self):
        self.mm.close()
        self.fh.close()


    def __enter__(self):
        return self


    def __exit__(self, *exc):
        self.close()


    #Read the section and interface headers, and note where packets start
    def _read_prologue(self):
        magic = struct.unpack_from("<I", self.mm, 0)[0]
        if magic != SHB:
            raise FeedError(f"{self.path.name} is not pcapng (magic {magic:#x}). "
                            "A classic .pcap reader will not open these files either way.")

        pos = 0
        while pos < self.size:
            btype, blen = struct.unpack_from("<II", self.mm, pos)

            if btype == EPB:
                self.first_block = pos
                return

            if btype == IDB:
                self.linktype = struct.unpack_from("<H", self.mm, pos + 8)[0]
                self._read_tsresol(pos + 8 + 8, pos + blen - 4)

            pos += blen

        raise FeedError("No packet blocks found")


    #if_tsresol is option code 9, one byte, power of ten unless the high bit is set
    def _read_tsresol(self, pos, end):
        while pos + 4 <= end:
            ocode, olen = struct.unpack_from("<HH", self.mm, pos)
            if ocode == 0:
                return

            if ocode == 9 and olen == 1:
                r = self.mm[pos + 4]
                self.tsresol = 2.0 ** -(r & 0x7F) if r & 0x80 else 10.0 ** -(r & 0x7F)

            pos += 4 + olen + (-olen % 4)


    #Decode one enhanced packet block into a Packet, or None if it is not IEX-TP
    def packet_at(self, pos):
        btype, blen = struct.unpack_from("<II", self.mm, pos)
        if btype != EPB or not (32 <= blen <= 65536):
            return None

        caplen = struct.unpack_from("<I", self.mm, pos + 20)[0]
        tsh, tsl = struct.unpack_from("<II", self.mm, pos + 12)
        ts_ns = int(((tsh << 32) | tsl) * self.tsresol * 1e9) - TAI_UTC_OFFSET_NS

        base = pos + 28
        pkt = self.mm[base:base + caplen]

        #Ethernet II carrying IPv4 carrying UDP, nothing else on this feed
        if caplen < 42 or struct.unpack_from(">H", pkt, 12)[0] != 0x0800:
            return None

        ihl = (pkt[ETH_LEN] & 0x0F) * 4
        if pkt[ETH_LEN + 9] != 17:
            return None

        udp = ETH_LEN + ihl
        port = struct.unpack_from(">H", pkt, udp + 2)[0]
        group = ".".join(str(b) for b in pkt[ETH_LEN + 16:ETH_LEN + 20])

        p = udp + UDP_LEN
        if caplen - p < IEXTP_LEN:
            return None

        (ver, res, proto, channel, session,
         plen, count, stream_offset, seq, sendtime) = struct.unpack_from("<BBHIIHHqqq", pkt, p)

        payload = pkt[p + IEXTP_LEN:p + IEXTP_LEN + plen]

        return Packet(pos, blen, ts_ns, proto, channel, session, count,
                      seq, stream_offset, payload, group, port)


    #Walk packets forward from an offset, stopping after n or at EOF
    def packets(self, start=None, limit=None):
        pos = self.first_block if start is None else start
        n = 0

        while pos < self.size:
            if limit is not None and n >= limit:
                return

            pkt = self.packet_at(pos)
            if pkt is None:
                blen = struct.unpack_from("<I", self.mm, pos + 4)[0]
                if blen < 12:
                    return

                pos += blen
                continue

            yield pkt

            pos += pkt.blocklen
            n += 1


    #Find the next real block boundary at or after an arbitrary byte offset
    def resync(self, offset):
        offset = max(offset, self.first_block)
        window = self.mm[offset:offset + (1 << 20)]

        for i in range(len(window) - 48):
            if window[i:i + 4] != b"\x06\x00\x00\x00":
                continue

            blen = struct.unpack_from("<I", window, i + 4)[0]
            if not (32 <= blen <= 2048) or blen % 4 or i + blen + 8 > len(window):
                continue

            #The trailing copy of the length must agree, and the next block must look sane
            if struct.unpack_from("<I", window, i + blen - 4)[0] != blen:
                continue

            ntype, nlen = struct.unpack_from("<II", window, i + blen)
            if ntype == EPB and 32 <= nlen <= 2048 and nlen % 4 == 0:
                return offset + i

        return None


    #Walk backwards from EOF using the trailing length each block carries
    def last_packets(self, n=1):
        pos = self.size
        found = []

        while len(found) < n and pos > self.first_block:
            blen = struct.unpack_from("<I", self.mm, pos - 4)[0]
            start = pos - blen
            if blen < 12 or start < self.first_block:
                break

            if struct.unpack_from("<I", self.mm, start + 4)[0] != blen:
                break

            pkt = self.packet_at(start)
            if pkt is not None:
                found.append(pkt)

            pos = start

        return list(reversed(found))


#Split an IEX-TP payload into raw messages
def messages(payload):
    j = 0

    while j + 2 <= len(payload):
        mlen = struct.unpack_from("<H", payload, j)[0]
        j += 2

        if mlen == 0 or j + mlen > len(payload):
            return

        yield payload[j:j + mlen]
        j += mlen


KIND_NAMES = {
    #Book events, the ones that move order state
    "a": "ADD",
    "R": "DELETE",
    "M": "MODIFY",
    "L": "EXECUTE",
    "T": "TRADE",

    #DEEP only. The aggregated view: one message per price level, not per order.
    #The type char is the side, same '8'/'5' convention as an add order's flags byte.
    "8": "LEVEL",
    "5": "LEVEL",

    #Reference and status, mostly pre-open
    "S": "SYSTEM",
    "D": "DIRECTORY",
    "H": "STATUS",
    "O": "OPHALT",
    "P": "SSR",
    "I": "RETAIL",
    "E": "SECEVENT",
    "B": "BREAK",
    "C": "CLEARBOOK",
}

#Order Modify carries the queue priority rule in bit 0 of its flags byte. This is the
#FR-2 distinction: a size reduction can keep queue position, a price change cannot.
MODIFY_PRIORITY = {0: "reset-priority", 1: "maintain-priority"}

#Retail Liquidity Indicator, one byte of state per symbol
RETAIL_INDICATOR = {
    0x20: "none",
    0x41: "buy-interest",
    0x42: "sell-interest",
    0x43: "buy-and-sell-interest",
}

#The book events. Everything else is reference data and never touches order state.
BOOK_KINDS = set("aRMLT")

#The DEEP aggregated equivalents, which the oracle comparison reads instead
LEVEL_KINDS = set("85")

#Status types carry a single ASCII code in the flags byte rather than a bitfield
SYSTEM_EVENTS = {
    "O": "start-of-messages",
    "S": "start-of-system-hours",
    "R": "start-of-regular-hours",
    "M": "end-of-regular-hours",
    "E": "end-of-system-hours",
    "C": "end-of-messages",
}

TRADING_STATUS = {
    "H": "halted",
    "O": "order-acceptance",
    "P": "pre-cross",
    "T": "trading",
}

OP_HALT_STATUS = {
    "N": "not-halted",
    "O": "halted",
}


#ts_ns and price stay exact integers. Anything that formats them for a human does the
#conversion at the edge, so nothing downstream ever inherits a float rounding error.
Event = collections.namedtuple(
    "Event",
    "seq kind ts_ns symbol side order_id size price trade_id flags detail raw"
)


#Decode one message body. All types share type, flags, timestamp; most then carry a symbol.
def decode(m, seq=None):
    kind = chr(m[0])
    flags = m[1]
    code = chr(flags) if 32 <= flags < 127 else None

    ts_ns = struct.unpack_from("<q", m, 2)[0] if len(m) >= 10 else None
    symbol = m[10:18].decode("ascii", "replace").rstrip() if len(m) >= 18 else None

    side = order_id = size = price = trade_id = detail = None

    #System event is the only message with no symbol at all
    if kind == "S":
        detail = SYSTEM_EVENTS.get(code, code)

    elif kind == "H":
        reason = m[18:22].decode("ascii", "replace").strip()
        detail = TRADING_STATUS.get(code, code) + (f" reason={reason}" if reason else "")

    elif kind == "O":
        detail = OP_HALT_STATUS.get(code, code)

    elif kind == "P":
        detail = "in-effect" if flags else "not-in-effect"

    elif kind == "I":
        detail = RETAIL_INDICATOR.get(flags, f"0x{flags:02x}")

    #Security directory carries the round lot and a reference price rather than a book event
    elif kind == "D" and len(m) >= 31:
        lot = struct.unpack_from("<I", m, 18)[0]
        price = struct.unpack_from("<q", m, 22)[0]
        detail = f"roundlot={lot} luld_tier={m[30]}"

    #A DEEP price level update replaces the whole level. size 0 means the level is gone.
    elif kind in ("8", "5") and len(m) >= 30:
        side = "B" if kind == "8" else "S"
        size = struct.unpack_from("<I", m, 18)[0]
        price = struct.unpack_from("<q", m, 22)[0]
        if size == 0:
            detail = "level-removed"

    #Trade reports name a trade rather than an order, so they carry no order id
    elif kind == "T" and len(m) >= 38:
        size = struct.unpack_from("<I", m, 18)[0]
        price = struct.unpack_from("<q", m, 22)[0]
        trade_id = struct.unpack_from("<q", m, 30)[0]

    elif len(m) >= 26:
        order_id = struct.unpack_from("<q", m, 18)[0]

        if len(m) >= 38:
            size = struct.unpack_from("<I", m, 26)[0]
            price = struct.unpack_from("<q", m, 30)[0]

        #An execution carries the trade it was part of. Note the price here is the
        #TRADE price, which can differ from the order's booking price through price
        #improvement or sliding, so it must not be used to move the book level.
        if len(m) >= 46:
            trade_id = struct.unpack_from("<q", m, 38)[0]

    if kind == "a":
        side = "B" if flags == SIDE_BUY else "S" if flags == SIDE_SELL else "?"

    elif kind == "M":
        detail = MODIFY_PRIORITY.get(flags & 1)

    return Event(seq, kind, ts_ns, symbol, side, order_id, size, price,
                 trade_id, flags, detail, m)


Gap = collections.namedtuple("Gap", "expected got ts_ns offset missing")
Restart = collections.namedtuple("Restart", "prev_seq prev_offset ts_ns offset")

#One datagram's worth of events. ts_ns here is the capture timestamp, when the packet
#arrived, not the exchange timestamp carried inside the messages.
Batch = collections.namedtuple("Batch", "ts_ns seq offset events")


class FeedReader:
    """
    Walks a capture and yields decoded events in stream order, numbering each one.

    FR-6 requires gap detection, not recovery, so gaps and restarts are recorded on the
    reader rather than raised. A caller that cares checks reader.gaps after the run, or
    watches it grow mid-stream to mark book state untrusted from that point.
    """

    def __init__(self, path):
        self.cap = path if isinstance(path, Capture) else Capture(path)
        self.gaps = []
        self.restarts = []
        self.packets_read = 0
        self.events_read = 0
        self.first = None
        self.last = None


    def close(self):
        self.cap.close()


    def __enter__(self):
        return self


    def __exit__(self, *exc):
        self.close()


    #A bare string, an iterable, or None for everything
    @staticmethod
    def _want(symbols):
        if symbols is None:
            return None

        return {symbols} if isinstance(symbols, str) else set(symbols)


    def events(self, start=None, max_events=None, kinds=None, symbol=None, symbols=None):
        for batch in self.batches(start, max_events, kinds, symbols or symbol):
            for e in batch.events:
                yield e


    #Yield events grouped exactly as the wire delivered them. One datagram carried 1.23
    #messages on average, and a consumer that wants to look like a real feed handler has
    #to see that grouping rather than a flattened stream.
    def batches(self, start=None, max_events=None, kinds=None, symbols=None):
        want = self._want(symbols)
        expect = None
        prev = None

        for pkt in self.cap.packets(start):
            self.packets_read += 1

            #A stream offset that goes backwards means the feed restarted from scratch.
            #Sequence numbers restart with it, so the gap check has to reset too.
            if prev is not None and pkt.stream_offset < prev.stream_offset:
                self.restarts.append(Restart(prev.seq, prev.stream_offset,
                                             pkt.ts_ns, pkt.offset))
                expect = None

            #Heartbeats carry no messages and do not advance the sequence
            if pkt.count:
                if expect is not None and pkt.seq != expect:
                    self.gaps.append(Gap(expect, pkt.seq, pkt.ts_ns,
                                         pkt.offset, pkt.seq - expect))

                expect = pkt.seq + pkt.count

            prev = pkt

            keep = []

            for i, m in enumerate(messages(pkt.payload)):
                e = decode(m, seq=pkt.seq + i)

                if kinds and e.kind not in kinds:
                    continue

                if want and e.symbol not in want:
                    continue

                if self.first is None:
                    self.first = e

                self.last = e
                self.events_read += 1
                keep.append(e)

                if max_events is not None and self.events_read >= max_events:
                    break

            #Heartbeats and packets filtered down to nothing are not arrivals
            if keep:
                yield Batch(pkt.ts_ns, pkt.seq, pkt.offset, keep)

            if max_events is not None and self.events_read >= max_events:
                return


    #Replay batches at the pace the capture recorded, to look like a live feed.
    #
    #  speed None or 0   as fast as possible, no sleeping
    #  speed 1.0         real time, the session takes as long as it originally did
    #  speed 60.0        a minute of market in a second
    #
    #Pacing uses the pcapng capture timestamp, which is when the datagram actually hit
    #the wire. The exchange timestamp inside a message is when the matching engine acted
    #and runs backwards within a packet, so it is useless for arrival simulation.
    def stream(self, speed=None, start=None, max_events=None, kinds=None, symbols=None):
        wall_start = None
        capture_start = None

        for batch in self.batches(start, max_events, kinds, symbols):
            if speed:
                if wall_start is None:
                    wall_start = time.perf_counter()
                    capture_start = batch.ts_ns

                target = (batch.ts_ns - capture_start) / 1e9 / speed
                behind = target - (time.perf_counter() - wall_start)

                if behind > 0:
                    time.sleep(behind)

            yield batch


#Resolve a trading day to its capture file. Accepts either a date the loader has already
#downloaded, or a path straight to a .pcap, so callers can take one argument and not care.
def capture_for(day, feed="DPLS", root="data"):
    given = Path(day)

    if given.suffix == ".pcap":
        if not given.exists():
            raise FeedError(f"No such capture: {given}")

        return given

    folder = Path(root) / str(day)
    if not folder.is_dir():
        raise FeedError(f"No data for {day}. Fetch it first:\n"
                        f"  python3 loader/fetch_hist.py --date {day} --feed {feed}")

    found = sorted(folder.glob(f"*_{feed.upper()}*.pcap"))
    if not found:
        have = ", ".join(sorted(p.name for p in folder.glob("*.pcap"))) or "nothing"
        raise FeedError(f"No {feed} capture in {folder} (have: {have}). Fetch it:\n"
                        f"  python3 loader/fetch_hist.py --date {day} --feed {feed}")

    return found[0]


#Open a whole day's feed. One argument in, a reader over every packet in it out.
def load_day(day, feed="DPLS", root="data"):
    return FeedReader(capture_for(day, feed, root))


def format_event(e):
    name = KIND_NAMES.get(e.kind, f"?{e.kind}")

    if e.ts_ns is None:
        return f"{name:<9} flags=0x{e.flags:02x} raw={e.raw.hex()}"

    out = f"{clock(e.ts_ns):%H:%M:%S.%f} {name:<9} {(e.symbol or ''):<8}"

    if e.side:
        out += f" {e.side}"

    if e.order_id is not None:
        out += f" oid={e.order_id}"

    if e.size is not None:
        out += f" size={e.size}"

    if e.price is not None:
        out += f" px={e.price / PRICE_SCALE:.4f}"

    #Both are genuine Trade IDs. They never collide because L covers executions against
    #a displayed order and T covers non-displayed trading against non-displayed, which
    #are disjoint sets of trades, not two views of the same one.
    if e.trade_id is not None:
        out += f" trade={e.trade_id}"

    if e.detail:
        out += f" {e.detail}"

    #Side already says what the flags byte meant on an add; elsewhere it still matters
    if e.kind == "T":
        out += f" cond=0x{e.flags:02x}"

    elif e.kind in ("R", "M", "L", "I"):
        out += f" flags=0x{e.flags:02x}"

    return out
