CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I.
LDLIBS   := -lpcap

#Binaries land here in src/, the sources they come from live in subdirectories.
#A new book is one .o in BOOK_OBJS and one line in the REGISTRY in bench/bench.cpp.
#bookbench, not bench, because bench/ is the source directory
BINS := dpls_dump make_gap_fixture bookbench matchcheck

BOOK_OBJS := books/naive_book.o books/exchange_book.o

MATCH_OBJS := match/taker.o match/matcher.o match/matchcheck.o

#The storage substrate both axes of the study sit on: the on-disk record, the batch and
#its mergeable summary, the multi-file pager, and the time index over the files. Nothing
#in here knows what a book or a log database is.
STORE_OBJS := store/record.o store/batch.o store/pager.o store/file_index.o

OBJS := loader/dpls_dump.o loader/dpls_reader.o make_gap_fixture.o \
        bench/bench.o bench/events.o bench/drift.o bench/resources.o \
        bench/metrics.o bench/report.o \
        $(BOOK_OBJS) $(MATCH_OBJS) $(STORE_OBJS)

TEST_BINS := tests/test_books

all: $(BINS)

dpls_dump: loader/dpls_dump.o loader/dpls_reader.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

make_gap_fixture: make_gap_fixture.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

#--drift puts the exchange book against the mirror, so this now links match/ for the
#taker reconstruction. matchcheck.o is excluded: it carries its own main().
bookbench: bench/bench.o bench/events.o bench/drift.o bench/resources.o \
           bench/metrics.o bench/report.o \
           loader/dpls_reader.o match/taker.o match/matcher.o $(BOOK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

#Reuses bench/events.o for the load, so the check replays the identical decoded stream
#the benchmark does rather than a second reader written to be slightly different
matchcheck: $(MATCH_OBJS) bench/events.o bench/resources.o loader/dpls_reader.o \
            $(BOOK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

#Book rules on hand-built events, so a fresh clone can check the books without a capture
tests/test_books: tests/test_books.o $(BOOK_OBJS) loader/dpls_reader.o match/taker.o match/matcher.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

tests/%.o: tests/%.cpp books/book.h books/naive_book.h books/exchange_book.h loader/dpls_reader.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: $(TEST_BINS)
	./tests/test_books

loader/%.o: loader/%.cpp loader/dpls_reader.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

bench/%.o: bench/%.cpp bench/bench.h bench/drift.h bench/resources.h bench/metrics.h bench/report.h books/book.h \
           books/null_book.h books/naive_book.h books/exchange_book.h \
           loader/dpls_reader.h match/taker.h match/matcher.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

books/%.o: books/%.cpp books/%.h books/book.h books/naive_book.h loader/dpls_reader.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

match/%.o: match/%.cpp match/taker.h match/matcher.h books/book.h \
           books/naive_book.h loader/dpls_reader.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

store/%.o: store/%.cpp store/record.h store/batch.h store/pager.h store/file_index.h \
           loader/dpls_reader.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

logdb/%.o: logdb/%.cpp logdb/logdb.h store/record.h store/batch.h store/pager.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(BINS) $(OBJS) $(TEST_BINS) tests/*.o

.PHONY: all clean test
