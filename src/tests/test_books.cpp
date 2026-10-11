//Unit tests for the books, on hand-built events rather than a capture.
//
//The replay harnesses (matchcheck, --drift) grade the books against IEX, but they need a
//multi-gigabyte HIST file that is not in the repository. These cover the same rules on a
//few orders each, so a fresh clone and CI can check the book logic without any data:
//price-time priority, modify priority, partial executions, cancels, clears, and that
//match() answers without mutating.
//
//Shared behaviour is written once as a template and run against every book, the same way
//the benchmark registry plugs books in.

#include "books/book.h"
#include "books/naive_book.h"
#include "books/exchange_book.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace std;
using namespace books;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if(!(cond)) { \
        failures++; \
        fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while(0)

static const char* SYM = "TEST";


//An event with only the fields a book reads filled in
static Event make_event(char kind, int64_t order_id, char side, uint32_t size, int64_t price) {
    Event e{};
    e.kind = kind;
    e.side = side;
    e.order_id = order_id;
    e.size = size;
    e.price = price;
    strncpy(e.symbol, SYM, sizeof(e.symbol) - 1);
    e.has_symbol = true;
    e.has_order_id = true;
    e.has_size = size != 0;
    e.has_price = price != 0;
    return e;
}

static Event add(int64_t id, char side, uint32_t size, int64_t price) {
    return make_event('a', id, side, size, price);
}

static Event cancel(int64_t id) {
    return make_event('R', id, 0, 0, 0);
}

static Event modify(int64_t id, uint32_t size, int64_t price, bool keep_priority) {
    Event e = make_event('M', id, 0, size, price);
    e.flags = keep_priority ? 1 : 0;
    return e;
}

static Event clear() {
    Event e = make_event('C', 0, 0, 0, 0);
    e.has_order_id = false;
    return e;
}


//Two bids and three asks, with two asks sharing the inside price so time priority matters.
//  bids: 11 x 200 (id 2), 10 x 100 (id 1)
//  asks: 12 x 300 (id 3, first), 12 x 100 (id 4, second), 13 x 50 (id 5)
template <typename B>
static void seed(B& b) {
    b.apply(add(1, 'B', 100, 10));
    b.apply(add(2, 'B', 200, 11));
    b.apply(add(3, 'S', 300, 12));
    b.apply(add(4, 'S', 100, 12));
    b.apply(add(5, 'S', 50, 13));
}

template <typename B>
static void test_empty_book() {
    B b(DROP_LOG);
    TopOfBook t = b.get_top_of_book(SYM);
    CHECK(!t.has_bid);
    CHECK(!t.has_ask);
    CHECK(!b.get_spread(SYM).valid);
}

template <typename B>
static void test_top_and_depth() {
    B b(DROP_LOG);
    seed(b);

    TopOfBook t = b.get_top_of_book(SYM);
    CHECK(t.has_bid && t.bid_price == 11 && t.bid_size == 200);
    CHECK(t.has_ask && t.ask_price == 12 && t.ask_size == 400);

    Spread s = b.get_spread(SYM);
    CHECK(s.valid && s.width == 1);

    vector<LevelView> asks;
    b.get_depth(SYM, 'S', 10, asks);
    CHECK(asks.size() == 2);
    CHECK(asks.size() == 2 && asks[0].price == 12 && asks[0].size == 400 && asks[0].order_count == 2);
    CHECK(asks.size() == 2 && asks[1].price == 13 && asks[1].size == 50);

    vector<LevelView> bids;
    b.get_depth(SYM, 'B', 1, bids);
    CHECK(bids.size() == 1 && bids[0].price == 11);
}

//A buy walks asks best price first and oldest first within a price, and leaves the book alone
template <typename B>
static void test_match_price_time_priority() {
    B b(DROP_LOG);
    seed(b);
    int64_t before = b.get_checksum();

    vector<Fill> fills;
    b.match(SYM, 'B', 420, fills);
    CHECK(fills.size() == 3);
    CHECK(fills.size() == 3 && fills[0].order_id == 3 && fills[0].size == 300 && fills[0].price == 12);
    CHECK(fills.size() == 3 && fills[1].order_id == 4 && fills[1].size == 100 && fills[1].price == 12);
    CHECK(fills.size() == 3 && fills[2].order_id == 5 && fills[2].size == 20 && fills[2].price == 13);

    CHECK(b.get_checksum() == before);
    CHECK(b.get_top_of_book(SYM).ask_size == 400);

    //Running out of liquidity is a short answer, not an error
    b.match(SYM, 'B', 10000, fills);
    uint32_t taken = 0;
    for(size_t i = 0; i < fills.size(); i++) {
        taken += fills[i].size;
    }
    CHECK(taken == 450);
}

template <typename B>
static void test_cancel() {
    B b(DROP_LOG);
    seed(b);
    b.apply(cancel(3));

    TopOfBook t = b.get_top_of_book(SYM);
    CHECK(t.ask_price == 12 && t.ask_size == 100);

    vector<Fill> fills;
    b.match(SYM, 'B', 1, fills);
    CHECK(fills.size() == 1 && fills[0].order_id == 4);
}

//A same-price reduction flagged to keep priority stays in front. A size increase, or a
//modify without the flag, goes to the back of its new level.
template <typename B>
static void test_modify_priority() {
    {
        B b(DROP_LOG);
        seed(b);
        b.apply(modify(3, 250, 12, true));

        vector<Fill> fills;
        b.match(SYM, 'B', 1, fills);
        CHECK(fills.size() == 1 && fills[0].order_id == 3);
        CHECK(b.get_top_of_book(SYM).ask_size == 350);
    }
    {
        B b(DROP_LOG);
        seed(b);
        b.apply(modify(3, 500, 12, true));

        vector<Fill> fills;
        b.match(SYM, 'B', 1, fills);
        CHECK(fills.size() == 1 && fills[0].order_id == 4);
    }
    {
        B b(DROP_LOG);
        seed(b);
        b.apply(modify(4, 100, 13, false));

        //Order 4 now rests at 13, behind order 5 which was there first
        TopOfBook t = b.get_top_of_book(SYM);
        CHECK(t.ask_price == 12 && t.ask_size == 300);

        vector<Fill> fills;
        b.match(SYM, 'B', 400, fills);
        CHECK(fills.size() == 3 && fills[0].order_id == 3 && fills[1].order_id == 5 &&
              fills[2].order_id == 4 && fills[2].size == 50);
    }
}

template <typename B>
static void test_clear() {
    B b(DROP_LOG);
    seed(b);
    b.apply(clear());

    TopOfBook t = b.get_top_of_book(SYM);
    CHECK(!t.has_bid);
    CHECK(!t.has_ask);
}

template <typename B>
static void run_shared(const char* name) {
    int before = failures;
    test_empty_book<B>();
    test_top_and_depth<B>();
    test_match_price_time_priority<B>();
    test_cancel<B>();
    test_modify_priority<B>();
    test_clear<B>();
    printf("  %-10s %s\n", name, failures == before ? "ok" : "FAILED");
}


//Executions are the mirror book's to apply. A partial fill shrinks the order in place and
//keeps its priority; a full fill removes it. Resting shares stay conserved throughout.
static void test_naive_executions() {
    int before = failures;
    NaiveBook b(DROP_LOG);
    seed(b);
    CHECK(b.audit_resting_shares() == 750);

    b.apply(make_event('L', 3, 0, 120, 12));
    CHECK(b.get_top_of_book(SYM).ask_size == 280);
    CHECK(b.audit_resting_shares() == 630);

    vector<Fill> fills;
    b.match(SYM, 'B', 1, fills);
    CHECK(fills.size() == 1 && fills[0].order_id == 3);

    b.apply(make_event('L', 3, 0, 180, 12));
    CHECK(b.get_top_of_book(SYM).ask_size == 100);
    CHECK(b.audit_resting_shares() == 450);
    CHECK(b.get_order_count() == 4);

    //An execute or cancel for an id that is not live is counted, not applied
    b.apply(make_event('L', 99, 0, 10, 12));
    CHECK(b.orphans() == 1);
    CHECK(b.audit_resting_shares() == 450);
    printf("  %-10s %s\n", "naive/exec", failures == before ? "ok" : "FAILED");
}

//The exchange book decides its own fills, so it must ignore IEX's executions
static void test_exchange_ignores_executions() {
    int before = failures;
    ExchangeBook b(DROP_LOG);
    seed(b);
    b.apply(make_event('L', 3, 0, 120, 12));
    CHECK(b.get_top_of_book(SYM).ask_size == 400);
    printf("  %-10s %s\n", "exch/exec", failures == before ? "ok" : "FAILED");
}


int main() {
    printf("book tests\n");
    run_shared<NaiveBook>("naive");
    run_shared<ExchangeBook>("exchange");
    test_naive_executions();
    test_exchange_ignores_executions();

    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
