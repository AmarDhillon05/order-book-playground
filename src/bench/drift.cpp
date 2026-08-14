#include "bench/drift.h"

#include "bench/resources.h"
#include "books/exchange_book.h"
#include "books/naive_book.h"
#include "match/matcher.h"
#include "match/taker.h"

#include <chrono>

using namespace std;


//Compare the two books instrument by instrument.
//
//The mirror is the reference set: an instrument it holds that the exchange does not is a
//disagreement, and one the exchange holds that the mirror does not shows up only as a
//count difference. Both books create instruments from the same adds, so a large gap either
//way is itself a finding.
//
//Price agreement and full agreement are reported apart on purpose. Prices moving apart
//means the two books disagree about where the market IS; sizes moving apart while prices
//hold means they agree on the level and disagree on how much is on it, which is the
//milder failure and the one a single wrong fill produces first.
static DriftSample take_sample(long long index, int64_t ts_ns,
                          const books::NaiveBook& mirror,
                          const books::ExchangeBook& exch) {
    DriftSample s;
    s.event_index = index;
    s.ts_ns = ts_ns;
    s.symbols = 0;
    s.exchange_symbols = exch.get_symbol_count();
    s.price_agree = 0;
    s.full_agree = 0;
    s.mirror_orders = static_cast<long long>(mirror.get_order_count());
    s.exchange_orders = static_cast<long long>(exch.get_order_count());
    s.mirror_shares = mirror.audit_resting_shares();
    s.exchange_shares = exch.audit_resting_shares();

    vector<NamedTop> tops;
    mirror.get_all_tops(tops);

    s.symbols = tops.size();

    for(size_t i = 0; i < tops.size(); i++) {
        const TopOfBook& m = tops[i].second;
        TopOfBook e = exch.get_top_of_book(tops[i].first);

        //An empty side has to agree about being empty before its price means anything,
        //which is why has_bid/has_ask are compared rather than the prices alone
        bool prices = m.has_bid == e.has_bid
                   && m.has_ask == e.has_ask
                   && (!m.has_bid || m.bid_price == e.bid_price)
                   && (!m.has_ask || m.ask_price == e.ask_price);

        if(!prices) {
            continue;
        }

        s.price_agree++;

        if((!m.has_bid || m.bid_size == e.bid_size)
                && (!m.has_ask || m.ask_size == e.ask_size)) {
            s.full_agree++;
        }
    }

    return s;
}


DriftReport measure_drift(const vector<Event>& events, long long sample_every) {
    DriftReport rep;
    rep.events = static_cast<long long>(events.size());

    //Taken before the taker index is built, so the reported cost covers everything this
    //function allocates - both books AND the reconstruction - rather than the replay alone
    ResourceSnapshot before = snapshot_resources();
    chrono::steady_clock::time_point t0 = chrono::steady_clock::now();

    vector<match::Taker> takers;
    match::extract_takers(events, takers);
    rep.takers = static_cast<long long>(takers.size());

    //Neither book retains: the stream they are replaying is already held in `events`, and
    //two retained copies of a multi gigabyte vector is how this OOMs a 16 GB machine
    books::NaiveBook mirror(books::DROP_LOG);
    books::ExchangeBook exch(books::DROP_LOG);

    //Events pulled forward ahead of a match must not be applied twice when the replay
    //reaches them: a repeated modify would move an order that already moved, and a
    //repeated delete would land as an orphan in both books at once
    vector<bool> consumed(events.size(), false);

    size_t next = 0;

    for(size_t i = 0; i < events.size(); i++) {
        while(next < takers.size() && takers[next].first_index == i) {
            match::Taker& t = takers[next];

            //Bring both books to the state the exchange matched against: liquidity this
            //instrument lost during the pass is gone before the taker walks it. The
            //argument is unchanged from matchcheck - a matching pass is atomic, so
            //anything at the same instant that REMOVED liquidity must have preceded it,
            //or the taker would have consumed that order instead of stepping over it.
            for(size_t k = 0; k < t.presteps.size(); k++) {
                size_t p = t.presteps[k];

                if(!consumed[p]) {
                    mirror.apply(events[p]);
                    exch.apply(events[p]);
                    consumed[p] = true;
                }
            }

            //Resolved against the MIRROR, never against the exchange.
            //
            //An execution carries no side, so the only way to know which way the aggressor
            //ran is to look up its first victim and take the opposite. The exchange's book
            //may already have drifted and may not hold that order at all, so asking it
            //would make the drift measurement depend on the drift. This is the one piece of
            //held-out information that reaches the exchange, and it leaks direction only -
            //never ordering, which is what is being measured.
            char victim = mirror.get_order_side(t.expected[0].order_id);
            t.side = (victim == 'B') ? 'S' : (victim == 'S') ? 'B' : 0;

            if(t.side == 0) {
                //Normal at the head of a capture that starts mid session: the order was
                //added before the stream opened, so neither book ever held it
                rep.skipped_no_side++;
                next++;
                continue;
            }

            //A market order, because the feed never publishes a taker's limit price and
            //reading it off the worst price the sweep reached would hand the exchange the
            //depth it is supposed to be deciding. See match/taker.h.
            books::NewOrder n;
            n.symbol = t.symbol;
            n.side = t.side;
            n.size = t.size;
            n.price = 0;
            n.type = books::MARKET;
            n.ts_ns = t.ts_ns;
            n.client_id = 0;            //Exchange assigns; a market order never rests

            books::ExecReport er = exch.submit(n);
            rep.submitted++;

            if(t.hidden) {
                //Submitted but not graded. The displayed shares really were consumed, so
                //withholding the order would leave the exchange holding liquidity IEX had
                //taken - but the held-out answer is short by the non-displayed rows, so
                //grading against it would score a decomposition that is missing entries.
                rep.ungraded_hidden++;

            } else {
                match::Verdict v = match::grade(er.fills, t.expected);
                rep.graded++;

                switch(v) {
                    case match::EXACT:         rep.exact++; break;
                    case match::WRONG_ORDERS:  rep.wrong_orders++; break;
                    case match::WRONG_SIZES:   rep.wrong_sizes++; break;
                    case match::BOOK_TOO_THIN: rep.too_thin++; break;
                    default: break;
                }

                //Partial credit, per fill. A sweep that places three of four orders right
                //is not the same failure as one that places none, and an exact rate cannot
                //tell them apart.
                rep.shares_expected += t.size;

                for(size_t k = 0; k < er.fills.size() && k < t.expected.size(); k++) {
                    if(er.fills[k].order_id == t.expected[k].order_id
                            && er.fills[k].size == t.expected[k].size) {
                        rep.shares_right += er.fills[k].size;
                    }
                }
            }

            next++;
        }

        //No resync. The mirror advances on the feed's executions and the exchange does not
        //see them at all - it advanced on its own fills above, right or wrong. This one
        //line is the whole difference from matchcheck.
        if(!consumed[i]) {
            mirror.apply(events[i]);
            exch.apply(events[i]);
        }

        if(sample_every > 0
                && static_cast<long long>(i + 1) % sample_every == 0) {
            rep.samples.push_back(
                take_sample(static_cast<long long>(i),
                            events[i].has_ts ? events[i].ts_ns : 0,
                            mirror, exch));
        }
    }

    //Always close on a final sample, so a run shorter than one interval still reports
    //agreement rather than an empty series
    if(!events.empty()) {
        const Event& last = events.back();

        rep.samples.push_back(
            take_sample(static_cast<long long>(events.size() - 1),
                        last.has_ts ? last.ts_ns : 0,
                        mirror, exch));
    }

    rep.crossed_adds = exch.crossed_adds();
    rep.crossed_add_shares = exch.crossed_add_shares();
    rep.crossed_modifies = exch.crossed_modifies();
    rep.crossed_modify_shares = exch.crossed_modify_shares();
    rep.exchange_orphans = exch.orphans();
    rep.executes_dropped = exch.ignored_executes();

    rep.conserved = exch.conserved();
    rep.submitted_shares = exch.submitted_shares();
    rep.accounted_shares = exch.traded_taker_shares() + exch.traded_maker_shares()
                         + exch.cancelled_shares() + exch.resting_shares();

    //Reported as a pair. The mirror's count is the cold start floor - orders that were
    //resting before the capture opened and can never be found by either book - so the
    //exchange's EXCESS over it, not its absolute value, is the drift.
    rep.mirror_orphans = mirror.orphans();

    //Read while both books are still alive, so the RSS delta is what holding them costs
    chrono::steady_clock::time_point t1 = chrono::steady_clock::now();
    ResourceSnapshot after = snapshot_resources();

    rep.wall_ns = chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count();
    rep.peak_rss_bytes = after.peak_rss_bytes;

    if(before.cpu_ns >= 0 && after.cpu_ns >= 0) {
        rep.cpu_ns = after.cpu_ns - before.cpu_ns;
    }

    if(before.rss_bytes >= 0 && after.rss_bytes >= 0) {
        rep.rss_delta_bytes = after.rss_bytes - before.rss_bytes;
    }

    if(before.read_bytes >= 0) {
        rep.read_bytes = after.read_bytes - before.read_bytes;
    }

    if(before.minor_faults >= 0) {
        rep.minor_faults = after.minor_faults - before.minor_faults;
    }

    return rep;
}
