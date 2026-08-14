//Measuring how far an exchange's book separates from the one the feed describes.
//
//Two books are run over one stream, side by side, from the same starting point:
//
//  the mirror    NaiveBook. Reads IEX's executions and reproduces them. Cannot diverge,
//                because it is told the answer. This is the reference.
//
//  the exchange  ExchangeBook. Drops IEX's executions and decides its own. Can diverge,
//                and does, and by how much is the result.
//
//Both are fed identical client actions - adds, cancels, modifies - in identical order.
//The only difference is who decides the matches. So every divergence traces to a matching
//decision, which is what makes the comparison mean anything.
//
//Reconstructed takers drive the exchange. The feed never publishes an incoming order, but
//several executions stamped at the same nanosecond came from one, so grouping them
//recovers it - see match/taker.h. Those aggregates are submitted to the exchange as market
//orders; the decomposition IEX reported is held out and compared against what the exchange
//actually consumed.
//
//WHAT THIS IS NOT
//
//This is not matchcheck with the numbers renamed. matchcheck resyncs the book on truth
//after grading every taker, which keeps each comparison independent and is why its 100%
//means something. Here there is NO resync: a wrong decision at 09:31 stays wrong all day
//and poisons every decision after it. The two numbers answer different questions and both
//are worth having:
//
//  matchcheck   is price-time priority implemented correctly?          (a pass/fail gate)
//  drift        how long does a correct implementation stay in step
//               with a real venue it cannot fully observe?             (a measurement)
//
//A drift figure well short of 100% is therefore not a bug. Three causes are structural and
//known in advance: liquidity IEX never publishes, orders resting before the capture opened,
//and IEX mechanics not modelled here (the 350 us speed bump, D-Limit price sliding). The
//job of this harness is to size them, not to eliminate them.

#ifndef BENCH_DRIFT_H
#define BENCH_DRIFT_H

#include "books/book.h"

#include <cstdint>
#include <string>
#include <vector>

//Agreement between the two books at one point in the replay. Taken at intervals rather
//than continuously: comparing every instrument costs a full scan of both books, which is
//far more expensive than the replay it is measuring.
struct DriftSample {
    long long event_index;
    int64_t ts_ns;

    //The mirror is the reference, so its instruments are the denominator. An instrument
    //the exchange does not hold at all counts as a disagreement, not as an absence.
    size_t symbols;                 //Instruments the mirror holds
    size_t exchange_symbols;        //Instruments the exchange holds
    size_t price_agree;             //Same best bid and ask PRICE, and same emptiness
    size_t full_agree;              //Same prices AND same sizes at both

    long long mirror_orders;
    long long exchange_orders;
    long long mirror_shares;
    long long exchange_shares;
};


struct DriftReport {
    long long events;

    //--- order flow ---
    long long takers;               //Reconstructed from the feed
    long long submitted;            //Reached the exchange
    long long skipped_no_side;      //Direction unresolvable, so nothing could be sent

    //--- fill agreement, FREE RUNNING (no resync) ---
    //Only takers the feed described completely are graded. One whose sweep also hit
    //non-displayed liquidity is still submitted - those displayed shares really were
    //consumed - but its held-out answer is short by an unknown amount, so grading it
    //would score the exchange against a decomposition that is missing rows.
    long long graded;
    long long exact;
    long long wrong_orders;
    long long wrong_sizes;
    long long too_thin;
    long long ungraded_hidden;

    long long shares_expected;
    long long shares_right;

    //--- drift the exchange detected on its own ---
    //The feed published these orders as RESTING, so at IEX they crossed nothing. Every one
    //that crossed here is a direct observation of liquidity the exchange held and IEX did
    //not, and needs no second book to see.
    long long crossed_adds;
    long long crossed_add_shares;
    long long crossed_modifies;
    long long crossed_modify_shares;

    //Cancels naming an order the book no longer has. Flat in the mirror after the cold
    //start; in the exchange it climbs each time a wrong fill removed an order IEX kept.
    long long mirror_orphans;
    long long exchange_orphans;

    //--- conservation: a gate, not a measurement ---
    //Every submitted share must be traded, cancelled, or resting. This holds or it does
    //not, with no reference to IEX at all, and a failure is a bug in ExchangeBook.
    bool conserved;
    long long submitted_shares;
    long long accounted_shares;

    long long executes_dropped;     //IEX decisions the exchange took over

    //--- what the drift replay itself cost ---
    //
    //Untimed in the sense that it is not a latency figure - it holds two books at once and
    //scans both at intervals - but the resource cost is worth reporting, because running two
    //books is what sets the memory ceiling for the whole project.
    long long wall_ns;
    long long cpu_ns;
    long long rss_delta_bytes;      //What the two books plus the taker index added
    long long peak_rss_bytes;
    long long read_bytes;
    long long minor_faults;

    std::vector<DriftSample> samples;

    DriftReport()
        : events(0), takers(0), submitted(0), skipped_no_side(0),
          graded(0), exact(0), wrong_orders(0), wrong_sizes(0), too_thin(0),
          ungraded_hidden(0), shares_expected(0), shares_right(0),
          crossed_adds(0), crossed_add_shares(0),
          crossed_modifies(0), crossed_modify_shares(0),
          mirror_orphans(0), exchange_orphans(0),
          conserved(true), submitted_shares(0), accounted_shares(0),
          executes_dropped(0),
          wall_ns(-1), cpu_ns(-1), rss_delta_bytes(-1), peak_rss_bytes(-1),
          read_bytes(-1), minor_faults(-1) {}
};


//Replay `events` through both books, sampling agreement every `sample_every` events.
//A sample_every of 0 or less takes only the closing sample.
DriftReport measure_drift(const std::vector<Event>& events, long long sample_every);

#endif
