# Data loader

Turns an IEX HIST capture into decoded events, and exports fixed CSV fixtures for the
C++ engine to replay.

```
fetch_hist.py    download a day's feed from IEX HIST -> data/YYYYMMDD/
iex_feed.py      parsing layer: pcapng -> IEX-TP -> Event.  No printing in it.
inspect_pcap.py  CLI for looking at a day (summary / types / dump / stream / at)
export_csv.py    write a CSV fixture + .meta.json provenance
```

## How the data flows

```
pcapng block -> Ethernet / IPv4 / UDP -> IEX-TP header -> length-prefixed messages -> Event
```

`Capture` owns the mmap and handles the first two layers. `FeedReader` handles the rest.

The capture files carry a `.pcap` extension but are **pcapng** (magic `0x0a0d0d0a`, SHB/IDB/EPB
blocks). A reader written against the classic 24-byte pcap header will not open them.

## Why FeedReader exists

Individual messages carry **no sequence number**. Only the IEX-TP packet header declares
`first_seq` and `message_count`. So `FeedReader` numbers each message `first_seq + i`, which
is the only way anything downstream can detect a gap.

- **Gap detection** — next packet's `first_seq` vs `prev.seq + prev.count`. Appends to `reader.gaps`.
- **Restart detection** — `stream_offset` going *backwards* means the feed restarted from
  scratch, so the gap check resets. Without that reset the 11:33 restart logs as one absurd
  50-million-message gap instead of what it actually is.

FR-6 requires detection, not recovery, so both are recorded on the reader rather than raised.
A caller checks `reader.gaps` after the run, or watches it grow mid-stream to mark book state
untrusted from that point.

## CSV schema

```
seq,ts_ns,kind,symbol,side,order_id,size,price,flags,xid
2326599,1734964314416362329,a,GOOG,B,177610710304446520,100,1948800,56,
2326601,1734964314416517027,R,GOOGL,,181832834955109672,,,32,
```

| Column | Notes |
|---|---|
| `seq` | per-message sequence number. Contiguous unless the feed dropped |
| `ts_ns` | exchange timestamp, ns since epoch. **Not** monotonic within a packet — see below |
| `kind` | raw type character, so C++ can `switch` on a byte. `a R M L T` for DPLS, `8 5` for DEEP |
| `symbol` | trailing spaces stripped |
| `side` | `B` / `S`, empty where the type has no side |
| `order_id` | empty on trade reports and DEEP levels |
| `size` | shares. On a DEEP level, `0` means the level was removed |
| `price` | **integer ticks, 1/10000 USD.** `1948800` is $194.88 |
| `flags` | raw flags byte as an integer |
| `xid` | trailing id on an execution, or the trade id on a trade report |

Everything is an exact integer on purpose. Prices are never decimal text, because parsing
`194.8800` back through a float and rescaling can land on `1948799.9999`. An empty column
means the message type does not carry that field, not zero — a `DELETE` genuinely carries
only an order id.

## Streaming

`FeedReader.batches()` yields events grouped **exactly as the wire delivered them** — one
datagram at a time, 1.23 messages per packet on average. A consumer that wants to behave
like a real feed handler needs to see that grouping, not a flattened stream.

`FeedReader.stream(speed=...)` adds pacing on top:

| speed | behaviour |
|---|---|
| `None` / `0` | as fast as possible, no sleeping |
| `1.0` | real time — the session takes as long as it originally did |
| `60` | a minute of market per second |

Pacing uses the **capture** timestamp, when the datagram hit the wire. The exchange
timestamp inside a message is when the matching engine acted and runs backwards within a
packet, so it is useless for arrival simulation.

```sh
python3 loader/inspect_pcap.py stream 20241223 --speed 60
python3 loader/inspect_pcap.py stream 20241223 --count 20
python3 loader/inspect_pcap.py stream 20241223 --tickers AAPL,MSFT --speed 1
```

`--tickers` is an optional filter, not a preset. Omit it and you get the whole feed.

## Running it

Every command takes a **trading day**, not a path, and reads the whole feed for that day.
No ticker is picked for you — the default is the entire DPLS stream.

```sh
python3 loader/fetch_hist.py --date 20241223           # download the day, ~1.5 GB gz
python3 loader/inspect_pcap.py summary 20241223        # coverage, gaps, restarts
python3 loader/inspect_pcap.py dump    20241223 --count 5000 --out day.log
python3 loader/inspect_pcap.py stream  20241223 --speed 60
python3 loader/inspect_pcap.py summary 20241223 --feed DEEP
```

`--feed` picks DPLS (default), DEEP or TOPS. A `.pcap` path still works anywhere a day does.
If the day is not downloaded, the error tells you the exact fetch command.

In code, `load_day("20241223")` returns a `FeedReader` over the whole capture.

## Exporting fixtures

`--ticker` filters *and* decides where the export lands. IEX HIST has no per-ticker
download — it serves whole-day, whole-market captures — so the filter happens here, which is
why a filtered export still scans all 6.7 GB and takes ~2.5 min.

```sh
python3 loader/export_csv.py 20241223                          # -> data/20241223/all/dpls.csv
python3 loader/export_csv.py 20241223 --ticker NVDA            # -> data/20241223/NVDA/dpls.csv
python3 loader/export_csv.py 20241223 --feed DEEP --ticker NVDA  # -> data/20241223/NVDA/deep.csv
```

A ticker's `dpls.csv` and `deep.csv` are the CR-1 pair: build the book from the orders,
aggregate by (side, price), and the totals must equal the levels at every message.

Throughput is ~128k rows/s. Every CSV gets a `.meta.json` recording source, exact command,
row count, seq range, and any gaps or restarts (MR-3).

## Gotchas

**The capture clock is TAI.** pcapng packet timestamps run exactly 37.000 s ahead of the
exchange timestamps inside the messages — TAI − UTC has been 37 s since 2017, and PTP-
disciplined capture hosts commonly log TAI. `iex_feed.TAI_UTC_OFFSET_NS` corrects it, after
which the residual is +53 to +94 µs of engine-to-wire latency. A naive pcapng reader will be
37 s off and will not notice, because the times still look like plausible market hours.

**Timestamps are not monotonic.** `ts_ns` is when IEX's matching engine acted, not send order.
Within a single packet you will see it go backwards. **`seq` is the ordering authority** —
sorting by `ts_ns` would silently corrupt the book.

**The exporter stops at a stream restart by default.** Pass `--through-restart` to override.
Without the stop a fixture would silently contain both streams, `seq` would jump from
50,878,185 back to 1, and a replay would build a corrupt book while looking fine. Meta records
`stopped_at_restart`.

**`TRADE` does not touch the book.** It is the tape print and carries no order id. `EXECUTE`
is the message that changes order state. `EXECUTE` is also *rarer* than `TRADE` — IEX runs
heavy non-displayed midpoint flow, so many prints have no corresponding displayed-order fill.
Book volume will legitimately not reconcile with tape volume.

**The `xid` on an EXECUTE does not join to a TRADE.** Over 60k contiguous events: zero shared
values. Three separate id spaces. It is not a foreign key.

**`MODIFY` tells you the queue priority rule — don't infer it.** Bit 0 of its flags byte is
`0` = reset priority, `1` = maintain priority. That is FR-2 delivered explicitly.

**`EXECUTE`'s price is the trade price, not the booking price.** Price improvement and price
sliding mean it can differ from where the order sits on the book. Never move a book level
with it. If the execution takes the order to 0 it leaves the book; otherwise the remainder
stays with the same time priority.

**`TRADE` covers non-displayed against non-displayed only.** That is why its trade ids never
collide with `EXECUTE`'s — they are disjoint sets of trades, not two views of one. The spec
says outright it can be ignored for book state.

**Field layouts are spec-confirmed.** Derived from the bytes first, then checked field by
field against IEX DEEP+ Specification v1.04. Every offset, length and flag value matched.
See `resources/MEMORY.md`.
