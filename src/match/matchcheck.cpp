//Check a matching engine against what IEX actually did.
//
//  ./matchcheck <capture.pcap> [--events=N] [--show=N]
//
//One replay, with a check fired at every execution instant. At each one the book is asked
//to match a taker reconstructed from that instant's executions, and the fills it produces
//are compared against the ones the feed reported.
//
//The ordering is the whole design. At an execution instant the check runs FIRST, against
//a book that has not yet seen any of that instant's executions, and only afterwards are
//the real executions applied to advance the book. Grading against truth and then
//resyncing to truth keeps every comparison independent: a wrong answer at 09:31 is one
//failure rather than the first of thirty thousand, and the counts below mean what they
//say. Letting the matcher's own output advance the book would be the natural shortcut and
//would make this report worthless.

#include "bench/bench.h"
#include "books/naive_book.h"
#include "match/matcher.h"
#include "match/taker.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

using namespace std;


static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s <capture.pcap> [options]\n"
            "\n"
            "  --events=N    replay at most N events (default: whole capture)\n"
            "  --show=N      print the first N disagreements (default: 10)\n",
            argv0);
}


//Print one disagreement side by side, so which orders moved and by how much is readable
//without going back to the capture
static void show(const match::Taker& t, const vector<Fill>& got,
             match::Verdict v) {
    printf("\n%-8s ts=%lld  taker %c %u shares  ->  %s\n",
           t.symbol.c_str(), (long long)t.ts_ns, t.side ? t.side : '?', t.size,
           match::verdict_name(v));

    size_t n = (got.size() > t.expected.size()) ? got.size() : t.expected.size();

    printf("     %-28s   %s\n", "matcher", "iex");

    for(size_t i = 0; i < n; i++) {
        char mine[32] = "-";
        char theirs[32] = "-";

        if(i < got.size()) {
            snprintf(mine, sizeof(mine), "%lld x%u",
                     (long long)got[i].order_id, got[i].size);
        }

        if(i < t.expected.size()) {
            snprintf(theirs, sizeof(theirs), "%lld x%u",
                     (long long)t.expected[i].order_id, t.expected[i].size);
        }

        bool same = (i < got.size() && i < t.expected.size()
                     && got[i].order_id == t.expected[i].order_id
                     && got[i].size == t.expected[i].size);

        printf("  %s  %-28s   %s\n", same ? " " : "!", mine, theirs);
    }
}


static void report(const match::MatchStats& s) {
    printf("\n%-24s %12s %8s\n", "outcome", "count", "share");
    printf("%-24s %12s %8s\n", "------------------------", "------------", "--------");

    long long comparable = s.exact + s.wrong_orders + s.wrong_sizes + s.book_too_thin;

    struct Row { const char* name; long long n; };

    Row rows[] = {
        { "exact",                s.exact },
        { "wrong-orders",         s.wrong_orders },
        { "wrong-sizes",          s.wrong_sizes },
        { "book-too-thin",        s.book_too_thin },
        { "skipped-hidden",       s.skipped_hidden },
        { "skipped-unknown-side", s.skipped_unknown_side },
    };

    for(size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        double pct = (s.takers > 0) ? 100.0 * rows[i].n / s.takers : 0.0;
        printf("%-24s %12lld %7.2f%%\n", rows[i].name, rows[i].n, pct);
    }

    printf("\n%-24s %12lld\n", "takers", s.takers);
    printf("%-24s %12lld\n", "  of which sweeps", s.sweeps);
    printf("%-24s %12lld\n", "comparable", comparable);
    printf("%-24s %12lld\n", "cancels pulled forward", s.presteps);

    if(comparable > 0) {
        printf("%-24s %11.2f%%\n", "  exact of comparable",
               100.0 * s.exact / comparable);
    }

    //The number the result actually rests on
    if(s.sweeps_comparable > 0) {
        printf("%-24s %11.2f%%  (%lld / %lld)\n", "  exact of sweeps",
               100.0 * s.sweeps_exact / s.sweeps_comparable,
               s.sweeps_exact, s.sweeps_comparable);
    }

    if(s.shares_seen > 0) {
        printf("%-24s %11.2f%%  (%lld / %lld)\n", "shares placed right",
               100.0 * s.shares_matched / s.shares_seen,
               s.shares_matched, s.shares_seen);
    }

    printf("\n");
}


int main(int argc, char** argv) {
    string path;
    long long limit = 0;
    long long show_n = 10;

    for(int i = 1; i < argc; i++) {
        string arg = argv[i];

        if(arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;

        } else if(arg.compare(0, 9, "--events=") == 0) {
            limit = atoll(arg.c_str() + 9);

        } else if(arg.compare(0, 7, "--show=") == 0) {
            show_n = atoll(arg.c_str() + 7);

        } else if(!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;

        } else if(path.empty()) {
            path = arg;

        } else {
            fprintf(stderr, "unexpected argument: %s\n", arg.c_str());
            return 2;
        }
    }

    if(path.empty()) {
        usage(argv[0]);
        return 2;
    }

    try {
        fprintf(stderr, "loading %s ...\n", path.c_str());

        vector<Event> events = load_events(path, limit);

        if(events.empty()) {
            fprintf(stderr, "error: capture yielded no events\n");
            return 1;
        }

        vector<match::Taker> takers;
        match::extract_takers(events, takers);

        fprintf(stderr, "%zu events, %zu taker(s) reconstructed\n",
                events.size(), takers.size());

        //No history is ever asked of this book, and the stream it replays is already held
        //in `events`, so retaining would just be a second copy of a multi gigabyte vector
        books::NaiveBook book(books::DROP_LOG);
        match::MatchStats stats;

        vector<Fill> got;
        size_t next = 0;
        long long shown = 0;

        //Events pulled forward ahead of a match must not be applied twice when the replay
        //reaches them: a repeated modify would move an order that already moved, and a
        //repeated delete would land as an orphan
        vector<bool> consumed(events.size(), false);

        for(size_t i = 0; i < events.size(); i++) {
            //Every group starting here is checked before the book moves. More than one
            //can share a starting index only if the stream repeats one, which it does
            //not, but the loop costs nothing and does not assume it.
            while(next < takers.size() && takers[next].first_index == i) {
                match::Taker& t = takers[next];

                stats.takers++;

                if(t.expected.size() > 1) {
                    stats.sweeps++;
                }

                //Bring the book to the state the exchange matched against: liquidity this
                //instrument lost during the pass is gone before the taker walks it
                for(size_t k = 0; k < t.presteps.size(); k++) {
                    size_t p = t.presteps[k];

                    if(!consumed[p]) {
                        book.apply(events[p]);
                        consumed[p] = true;
                        stats.presteps++;
                    }
                }

                //The side of the aggressor is the opposite of whatever its victims were
                //resting on, and the book is the only thing that knows which that was
                char victim = book.get_order_side(t.expected[0].order_id);
                t.side = (victim == 'B') ? 'S' : (victim == 'S') ? 'B' : 0;

                match::Verdict v;

                if(t.hidden) {
                    //Part of the real order filled off book, so the reconstructed size is
                    //short by an unknown amount and nothing here would be a fair test
                    v = match::SKIPPED_HIDDEN;
                    stats.skipped_hidden++;

                } else if(t.side == 0) {
                    //Normal at the head of a capture that starts mid session: the order
                    //was added before the stream opened, so the book never held it
                    v = match::SKIPPED_UNKNOWN_SIDE;
                    stats.skipped_unknown_side++;

                } else {
                    //The book matches itself. Nothing here knows how it is stored, which
                    //is what lets the same harness grade a different backend unchanged.
                    book.match(t.symbol, t.side, t.size, got);
                    v = match::grade(got, t.expected);

                    switch(v) {
                        case match::EXACT: stats.exact++; break;
                        case match::WRONG_ORDERS: stats.wrong_orders++; break;
                        case match::WRONG_SIZES: stats.wrong_sizes++; break;
                        case match::BOOK_TOO_THIN: stats.book_too_thin++; break;
                        default: break;
                    }

                    if(t.expected.size() > 1) {
                        stats.sweeps_comparable++;

                        if(v == match::EXACT) {
                            stats.sweeps_exact++;
                        }
                    }

                    //Partial credit, per fill. A sweep that gets three of four orders
                    //right is not the same failure as one that gets none, and the exact
                    //rate alone cannot tell them apart.
                    stats.shares_seen += t.size;

                    for(size_t k = 0; k < got.size() && k < t.expected.size(); k++) {
                        if(got[k].order_id == t.expected[k].order_id
                                && got[k].size == t.expected[k].size) {
                            stats.shares_matched += got[k].size;
                        }
                    }

                    if(v != match::EXACT && shown < show_n) {
                        show(t, got, v);
                        shown++;
                    }
                }

                next++;
            }

            //Resync on the feed's own answer, never on the matcher's
            if(!consumed[i]) {
                book.apply(events[i]);
            }
        }

        report(stats);

    } catch(const exception& err) {
        fprintf(stderr, "error: %s\n", err.what());
        return 1;
    }

    return 0;
}
