# Order book engine over real IEX market data

> - A matching engine running free, with no resync, agreed with IEX on **99,122 of 99,123 fills over 51 million messages**. The single divergence never moved a top-of-book price.
> - The `std::map` mirror book costs **100 ns p50 / 1.0 µs p99 / 2.2 µs p99.9** per message (≈170 ns/event mean, pinned P-core).
> - Share conservation holds over 7.0 billion shares. Book rules are unit-tested in CI under ASan/UBSan.

A limit order book built from a real exchange feed, in two modes that answer different
questions.

**Mirror mode** reads IEX's DEEP+ (DPLS) feed and reproduces the book IEX had. It is told
what every match was, so it cannot get matching wrong — only bookkeeping.

**Exchange mode** throws away IEX's executions and matches for itself. Adds, cancels and
modifies are replayed as *client orders*; incoming aggressive orders are reconstructed from
the feed and submitted; every fill is this engine's own decision. It runs free with no
resync, so a wrong decision stays wrong for the rest of the day.

Running both over one stream and diffing them is the interesting part: it measures whether
a matching engine, given real order flow, stays in step with the venue that produced it.

```
                    ┌─────────────── one decoded stream ───────────────┐
                    │                                                  │
              mirror (naive)                                    exchange
        applies IEX's executions                    drops them, decides its own
                    │                                                  │
                    └──────────────► compared ◄───────────────────────┘
```

---

## Requirements

- **g++** with C++17 (or clang++; set `CXX`)
- **libpcap** development headers
- **Python 3.8+** for the data loader — standard library only, nothing to `pip install`
- **~8 GB free disk** per trading day, and see [memory](#memory) below

```sh
sudo apt install g++ make libpcap-dev     # Debian / Ubuntu
sudo dnf install gcc-c++ make libpcap-devel # Fedora
brew install libpcap                        # macOS
```

## Build

```sh
cd src
make
make test      # book unit tests on hand-built events; no capture needed
```

Four binaries land in `src/`:

| Binary | What it does |
|---|---|
| `bookbench` | benchmarks each book, and measures drift with `--drift` |
| `matchcheck` | grades matching against what IEX actually did |
| `dpls_dump` | prints decoded messages, one per line |
| `make_gap_fixture` | builds a capture with known gaps, to test gap detection |

## Get a day of data

IEX publishes a full pcap capture of each feed, free, on a T+1 basis. No account, no key.

```sh
python3 loader/fetch_hist.py --date 20241223
```

That is ~1.7 GB gzipped, ~6.7 GB unpacked, into `data/20241223/`. Pick a different day with
`--date`, the newest with `--latest`, or see what a day offers with `--list`.

> **DPLS only exists from 2024-10-01 onward.** It is the order-level feed and the only one
> this engine can build a book from. `DEEP` and `TOPS` go back to 2016 but are aggregated,
> not order-by-order.

Check what you got before doing anything else:

```sh
python3 loader/inspect_pcap.py summary 20241223
```

## Run it

Everything takes a path to the capture. **Start with `--events=N`** — the full stream is
~50.9 million messages and you do not want to wait for it on a first run.

### Look at the raw feed

```sh
cd src
./dpls_dump ../data/20241223/20241223_IEXTP1_DPLS1.0.pcap 20
```

### Benchmark the books

```sh
./bookbench ../data/20241223/20241223_IEXTP1_DPLS1.0.pcap --events=2000000
```

```
book           events      best ms   ns/event   events/sec       checksum
-------- ------------ ------------ ---------- ------------ --------------
null          2000000       12.200        6.1    163933190  2000001000000
naive         2000000      528.618      264.3      3783449     -160810524
exchange      2000000      749.853      374.9      2667190     -105738789
```

`null` stores nothing and is the floor — subtract it and what is left is what the data
structure actually costs. `--list` shows the registered books, `--books=naive,exchange`
selects some, `--reps=N` sets repetitions (best is reported).

The table above is one unpinned run. The saved reports in `src/logs/` were pinned to a
P-core (`taskset -c 2`), best of two repetitions. Under those conditions naive comes in at
170 ns/event and exchange at 240 ns/event, and the two reports agree within 0.5%.

`--latency` times every `apply()` with `rdtscp` into preallocated histograms. From
`src/logs/report_0001.txt`, 2M events, ns per message:

```
book       kind        p50     p90     p99   p99.9
--------   -------   -----   -----   -----   -----
null       all           8       8       9       9
naive      add         108     176    1088    2176
naive      delete       92     160    1024    2176
naive      all         100     168    1024    2176
exchange   add         136     288    1216    2304
exchange   all         120     256    1152    2304
```

`null` is the timer's own cost. The max column (not shown) reaches tens of milliseconds on
a few adds. The likely causes are hash map rehashing and first-touch page faults. Neither
has been confirmed yet; pre-reserving is the next experiment. Those runs used the powersave governor with turbo on, so read the tails as
indicative rather than tuned.

The `exchange` row is slower than `naive` because its `apply()` does strictly more work:
every add is offered up for matching before it rests. Timings jitter ~10% run to run on an
unpinned machine; the checksums do not.

Underneath it comes a resource table and a disk block:

```
resource use
book       book RSS   peak RSS    b/event  symbols  b/symbol   cpu util invol cs    disk rd
-------- ---------- ---------- ---------- -------- --------- ---------- -------- ----------
null         64.0 K    249.9 M        0.0        0         -     100.0%        0        0 B
naive       249.7 M    541.7 M      130.9     6598     39684     100.0%        5        0 B
exchange    396.7 M    722.3 M      208.0     6598     63049     100.0%        7        0 B

disk
  capture on disk                       6.2 G
  read from device, whole run          61.5 M
    requested via syscalls            273.3 M
  written to device                       0 B
  volume free (available)              75.6 G
  read amplification                    0.23x
```

## Performance metrics

Everything is read *around* the timed region, never inside it — a snapshot before the clock
starts and one after it stops, with the `/proc` reads outermost where they cost least.

| Column | Means |
|---|---|
| `book RSS` | resident memory the book added, **rep 0 only** |
| `peak RSS` | process high-water mark, whole run |
| `b/event` | book RSS ÷ events |
| `b/symbol` | book RSS ÷ instruments held |
| `cpu util` | CPU time ÷ wall time for the best repetition |
| `invol cs` | involuntary context switches — the scheduler preempted the measured region |
| `disk rd` | bytes actually fetched from the block device |

Four things to know before quoting any of it:

**`book RSS` includes the retained event log.** The harness default-constructs each book and
retention defaults on, so a book holds a *second* copy of the stream at 128 bytes per
message — which dominates the data structure itself. **`b/symbol` is therefore not the
memory-per-book figure yet**; getting there needs a book built with `DROP_LOG`, which the
generic harness cannot pass. The output says so too, so nobody quotes 39,684 as a per-book
cost.

**Memory figures are rep 0 only.** glibc does not reliably return freed pages to the kernel,
so after the first repetition there is no clean baseline and the delta reads far too low.

**`cpu util` is ~100% by construction** — the replay is single-threaded. The number is only
interesting when it *dips*, which means wall time passed that the process did not spend on a
CPU: page faults, I/O, or preemption. Read it next to `invol cs`. It can read a few
hundredths over 100% because the CPU bracket is a hair wider than the wall bracket; that is
instrument skew, not a reading.

**`read amplification` under 1.00x means the page cache served the capture**, so the timings
are measuring the book rather than the disk. Above 1.00x on a future store would be write
amplification — the thing a persistence layer has to be judged on.

### What is deliberately not here

Instructions per cycle, cache miss rates and branch misses need hardware counters. Use `perf`
around the binary rather than self-instrumentation — it is more accurate and keeps
`perf_event_open` off the measured path:

```sh
perf stat -e cycles,instructions,cache-misses,cache-references,branch-misses \
    ./bookbench ../data/20241223/20241223_IEXTP1_DPLS1.0.pcap --events=2000000
```

> The `exchange` row measures ingest only, because the benchmark loop calls `apply()` and
> nothing else. That means it matches feed adds but never sees reconstructed aggressive
> orders, so **its checksum is not the exchange's real book state** — use `--drift` for
> that.

### Grade the matching (mirror mode)

```sh
./matchcheck ../data/20241223/20241223_IEXTP1_DPLS1.0.pcap --events=2000000
```

IEX never publishes the incoming order that causes a trade — only the fills it caused. But
several fills stamped at the same nanosecond came from one order, so grouping them recovers
it. The aggregate (side, total shares) goes to the book; the decomposition (which orders, in
what sequence, for how much) is held back and compared against what the book chose.

The book is then **resynced on IEX's answer**, not its own, so each comparison is
independent and one wrong answer is one failure rather than the first of thousands.

### Measure drift (exchange mode)

```sh
./bookbench ../data/20241223/20241223_IEXTP1_DPLS1.0.pcap \
    --events=2000000 --reps=1 --books=null --drift-sample=250000
```

`--drift` runs an extra untimed replay holding *both* books at once. `--drift-sample=N`
takes an agreement reading every N events; `0` reports only the closing figure.

`--books=null` is worth passing: the timed books retain the event log, so skipping them
keeps peak memory for the two books that matter.

## The result

Over the full usable stream — 50,959,176 messages, 40 s, 8.4 GB peak:

| | |
|---|---|
| shares submitted | 6,996,617,788 |
| conservation of shares | **holds exactly** |
| takers reconstructed / submitted | 107,394 / 107,341 |
| fills graded, free running | 99,123 |
| **exact** | **99,122 — one divergence** |
| shares placed right | 16,433,036 / 16,433,036 (100.00%) |
| adds that crossed | 0 |
| modifies that crossed | 3 (400 shares) |
| top-of-book price agreement | 100.00% at every sample but the last |
| top-of-book price+size agreement | 99.99% |

The headline is that an engine running free, deciding every fill itself with nothing to
resync against, stays in step with a real venue across 51 million messages and diverges
**once**. That single bad decision leaves one order — 37 shares — resting that should have
been consumed, which is visible for the rest of the run as one instrument's top-of-book
*size* being wrong. It never moved a *price*: price agreement is 100.00% throughout.

For comparison, `matchcheck` scores 99,123 / 99,123 on the same stream. It resyncs on IEX's
answer after every taker, so it cannot accumulate error. The gap between the two figures —
one divergence — is the entire measured drift.

## Reading the drift output

```
conservation of shares HOLDS
  submitted                        6996617788
  traded + cancelled + resting     6996617788
```

**Read this first — everything below it is meaningless if it fails.** Every share ever
submitted must be traded, cancelled, or resting on the book: nothing created, nothing
vanished. This makes no reference to IEX at all, so a violation is a bug in the engine, not
a disagreement with the venue.

```
fill agreement, free running (no resync)
  graded                                99123
    exact                               99122  100.00%
    wrong sizes                             1    0.00%
```

Did the exchange consume the same orders, in the same sequence, for the same sizes as IEX?
**Not comparable to `matchcheck`'s figure** — that one resyncs after every taker, this one
never does, so errors here compound. A number below `matchcheck`'s is the expected outcome,
and the gap between them *is* the drift.

```
divergence the exchange saw by itself
  adds that crossed                         0  (0 shares)
  modifies that crossed                     3  (400 shares)
```

The neatest signal in the report. IEX published these orders as *resting*, so at IEX they
crossed nothing. Any that cross here prove this book held liquidity IEX's did not — drift
visible from the inside, with no second book needed to see it.

```
top-of-book agreement over the replay
          events  symbols     price   px+size   orders m/e   shares m/e
  -------------- -------- --------- --------- ------------ ------------
         5000000     7109   100.00%   100.00% 25013/25013  17081556/17081556
        25000000     7733   100.00%    99.99% 39779/39780  28817827/28817864
        50000000     8157   100.00%    99.99% 43398/43399  42825744/42825781
```

The curve, not just the endpoint: a book that diverged immediately and then held is a
different finding from one that decayed steadily. `price` is agreement on best bid and ask
*price*; `px+size` also requires the sizes to match, which is stricter and degrades first.

## What is actually verified

| Claim | Evidence |
|---|---|
| Price-time priority is correct | `matchcheck` — 100.00% exact over 99,123 comparable takers, 10,926 sweeps, 16.4M shares |
| It stays correct without a safety net | `--drift` — 99,122 / 99,123 with no resync, one divergence in 51M messages |
| No shares are created or destroyed | conservation identity over 6,996,617,788 submitted shares |
| Message layouts are right | derived from the bytes, then checked field by field against IEX DEEP+ Specification v1.04 — every offset, length and flag matched |
| Book rules hold in isolation | `make test`: price-time priority, modify priority, partial fills, cancels, clears, and a non-mutating `match()`, run against both books in CI, including under ASan/UBSan |
| Gap detection works | the real captures are clean, so `make_gap_fixture` builds a file with known holes and the reader is asserted to find exactly them |

## Limitations, stated plainly

- **The 2024-12-23 capture is not a full trading day.** It covers pre-market, the opening
  auction and roughly the first 55 minutes, then the message rate collapses and the stream
  **restarts at sequence 1** at 11:32:50 ET. Usable payload is ~50.9M messages.
- **Non-displayed liquidity is invisible.** IEX runs heavy midpoint and D-Limit flow that
  never appears on the feed, so neither book can contain it. Reconstructed aggressive orders
  are the *displayed residue* of the real order, not the real order.
- **The exchange does not simulate IEX.** IEX's 350 µs speed bump and D-Limit price sliding
  are not modelled and cannot be recovered from this feed. The honest claim is "price-time
  priority, implemented correctly, matching real order flow" — never "this is IEX".
- **One leak into the exchange.** An execution carries no side, so the direction of a
  reconstructed order is resolved by looking up its first victim in the mirror book. That
  leaks direction, never ordering — and ordering is what is measured.
- **Single machine, no NUMA testing, no frequency pinning** in the figures above.
- **Order types are limit, market and IOC.** No fill-or-kill, post-only, pegs, or
  self-match prevention (the feed carries no participant identity to build it from).

### Memory

The decoded stream is held in RAM at 128 bytes per message, and books retain it again by
default:

| Run | Peak RSS |
|---|---|
| `--events=10000000`, timed books plus drift | ~4.7 GB |
| `--drift` over the full stream with `--books=null` | ~8.4 GB |
| `matchcheck`, full stream | ~8.4 GB |

The full stream with several *retaining* books at once will exhaust a 16 GB machine — the
timed benchmark constructs each book with retention on, so it holds a second copy of the
event log. Use `--events=N` while iterating, and `--books=null` when measuring drift.

**The books are not the problem — retention is.** Measured at 10M events with `--books=null`,
the two drift books plus the taker index add **19.9 MB**, against a 1.2 GB peak. The books
hold only ~30k live orders because most orders are cancelled quickly; nearly all the memory
is the decoded event vector held in RAM as *input*.

That reframes the ceiling: it is an input-retention problem, not a data-structure problem, and
it is the strongest argument for spilling the log to disk rather than optimising the book.

## Gotchas that will bite you

**The capture clock is TAI, not UTC.** pcapng packet timestamps run exactly 37.000 s ahead
of the exchange timestamps inside the messages. That is not drift or capture lag — TAI − UTC
has been 37 s since 2017, and PTP-disciplined capture hosts commonly log TAI. A naive reader
is 37 s off and never notices, because the times still look like plausible market hours.

**Timestamps are not monotonic.** `ts_ns` is when the matching engine acted, and it runs
*backwards* within a single packet. Sequence number is the ordering authority; sorting by
timestamp silently corrupts the book. This is also why a point-in-time query cannot stop
early at the first message past the cutoff.

**The files are pcapng despite the `.pcap` extension.** Magic `0x0a0d0d0a`, SHB/IDB/EPB
blocks. A reader written against the classic 24-byte pcap global header will not open them.

**Prices are integers, not decimals.** Fixed point, four decimal places: `1948800` is
$194.88. Never round-trip them through a float.

**`MODIFY` states the queue priority rule — do not infer it.** Bit 0 of its flags byte is
`0` = reset priority, `1` = maintain priority.

**An execution's price is the *trade* price, not the booking price.** Price improvement can
move them apart, so never shift a book level with it.

**`TRADE` does not touch the book.** It reports a trade between two non-displayed orders, so
nothing visible moved. It is also *more common* than `EXECUTE` on IEX, which means book
volume legitimately will not reconcile with tape volume.

## Layout

```
src/
  books/       book.h is the contract every book satisfies
               naive_book   mirror: std::map levels + std::list time queues
               exchange_book authoritative matching, decides its own fills
               null_book    the control, stores nothing
  match/       taker reconstruction and match grading
  loader/      the pcapng / IEX-TP / message decoder
  bench/       timing harness and the drift measurement
loader/        Python: fetch a day, inspect it, export CSV fixtures
data/          downloaded captures (gitignored)
```

`src/books/book.h` is the place to start reading. It is the twelve-method contract, written
out as declarations with the reasoning for each, and it is checked against every book at
compile time — a book missing a method fails the build naming the method.

## License

Not yet specified.
