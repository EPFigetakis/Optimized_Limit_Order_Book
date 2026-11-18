// Unit, regression and differential tests for lob.hpp.
// Uses its own CHECK macro so that checks stay active in Release builds.
#include "baseline_book.hpp"
#include "lob.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <vector>

using namespace lob;

static int g_checks = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                            \
        }                                                                            \
    } while (0)

#define RUN(fn)                                  \
    do {                                         \
        std::printf("[RUN ] %s\n", #fn);         \
        fn();                                    \
        std::printf("[ OK ] %s\n", #fn);         \
    } while (0)

// ===========================================================================
// FlatHashMap
// ===========================================================================

// Collect `n` distinct keys that share a home bucket.
template <class Map>
static std::vector<OrderId> colliding_keys(size_t n, size_t want_bucket) {
    std::vector<OrderId> keys;
    for (OrderId k = 1; keys.size() < n; ++k)
        if (Map::bucket(k) == want_bucket) keys.push_back(k);
    return keys;
}

// REGRESSION: erasing the head of a probe chain used to empty its slot, making
// every later member of the chain unreachable.
static void test_hash_erase_keeps_probe_chain() {
    using Map = FlatHashMap<16>;
    Order a, b, c;
    auto keys = colliding_keys<Map>(3, 3);
    Map m;
    m.insert(keys[0], &a);
    m.insert(keys[1], &b);
    m.insert(keys[2], &c);
    CHECK(m.erase(keys[0]));
    CHECK(m.find(keys[0]) == nullptr);
    CHECK(m.find(keys[1]) == &b);   // failed before the fix
    CHECK(m.find(keys[2]) == &c);
    CHECK(m.erase(keys[1]));
    CHECK(m.find(keys[2]) == &c);
    CHECK(m.erase(keys[2]));
    CHECK(m.size() == 0);
    CHECK(!m.erase(keys[2]));
}

// Chain that wraps around the end of the table.
static void test_hash_wraparound_chain() {
    using Map = FlatHashMap<16>;
    Order o[4];
    auto keys = colliding_keys<Map>(4, Map::kSlots - 1);
    Map m;
    for (int i = 0; i < 4; ++i) m.insert(keys[i], &o[i]);
    CHECK(m.erase(keys[0]));
    for (int i = 1; i < 4; ++i) CHECK(m.find(keys[i]) == &o[i]);
    CHECK(m.erase(keys[2]));
    CHECK(m.find(keys[1]) == &o[1]);
    CHECK(m.find(keys[3]) == &o[3]);
}

// An entry that is NOT part of the chain must not be shifted out of reach.
static void test_hash_shift_does_not_break_other_buckets() {
    using Map = FlatHashMap<16>;
    Order a, b, c;
    auto k5 = colliding_keys<Map>(2, 5);
    auto k6 = colliding_keys<Map>(1, 6);
    Map m;
    m.insert(k5[0], &a);   // slot 5
    m.insert(k5[1], &b);   // slot 6 (displaced)
    m.insert(k6[0], &c);   // slot 7 (displaced)
    CHECK(m.erase(k5[0]));
    CHECK(m.find(k5[1]) == &b);
    CHECK(m.find(k6[0]) == &c);
    CHECK(m.erase(k5[1]));
    CHECK(m.find(k6[0]) == &c);
}

// Randomized model check against std::unordered_map at high load.
static void test_hash_randomized_vs_unordered_map() {
    using Map = FlatHashMap<64>;
    std::vector<Order> storage(64);
    std::mt19937_64 rng(1234);
    Map m;
    std::unordered_map<OrderId, Order*> ref;
    for (int step = 0; step < 300000; ++step) {
        OrderId key = rng() % 200;                 // small key space => collisions
        int op = rng() % 3;
        if (op == 0 && ref.size() < 48 /* load <= 0.75 */) {
            Order* v = &storage[rng() % storage.size()];
            m.insert(key, v);
            ref[key] = v;
        } else if (op == 1) {
            CHECK(m.erase(key) == (ref.erase(key) == 1));
        } else {
            auto it = ref.find(key);
            CHECK(m.find(key) == (it == ref.end() ? nullptr : it->second));
        }
        CHECK(m.size() == ref.size());
    }
    for (OrderId k = 0; k < 200; ++k) {
        auto it = ref.find(k);
        CHECK(m.find(k) == (it == ref.end() ? nullptr : it->second));
    }
}

// ===========================================================================
// OrderBook
// ===========================================================================

static void test_original_smoke() {
    OrderBook<1000, 10000> book;
    book.add_order(1, 1010, 100, Side::Sell);
    book.add_order(2, 1015, 200, Side::Sell);
    CHECK(book.get_best_ask() == 1010);

    book.add_order(3, 1010, 60, Side::Buy);
    CHECK(book.get_best_ask() == 1010);
    CHECK(book.order_quantity(1) == 40);

    book.add_order(4, 1012, 50, Side::Buy);
    CHECK(book.get_best_ask() == 1015);
    CHECK(book.get_best_bid() == 1012);
    CHECK(book.order_quantity(4) == 10);
    CHECK(book.validate());
}

// REGRESSION: cancel_order on a live order failed after a colliding id was erased.
static void test_book_cancel_after_colliding_erase() {
    constexpr size_t N = 4;                       // hash table has 8 slots
    using Map = FlatHashMap<N * 2>;
    auto ids = colliding_keys<Map>(3, 2);
    OrderBook<N, 100> book;
    CHECK(book.add_order(ids[0], 10, 5, Side::Buy).rested == 5);
    CHECK(book.add_order(ids[1], 11, 5, Side::Buy).rested == 5);
    CHECK(book.add_order(ids[2], 12, 5, Side::Buy).rested == 5);
    CHECK(book.cancel_order(ids[0]));
    CHECK(book.cancel_order(ids[1]));             // returned false before the fix
    CHECK(book.cancel_order(ids[2]));
    CHECK(!book.has_bid());
    CHECK(book.validate());
}

// Same, but the first colliding order is removed by a fill instead of a cancel.
static void test_book_fill_then_cancel_colliding() {
    constexpr size_t N = 4;
    using Map = FlatHashMap<N * 2>;
    auto ids = colliding_keys<Map>(2, 1);
    OrderBook<N, 100> book;
    book.add_order(ids[0], 50, 5, Side::Sell);
    book.add_order(ids[1], 51, 5, Side::Sell);
    CHECK(book.add_order(900, 50, 5, Side::Buy).filled == 5);   // consumes ids[0]
    CHECK(book.order_quantity(ids[1]) == 5);
    CHECK(book.cancel_order(ids[1]));
    CHECK(book.validate());
}

// REGRESSION: prices outside 1..PriceRange-1 used to index past bids_/asks_.
static void test_book_rejects_out_of_range_prices() {
    OrderBook<16, 100> book;
    const Price bad[] = {0, 100, 101, 5000, UINT64_MAX};
    for (Price p : bad)
        for (Side s : {Side::Buy, Side::Sell}) {
            AddResult r = book.add_order(1, p, 10, s);
            CHECK(r.status == AddStatus::RejectedPrice);
            CHECK(r.filled == 0 && r.rested == 0);
        }
    CHECK(book.order_count() == 0);
    CHECK(!book.has_bid() && !book.has_ask());
    // Boundary prices are accepted.
    CHECK(book.add_order(1, 1, 10, Side::Buy).status == AddStatus::Ok);
    CHECK(book.add_order(2, 99, 10, Side::Sell).status == AddStatus::Ok);
    CHECK(book.get_best_bid() == 1 && book.get_best_ask() == 99);
    CHECK(book.validate());
    // A marketable order with a bad price must not touch the book.
    CHECK(book.add_order(3, 1000, 10, Side::Buy).status == AddStatus::RejectedPrice);
    CHECK(book.order_quantity(2) == 10);
}

static void test_book_rejects_zero_qty_and_duplicates() {
    OrderBook<16, 100> book;
    CHECK(book.add_order(1, 50, 0, Side::Buy).status == AddStatus::RejectedQuantity);
    CHECK(book.add_order(1, 50, 10, Side::Buy).status == AddStatus::Ok);
    CHECK(book.add_order(1, 60, 99, Side::Sell).status == AddStatus::RejectedDuplicate);
    CHECK(book.order_quantity(1) == 10);
    CHECK(book.order_count() == 1);
    // Once an id has been fully filled it may be reused.
    CHECK(book.add_order(2, 50, 10, Side::Sell).filled == 10);
    CHECK(book.order_quantity(1) == 0);
    CHECK(book.add_order(1, 40, 3, Side::Buy).status == AddStatus::Ok);
    CHECK(book.validate());
}

// REGRESSION: after the last ask was consumed best_ask_ became PriceRange (not
// the "none" sentinel) and the next buy order read asks_[PriceRange].
static void test_book_empty_and_drained() {
    OrderBook<16, 100> book;
    CHECK(!book.has_bid() && !book.has_ask());
    CHECK(book.get_best_bid() == 0 && book.get_best_ask() == 0);
    CHECK(!book.cancel_order(42));

    book.add_order(1, 99, 10, Side::Sell);           // top-of-range ask
    CHECK(book.get_best_ask() == 99);
    CHECK(book.add_order(2, 99, 10, Side::Buy).filled == 10);
    CHECK(!book.has_ask() && book.get_best_ask() == 0);
    CHECK(book.add_order(3, 99, 4, Side::Buy).rested == 4);   // must rest, not read OOB
    CHECK(book.get_best_bid() == 99);

    book.add_order(4, 1, 10, Side::Buy);             // bottom-of-range bid
    CHECK(book.cancel_order(3));
    CHECK(book.get_best_bid() == 1);
    CHECK(book.add_order(5, 1, 10, Side::Sell).filled == 10);
    CHECK(!book.has_bid() && book.get_best_bid() == 0);
    CHECK(book.add_order(6, 1, 4, Side::Sell).rested == 4);
    CHECK(book.get_best_ask() == 1);
    CHECK(book.validate());
}

static void test_book_pool_exhaustion() {
    OrderBook<4, 100> book;
    for (OrderId i = 1; i <= 4; ++i)
        CHECK(book.add_order(i, 10 + i, 5, Side::Buy).status == AddStatus::Ok);
    AddResult r = book.add_order(5, 30, 5, Side::Buy);
    CHECK(r.status == AddStatus::PoolExhausted && r.rested == 0);
    CHECK(book.order_quantity(5) == 0);
    CHECK(book.get_best_bid() == 14);
    CHECK(book.validate());

    // A marketable order still executes, and the free slots are reusable.
    r = book.add_order(6, 14, 5, Side::Sell);
    CHECK(r.status == AddStatus::Ok && r.filled == 5);
    CHECK(book.add_order(7, 30, 5, Side::Buy).status == AddStatus::Ok);

    // Pool full, but an incoming order that consumes a resting order frees its
    // slot before the remainder rests (a remainder implies a full consumption).
    OrderBook<2, 100> small;
    small.add_order(1, 50, 5, Side::Sell);
    small.add_order(2, 40, 5, Side::Buy);
    CHECK(small.order_count() == 2);
    r = small.add_order(3, 50, 8, Side::Buy);       // fills 5, rests 3 in the freed slot
    CHECK(r.status == AddStatus::Ok && r.filled == 5 && r.rested == 3);
    CHECK(small.add_order(4, 60, 1, Side::Sell).status == AddStatus::PoolExhausted);  // no cross, no slot
    CHECK(small.order_count() == 2);
    CHECK(small.validate());
}

static void test_book_sweep_multiple_levels() {
    OrderBook<64, 100> book;
    book.add_order(1, 50, 10, Side::Sell);
    book.add_order(2, 50, 10, Side::Sell);   // same level, later in FIFO
    book.add_order(3, 51, 10, Side::Sell);
    book.add_order(4, 53, 10, Side::Sell);   // gap at 52
    book.add_order(5, 55, 10, Side::Sell);

    AddResult r = book.add_order(100, 54, 35, Side::Buy);
    CHECK(r.status == AddStatus::Ok && r.filled == 35 && r.rested == 0);
    CHECK(book.order_quantity(1) == 0 && book.order_quantity(2) == 0);
    CHECK(book.order_quantity(3) == 0);
    CHECK(book.order_quantity(4) == 5);      // partially filled
    CHECK(book.order_quantity(5) == 10);
    CHECK(book.get_best_ask() == 53);
    CHECK(book.volume_at(Side::Sell, 53) == 5);
    CHECK(book.validate());

    // Sweep everything that crosses, remainder rests at the limit price.
    r = book.add_order(101, 54, 20, Side::Buy);
    CHECK(r.filled == 5 && r.rested == 15);
    CHECK(book.get_best_ask() == 55 && book.get_best_bid() == 54);
    CHECK(book.validate());

    // Sell-side sweep mirrors it.
    OrderBook<64, 100> b2;
    b2.add_order(1, 60, 10, Side::Buy);
    b2.add_order(2, 60, 10, Side::Buy);
    b2.add_order(3, 58, 10, Side::Buy);
    b2.add_order(4, 55, 10, Side::Buy);
    r = b2.add_order(5, 57, 100, Side::Sell);      // takes 60, 60, 58; 55 does not cross
    CHECK(r.filled == 30 && r.rested == 70);
    CHECK(b2.get_best_bid() == 55 && b2.get_best_ask() == 57);
    CHECK(b2.order_quantity(4) == 10 && b2.order_quantity(5) == 70);
    CHECK(b2.validate());
}

static void test_book_fifo_priority() {
    OrderBook<16, 100> book;
    book.add_order(1, 50, 10, Side::Buy);
    book.add_order(2, 50, 10, Side::Buy);
    book.add_order(3, 50, 10, Side::Buy);
    book.add_order(9, 50, 15, Side::Sell);
    CHECK(book.order_quantity(1) == 0);
    CHECK(book.order_quantity(2) == 5);
    CHECK(book.order_quantity(3) == 10);
    CHECK(book.volume_at(Side::Buy, 50) == 15);
    CHECK(book.validate());
}

// Cancel after a partial fill must remove exactly the *remaining* quantity.
static void test_book_cancel_after_partial_fill() {
    OrderBook<16, 100> book;
    book.add_order(1, 50, 10, Side::Sell);
    book.add_order(2, 50, 10, Side::Sell);
    book.add_order(3, 50, 4, Side::Buy);               // order 1 -> 6 left
    CHECK(book.order_quantity(1) == 6);
    CHECK(book.volume_at(Side::Sell, 50) == 16);
    CHECK(book.cancel_order(1));
    CHECK(book.volume_at(Side::Sell, 50) == 10);
    CHECK(!book.cancel_order(1));                       // already gone
    CHECK(book.get_best_ask() == 50);
    CHECK(book.cancel_order(2));
    CHECK(!book.has_ask());
    CHECK(book.volume_at(Side::Sell, 50) == 0);
    CHECK(!book.cancel_order(3));                       // fully filled, never rested
    CHECK(book.validate());
}

static void test_book_cancel_best_moves_top_of_book() {
    OrderBook<16, 100> book;
    book.add_order(1, 40, 1, Side::Buy);
    book.add_order(2, 45, 1, Side::Buy);
    book.add_order(3, 60, 1, Side::Sell);
    book.add_order(4, 70, 1, Side::Sell);
    CHECK(book.cancel_order(2));
    CHECK(book.get_best_bid() == 40);
    CHECK(book.cancel_order(3));
    CHECK(book.get_best_ask() == 70);
    CHECK(book.cancel_order(1));
    CHECK(book.cancel_order(4));
    CHECK(!book.has_bid() && !book.has_ask());
    CHECK(book.validate());
}

// ===========================================================================
// Differential test: OrderBook vs BaselineBook
// ===========================================================================

template <size_t MaxOrders, size_t PriceRange>
static void run_differential(uint64_t seed, size_t events, size_t full_check_every,
                             int cancel_pct, int bad_price_pct, const char* name) {
    std::mt19937_64 rng(seed);
    OrderBook<MaxOrders, PriceRange> book;
    BaselineBook ref(PriceRange, MaxOrders);

    std::vector<OrderId> issued;            // every id handed out (mostly dead)
    std::vector<OrderId> live;              // ids that rested at some point (may have been filled since)
    std::vector<OrderId> touched;
    OrderId next_id = 1;
    int64_t mid = PriceRange / 2;

    size_t n_add = 0, n_cancel = 0, n_cross = 0, n_reject = 0, n_exhaust = 0, n_cancel_hit = 0;

    auto full_compare = [&]() {
        CHECK(book.validate());
        CHECK(book.order_count() == ref.order_count());
        ref.for_each_order([&](OrderId id, Price price, Side side, Quantity q) {
            CHECK(book.order_quantity(id) == q);
            (void)price; (void)side;
        });
        for (Price p = 1; p < PriceRange; ++p) {
            CHECK(book.volume_at(Side::Buy, p)  == ref.volume_at(Side::Buy, p));
            CHECK(book.volume_at(Side::Sell, p) == ref.volume_at(Side::Sell, p));
        }
    };

    for (size_t ev = 0; ev < events; ++ev) {
        // Slow random walk of the mid so the book repeatedly crosses and recovers.
        mid += static_cast<int64_t>(rng() % 3) - 1;
        mid = std::max<int64_t>(5, std::min<int64_t>(mid, static_cast<int64_t>(PriceRange) - 6));

        int roll = static_cast<int>(rng() % 100);
        if (roll < cancel_pct && !issued.empty()) {
            ++n_cancel;
            OrderId id;
            if (rng() % 20 == 0)       id = next_id + 12345;                      // never-seen id
            else if (rng() % 10 == 0 || live.empty()) id = issued[rng() % issued.size()];  // likely dead
            else {                                                                // likely resting
                size_t k = rng() % live.size();
                id = live[k];
                live[k] = live.back();
                live.pop_back();
            }
            bool a = book.cancel_order(id);
            bool b = ref.cancel_order(id);
            CHECK(a == b);
            n_cancel_hit += a;
            CHECK(book.order_quantity(id) == 0);
        } else {
            ++n_add;
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            int64_t spread = static_cast<int64_t>(rng() % 8) - 3;  // negative => crosses
            int64_t p = side == Side::Buy ? mid - spread : mid + spread;
            if (rng() % 50 == 0) p = mid + static_cast<int64_t>(rng() % 40) - 20;  // aggressive
            Price price = static_cast<Price>(std::max<int64_t>(1, p));
            if (static_cast<int>(rng() % 100) < bad_price_pct) {
                const Price bad[] = {0, PriceRange, PriceRange + 1, PriceRange * 3, UINT64_MAX};
                price = bad[rng() % 5];
            }
            Quantity qty = (rng() % 40 == 0) ? 0 : static_cast<Quantity>(1 + rng() % ((rng() % 10 == 0) ? 120 : 12));
            OrderId id;
            if (!issued.empty() && rng() % 40 == 0) id = issued[rng() % issued.size()];  // maybe duplicate
            else { id = next_id++; issued.push_back(id); }

            touched.clear();
            AddResult a = book.add_order(id, price, qty, side);
            AddResult b = ref.add_order(id, price, qty, side, &touched);
            CHECK(a.status == b.status);
            CHECK(a.filled == b.filled);
            CHECK(a.rested == b.rested);
            n_cross += a.filled > 0;
            n_reject += (a.status != AddStatus::Ok && a.status != AddStatus::PoolExhausted);
            n_exhaust += a.status == AddStatus::PoolExhausted;
            if (a.rested) live.push_back(id);
            CHECK(book.order_quantity(id) == ref.order_quantity(id));
            for (OrderId t : touched) CHECK(book.order_quantity(t) == ref.order_quantity(t));
        }

        CHECK(book.get_best_bid() == ref.get_best_bid());
        CHECK(book.get_best_ask() == ref.get_best_ask());
        CHECK(book.order_count() == ref.order_count());

        if (live.size() > 4 * MaxOrders + 1000) live.erase(live.begin(), live.begin() + live.size() / 2);
        if (issued.size() > 4 * MaxOrders + 1000) {      // bound memory: forget old ids
            issued.erase(issued.begin(), issued.begin() + issued.size() / 2);
        }
        if ((ev + 1) % full_check_every == 0) full_compare();
    }
    full_compare();
    std::printf("      %-10s events=%zu adds=%zu cancels=%zu (hit %zu) crossing=%zu rejected=%zu pool_exhausted=%zu live=%zu\n",
                name, events, n_add, n_cancel, n_cancel_hit, n_cross, n_reject, n_exhaust, ref.order_count());
}

static void test_differential_dense_collisions() {
    // Tiny price range and tiny pool: constant crossing, hash collisions, exhaustion.
    run_differential<48, 40>(0xC0FFEE, 500000, 499, 40, 2, "tiny");
}

static void test_differential_large() {
    run_differential<20000, 400>(0xBADA55, 500000, 4999, 35, 1, "large");
}

int main() {
    RUN(test_hash_erase_keeps_probe_chain);
    RUN(test_hash_wraparound_chain);
    RUN(test_hash_shift_does_not_break_other_buckets);
    RUN(test_hash_randomized_vs_unordered_map);

    RUN(test_original_smoke);
    RUN(test_book_cancel_after_colliding_erase);
    RUN(test_book_fill_then_cancel_colliding);
    RUN(test_book_rejects_out_of_range_prices);
    RUN(test_book_rejects_zero_qty_and_duplicates);
    RUN(test_book_empty_and_drained);
    RUN(test_book_pool_exhaustion);
    RUN(test_book_sweep_multiple_levels);
    RUN(test_book_fifo_priority);
    RUN(test_book_cancel_after_partial_fill);
    RUN(test_book_cancel_best_moves_top_of_book);

    RUN(test_differential_dense_collisions);
    RUN(test_differential_large);

    std::printf("ALL TESTS PASSED (%d checks)\n", g_checks);
    return 0;
}
