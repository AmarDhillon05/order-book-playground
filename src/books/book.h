//The contract every book in this directory satisfies.
//
//BookModel below is the whole surface, written out as declarations. It is never
//instantiated and never inherited; it exists so the required API can be read off one
//screen instead of reconstructed from whichever implementation happens to be open. A new
//book is written by copying BookModel's declarations and filling them in.
//
//BookContract<B> then checks a real book against that surface at compile time. It is
//instantiated once per registered book inside run_book(), so a book missing a method, or
//carrying one with the wrong return type or constness, fails the build with a message
//naming the method rather than with a page of template noise.
//
//This is a compile time contract, not an abstract base class, and the check is kept
//separate from the implementation rather than inherited by it. Books are plugged into
//the benchmark as template arguments so the replay loop calls apply() directly, with
//nothing virtual in the way. That matters here: a virtual call costs on the same order
//as the map lookup being measured, and it would inline differently per backend, so
//paying it per event would fold dispatch cost into every number and blur the difference
//the study exists to expose.
//
//Swapping storage still costs nothing outside this directory. Ingestion, query and
//benchmark code are written once against the names below; putting a new backend under
//test is one line in the benchmark registry and nothing else.
//
//If a runtime polymorphic book is ever wanted, wrap a backend in an adapter deriving from
//an IBook and register the adapter as one more entry. The vtable cost then shows up as
//its own row rather than as a tax on every row.

#ifndef BOOKS_BOOK_H
#define BOOKS_BOOK_H

#include "loader/dpls_reader.h"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

//Best bid and ask. has_bid / has_ask keep an empty side distinct from a zero price.
struct TopOfBook {
    int64_t bid_price;
    uint32_t bid_size;
    int64_t ask_price;
    uint32_t ask_size;
    bool has_bid;
    bool has_ask;
};

//One rung of a depth snapshot, aggregated across the orders resting at that price
struct LevelView {
    int64_t price;
    uint32_t size;
    uint32_t order_count;
};

//A named top of book, as returned by the cross instrument scan
typedef std::pair<std::string, TopOfBook> NamedTop;


//One resting order consumed by an incoming order. Unlike LevelView this does not
//aggregate: matching happens against individual orders, and which ones were taken in
//which sequence is the whole answer.
//
//price is the resting order's booking price. An execution on the wire reports the TRADE
//price instead, which price improvement can move off it, so the two are not comparable
//and nothing should assert across them.
struct Fill {
    int64_t order_id;
    uint32_t size;
    int64_t price;
};


//The distance between the two sides. Invalid whenever either side is empty, which is
//common outside regular hours and on thin names, and is not the same as a zero spread.
struct Spread {
    int64_t bid_price;
    int64_t ask_price;
    int64_t width;                  //ask - bid, in the same integer ticks
    bool valid;
};


//Filter for an event query. Every field is optional and an absent one does not
//constrain; a default constructed query therefore asks for the whole retained stream.
//
//Times are half open, [from, to), so two adjacent windows tile the stream without
//double counting the boundary. Both bounds read e.ts_ns, the exchange clock carried in
//the message, not the capture clock.
//
//kinds is a set of raw type characters rather than an enum, matching what the decoder
//puts in Event::kind. "aRML" selects the order lifecycle and drops everything else.
//
//An event carrying no symbol, such as a system message, is excluded whenever symbol is
//set; one carrying no timestamp is excluded whenever either time bound is set. Absent is
//treated as not matching rather than as matching everything, so a filtered query never
//returns rows it could not have filtered on.
struct EventQuery {
    std::optional<std::string> symbol;
    std::optional<std::string> kinds;
    std::optional<int64_t> from_ts_ns;
    std::optional<int64_t> to_ts_ns;
};


//One point in a historical series. The timestamp is the exchange time of the message
//that produced the value.
struct SpreadSample {
    int64_t ts_ns;
    Spread spread;
};

struct TopSample {
    int64_t ts_ns;
    TopOfBook top;
};


//The contract, in full. Declarations only: this class has no definitions and cannot be
//constructed. Every book in this directory implements exactly these twelve: one to
//ingest, four to read present state, one to match, five to read history, and one digest.
class BookModel {
public:
    //Ingest one decoded message. Called in strict sequence order, so a book may assume
    //it never sees the stream out of order. Messages carrying no book effect are the
    //book's to ignore.
    void apply(const Event& e);

    //Best bid and ask for one instrument. An unknown symbol is not an error; it comes
    //back with has_bid and has_ask false.
    TopOfBook get_top_of_book(const std::string& symbol) const;

    //Up to n levels from the inside out, on side 'B' or 'S'. Clears out first.
    void get_depth(const std::string& symbol, char side, size_t n,
               std::vector<LevelView>& out) const;

    //Top of book for every instrument currently held. Clears out first.
    void get_all_tops(std::vector<NamedTop>& out) const;

    //Current spread for one instrument. Equivalent to reading top_of_book and
    //subtracting, but stated separately because a book may track it directly.
    Spread get_spread(const std::string& symbol) const;


    //Match an incoming order of `size` shares on `side` against the resting liquidity it
    //would consume, appending one Fill per order taken, in the order taken. Clears out
    //first. Does NOT mutate the book: this answers what would happen, and the caller
    //decides whether anything should.
    //
    //Part of the contract rather than a free function over some exposed queue, because
    //the walk is where a storage strategy is most exposed. An ordered map of linked lists
    //chases a pointer per order across two levels of indirection; a dense array reads a
    //contiguous run. Those are the same operation with different cache behaviour, which
    //is the study's subject, so each backend has to implement its own and be measured on
    //it. It also keeps the queues private - nothing outside a book needs to see them.
    //
    //No limit price, deliberately. Depth reached is an output, and a caller replaying a
    //real order already knows how deep it went; passing that in would hand the matcher
    //the answer. Running out of resting liquidity is a short result, not an error.
    void match(const std::string& symbol, char side, uint32_t size,
           std::vector<Fill>& out) const;


    //Everything below reads history rather than present state, so a book has to retain
    //enough of the stream to answer. How it does that is the backend's business and is
    //one of the things worth measuring: keeping the raw log and replaying is cheap to
    //ingest and slow to query, while recording a change series as events arrive inverts
    //both. Only the answers are fixed here.

    //Every retained event matching the filter, in the order it was ingested. Clears out
    //first. A default constructed query returns the whole stream.
    void get_events(const EventQuery& q, std::vector<Event>& out) const;

    //State as of an instant: the book as it stood after every event timestamped at or
    //before ts_ns had been applied. Inclusive of ts_ns, so querying the exact timestamp
    //of an event sees that event's effect.
    //
    //Sequence order decides, not timestamp order. Exchange timestamps are not monotonic
    //within a packet, so "up to ts_ns" cannot be treated as a prefix of the stream; it
    //is every event at or below that time, applied in the order it arrived.
    TopOfBook get_top_of_book_at(const std::string& symbol, int64_t ts_ns) const;
    Spread get_spread_at(const std::string& symbol, int64_t ts_ns) const;

    //Every point in [from_ts_ns, to_ts_ns) at which the value changed, in stream order.
    //Clears out first. A window in which nothing moved comes back empty rather than as
    //one repeated sample, so the series is a change log and not a fixed rate sampling.
    void get_top_of_book_series(const std::string& symbol, int64_t from_ts_ns,
                            int64_t to_ts_ns, std::vector<TopSample>& out) const;

    void get_spread_series(const std::string& symbol, int64_t from_ts_ns, int64_t to_ts_ns,
                       std::vector<SpreadSample>& out) const;

    //Cheap digest of book state. Not a correctness test. It exists so the optimizer
    //cannot delete a book whose state is never read, and so two books fed the same
    //stream can be compared coarsely. Never called inside a timed region.
    int64_t get_checksum() const;

private:
    BookModel();
};


//Detection of each method, by call expression and return type. A book taking its
//arguments by different but compatible types still passes, which is deliberate: what is
//pinned down is the call the benchmark makes and the type it gets back.
namespace book_detail {

template <class B, class = void>
struct has_apply : std::false_type {};

template <class B>
struct has_apply<B, std::void_t<decltype(
        std::declval<B&>().apply(std::declval<const Event&>()))> >
    : std::is_same<void, decltype(
        std::declval<B&>().apply(std::declval<const Event&>()))> {};


template <class B, class = void>
struct has_get_top_of_book : std::false_type {};

template <class B>
struct has_get_top_of_book<B, std::void_t<decltype(
        std::declval<const B&>().get_top_of_book(std::declval<const std::string&>()))> >
    : std::is_same<TopOfBook, decltype(
        std::declval<const B&>().get_top_of_book(std::declval<const std::string&>()))> {};


template <class B, class = void>
struct has_get_depth : std::false_type {};

template <class B>
struct has_get_depth<B, std::void_t<decltype(
        std::declval<const B&>().get_depth(std::declval<const std::string&>(), 'B',
                                       size_t(0),
                                       std::declval<std::vector<LevelView>&>()))> >
    : std::true_type {};


template <class B, class = void>
struct has_get_all_tops : std::false_type {};

template <class B>
struct has_get_all_tops<B, std::void_t<decltype(
        std::declval<const B&>().get_all_tops(std::declval<std::vector<NamedTop>&>()))> >
    : std::true_type {};


template <class B, class = void>
struct has_get_spread : std::false_type {};

template <class B>
struct has_get_spread<B, std::void_t<decltype(
        std::declval<const B&>().get_spread(std::declval<const std::string&>()))> >
    : std::is_same<Spread, decltype(
        std::declval<const B&>().get_spread(std::declval<const std::string&>()))> {};


template <class B, class = void>
struct has_match : std::false_type {};

template <class B>
struct has_match<B, std::void_t<decltype(
        std::declval<const B&>().match(std::declval<const std::string&>(), 'B',
                                   uint32_t(0), std::declval<std::vector<Fill>&>()))> >
    : std::true_type {};


template <class B, class = void>
struct has_get_events : std::false_type {};

template <class B>
struct has_get_events<B, std::void_t<decltype(
        std::declval<const B&>().get_events(std::declval<const EventQuery&>(),
                                        std::declval<std::vector<Event>&>()))> >
    : std::true_type {};


template <class B, class = void>
struct has_get_top_of_book_at : std::false_type {};

template <class B>
struct has_get_top_of_book_at<B, std::void_t<decltype(
        std::declval<const B&>().get_top_of_book_at(std::declval<const std::string&>(),
                                                int64_t(0)))> >
    : std::is_same<TopOfBook, decltype(
        std::declval<const B&>().get_top_of_book_at(std::declval<const std::string&>(),
                                                int64_t(0)))> {};


template <class B, class = void>
struct has_get_spread_at : std::false_type {};

template <class B>
struct has_get_spread_at<B, std::void_t<decltype(
        std::declval<const B&>().get_spread_at(std::declval<const std::string&>(),
                                           int64_t(0)))> >
    : std::is_same<Spread, decltype(
        std::declval<const B&>().get_spread_at(std::declval<const std::string&>(),
                                           int64_t(0)))> {};


template <class B, class = void>
struct has_get_top_of_book_series : std::false_type {};

template <class B>
struct has_get_top_of_book_series<B, std::void_t<decltype(
        std::declval<const B&>().get_top_of_book_series(
            std::declval<const std::string&>(), int64_t(0), int64_t(0),
            std::declval<std::vector<TopSample>&>()))> >
    : std::true_type {};


template <class B, class = void>
struct has_get_spread_series : std::false_type {};

template <class B>
struct has_get_spread_series<B, std::void_t<decltype(
        std::declval<const B&>().get_spread_series(
            std::declval<const std::string&>(), int64_t(0), int64_t(0),
            std::declval<std::vector<SpreadSample>&>()))> >
    : std::true_type {};


template <class B, class = void>
struct has_get_checksum : std::false_type {};

template <class B>
struct has_get_checksum<B, std::void_t<decltype(std::declval<const B&>().get_checksum())> >
    : std::is_same<int64_t, decltype(std::declval<const B&>().get_checksum())> {};

} //namespace book_detail


//Instantiate this against a book to check it. run_book() does so for every backend it is
//asked to time, so registering a nonconforming book is a build error rather than a
//surprise at the bottom of a results table.
template <class B>
struct BookContract {
    static_assert(book_detail::has_apply<B>::value,
        "book is missing: void apply(const Event&)");

    static_assert(book_detail::has_get_top_of_book<B>::value,
        "book is missing: TopOfBook get_top_of_book(const std::string&) const");

    static_assert(book_detail::has_get_depth<B>::value,
        "book is missing: void get_depth(const std::string&, char, size_t, "
        "std::vector<LevelView>&) const");

    static_assert(book_detail::has_get_all_tops<B>::value,
        "book is missing: void get_all_tops(std::vector<NamedTop>&) const");

    static_assert(book_detail::has_get_spread<B>::value,
        "book is missing: Spread get_spread(const std::string&) const");

    static_assert(book_detail::has_match<B>::value,
        "book is missing: void match(const std::string&, char, uint32_t, "
        "std::vector<Fill>&) const");

    static_assert(book_detail::has_get_events<B>::value,
        "book is missing: void get_events(const EventQuery&, std::vector<Event>&) const");

    static_assert(book_detail::has_get_top_of_book_at<B>::value,
        "book is missing: TopOfBook get_top_of_book_at(const std::string&, int64_t) const");

    static_assert(book_detail::has_get_spread_at<B>::value,
        "book is missing: Spread get_spread_at(const std::string&, int64_t) const");

    static_assert(book_detail::has_get_top_of_book_series<B>::value,
        "book is missing: void get_top_of_book_series(const std::string&, int64_t, int64_t, "
        "std::vector<TopSample>&) const");

    static_assert(book_detail::has_get_spread_series<B>::value,
        "book is missing: void get_spread_series(const std::string&, int64_t, int64_t, "
        "std::vector<SpreadSample>&) const");

    static_assert(book_detail::has_get_checksum<B>::value,
        "book is missing: int64_t get_checksum() const");

    static_assert(std::is_default_constructible<B>::value,
        "book must be default constructible: the benchmark builds a fresh one per rep");
};

#endif
