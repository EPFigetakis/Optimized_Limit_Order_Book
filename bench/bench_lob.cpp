// Benchmark: lob::OrderBook vs a naive std::map + std::list + std::unordered_map book.
//
//   bench_lob [--out DIR] [--events N] [--depth-events N] [--reps N] [--quick]
//
// Phases (all deterministic: fixed seed, identical event stream for both books):
//   1. warm-up pass (discarded)
//   2. throughput: `reps` timed passes over the whole stream, fresh book each
//   3. latency: one pass with per-event rdtsc timing (separate from throughput)
//   4. depth sweep: throughput vs resting-order count
// Raw results go to CSV files in DIR; see README for how they are plotted.
#include "baseline_book.hpp"
#include "lob.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define LOB_HAVE_TSC 1
#else
#define LOB_HAVE_TSC 0
#endif
#ifdef _WIN32
#include <windows.h>
#endif

using namespace lob;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static constexpr size_t kPriceRange = 10000;
static constexpr size_t kGenCapacity  = 1u << 21;  // generator oracle: large enough never to exhaust
static constexpr size_t kMainCapacity = 1u << 18;  // book under test for the main stream (262,144 orders)
static constexpr uint64_t kSeed       = 0x5EEDC0DE2024ULL;

using Gen  = OrderBook<kGenCapacity, kPriceRange>;
using Ours = OrderBook<kMainCapacity, kPriceRange>;

enum class Ev : uint8_t { Add, Cancel, Marketable };

struct Event {
    OrderId  id;
    uint32_t price;
    uint32_t qty;
    Ev       type;
    Side     side;
};

// ---------------------------------------------------------------------------
// Synthetic event stream
// ---------------------------------------------------------------------------
struct StreamParams {
    size_t   events        = 4000000;
    double   p_add         = 0.60;     // nominal mix (ignored when `regulate`)
    double   p_cancel      = 0.35;     // remainder is marketable
    double   p_marketable  = 0.05;
    double   mean_offset   = 30.0;     // ticks away from mid for passive orders (exponential)
    double   mean_qty      = 40.0;     // passive order size (exponential)
    double   mean_mkt_qty  = 250.0;    // marketable order size (exponential)
    bool     regulate      = false;    // keep resting depth near `target_depth`
    size_t   target_depth  = 0;
    OrderId  first_id      = 1;
};

struct Rng {  // xoshiro-free tiny PRNG wrapper on mt19937_64 for reproducibility
    std::mt19937_64 g;
    explicit Rng(uint64_t s) : g(s) {}
    double u01() { return (g() >> 11) * (1.0 / 9007199254740992.0); }
    double expo(double mean) { return -mean * std::log(1.0 - u01()); }
    uint64_t below(uint64_t n) { return g() % n; }
};

struct StreamStats {
    size_t max_live = 0, final_live = 0, adds = 0, cancels = 0, marketable = 0;
    size_t cancel_hits = 0, orders_that_matched = 0;
    uint64_t filled = 0;
};

// The stream is generated while replaying it through a reference lob::OrderBook,
// so that cancels target orders that are (almost always) really resting.
static std::vector<Event> generate_stream(const StreamParams& P, Rng& rng, Gen& oracle,
                                          std::vector<OrderId>& live, StreamStats& st,
                                          int64_t& mid) {
    std::vector<Event> out;
    out.reserve(P.events);
    OrderId next_id = P.first_id;
    const int64_t lo = 200, hi = static_cast<int64_t>(kPriceRange) - 200;

    auto clamp_price = [&](int64_t p) {
        return static_cast<uint32_t>(std::max<int64_t>(1, std::min<int64_t>(p, kPriceRange - 1)));
    };
    auto qty_of = [&](double mean) {
        return static_cast<uint32_t>(std::min(5000.0, 1.0 + std::floor(rng.expo(mean))));
    };

    for (size_t i = 0; i < P.events; ++i) {
        if (rng.below(50) == 0) mid = std::max(lo, std::min(hi, mid + (rng.below(2) ? 1 : -1)));

        Ev type;
        double r = rng.u01();
        if (P.regulate) {
            if (r < P.p_marketable) type = Ev::Marketable;
            else type = live.size() < P.target_depth ? Ev::Add : Ev::Cancel;
        } else {
            type = r < P.p_add ? Ev::Add : (r < P.p_add + P.p_cancel ? Ev::Cancel : Ev::Marketable);
        }
        if (type == Ev::Cancel && live.empty()) type = Ev::Add;

        Event e{};
        e.type = type;
        e.side = rng.below(2) ? Side::Buy : Side::Sell;

        if (type == Ev::Cancel) {
            OrderId id = 0;
            for (int attempt = 0; attempt < 4 && !live.empty(); ++attempt) {
                size_t k = rng.below(live.size());
                id = live[k];
                live[k] = live.back();
                live.pop_back();
                if (oracle.order_quantity(id) != 0) break;   // still resting
            }
            e.id = id;
            ++st.cancels;
            st.cancel_hits += oracle.cancel_order(id);
        } else {
            e.id = next_id++;
            if (type == Ev::Add) {
                int64_t off = 1 + static_cast<int64_t>(std::floor(rng.expo(P.mean_offset)));
                e.price = clamp_price(e.side == Side::Buy ? mid - off : mid + off);
                e.qty = qty_of(P.mean_qty);
                ++st.adds;
            } else {
                int64_t k = static_cast<int64_t>(rng.below(3));
                e.price = clamp_price(e.side == Side::Buy ? mid + 1 + k : mid - 1 - k);
                e.qty = qty_of(P.mean_mkt_qty);
                ++st.marketable;
            }
            AddResult res = oracle.add_order(e.id, e.price, e.qty, e.side);
            if (res.status == AddStatus::PoolExhausted) {
                std::fprintf(stderr, "generator: pool exhausted; raise kCapacity\n");
                std::exit(2);
            }
            st.filled += res.filled;
            st.orders_that_matched += res.filled > 0;
            if (res.rested) live.push_back(e.id);
        }
        st.max_live = std::max(st.max_live, oracle.order_count());
        out.push_back(e);
    }
    st.final_live = oracle.order_count();
    return out;
}

// ---------------------------------------------------------------------------
// Event application (identical for both books)
// ---------------------------------------------------------------------------
struct Totals {
    uint64_t filled = 0, cancel_hits = 0, rested = 0;
    bool operator==(const Totals& o) const {
        return filled == o.filled && cancel_hits == o.cancel_hits && rested == o.rested;
    }
};

template <class Book>
static inline void apply(Book& b, const Event& e, Totals& t) {
    if (e.type == Ev::Cancel) {
        t.cancel_hits += b.cancel_order(e.id);
    } else {
        AddResult r = b.add_order(e.id, e.price, e.qty, e.side);
        t.filled += r.filled;
        t.rested += r.rested;
    }
}

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------
static double g_ticks_per_ns = 1.0;   // TSC ticks per nanosecond (1.0 => clock is already ns)

static inline void compiler_barrier() {
#if defined(__GNUC__)
    asm volatile("" ::: "memory");
#endif
}

#if LOB_HAVE_TSC
static inline uint64_t tsc_begin() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
static inline uint64_t tsc_end() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}
#else
static inline uint64_t tsc_begin() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
static inline uint64_t tsc_end() { return tsc_begin(); }
#endif

static void calibrate_tsc() {
#if LOB_HAVE_TSC
    auto t0 = Clock::now();
    uint64_t c0 = tsc_begin();
    while (Clock::now() - t0 < std::chrono::milliseconds(500)) {}
    uint64_t c1 = tsc_end();
    auto t1 = Clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    g_ticks_per_ns = static_cast<double>(c1 - c0) / ns;
#endif
}

static void pin_and_prioritise() {
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);   // a fixed core, away from core 0 interrupts
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

static std::string cpu_brand() {
#if defined(__x86_64__) && defined(__GNUC__)
    char brand[49] = {0};
    unsigned a, b, c, d;
    for (unsigned i = 0; i < 3; ++i) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002 + i), "c"(0));
        std::memcpy(brand + 16 * i + 0, &a, 4);
        std::memcpy(brand + 16 * i + 4, &b, 4);
        std::memcpy(brand + 16 * i + 8, &c, 4);
        std::memcpy(brand + 16 * i + 12, &d, 4);
    }
    std::string s(brand);
    size_t p = s.find_first_not_of(' ');
    size_t e = s.find_last_not_of(' ');
    return p == std::string::npos ? "unknown" : s.substr(p, e - p + 1);
#else
    return "unknown";
#endif
}

static double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// Throughput
// ---------------------------------------------------------------------------
// `make` builds a fresh book; `prefill` (may be empty) replays preload events
// untimed before the measured stream.
template <class Book, class Make>
static double timed_pass(Make make, const std::vector<Event>& prefill,
                         const std::vector<Event>& stream, Totals& totals) {
    auto book = make();
    Totals scratch;
    for (const Event& e : prefill) apply(*book, e, scratch);
    Totals t;
    compiler_barrier();
    auto t0 = Clock::now();
    for (const Event& e : stream) apply(*book, e, t);
    compiler_barrier();
    double s = seconds_since(t0);
    totals = t;
    return s;
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------
enum Op { OpAdd = 0, OpMatch = 1, OpCancel = 2, kOps = 3 };
static const char* kOpName[kOps] = {"add", "match", "cancel"};

struct LatencyResult {
    std::vector<uint32_t> ticks[kOps];
};

template <class Book, class Make>
static LatencyResult latency_pass(Make make, const std::vector<Event>& stream) {
    auto book = make();
    LatencyResult R;
    for (auto& v : R.ticks) v.reserve(stream.size());
    Totals t;
    for (const Event& e : stream) {
        uint64_t a, b;
        int op;
        if (e.type == Ev::Cancel) {
            a = tsc_begin();
            compiler_barrier();
            bool hit = book->cancel_order(e.id);
            compiler_barrier();
            b = tsc_end();
            t.cancel_hits += hit;
            op = OpCancel;
        } else {
            a = tsc_begin();
            compiler_barrier();
            AddResult r = book->add_order(e.id, e.price, e.qty, e.side);
            compiler_barrier();
            b = tsc_end();
            t.filled += r.filled;
            op = r.filled > 0 ? OpMatch : OpAdd;
        }
        R.ticks[op].push_back(static_cast<uint32_t>(std::min<uint64_t>(b - a, UINT32_MAX)));
    }
    return R;
}

static double pct(const std::vector<uint32_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    size_t i = static_cast<size_t>(q * (sorted.size() - 1));
    return sorted[i] / g_ticks_per_ns;
}

// ---------------------------------------------------------------------------
// Depth sweep: the book under test is sized for its depth (capacity ~ 2-4x depth),
// as a user would configure it, so cache footprint scales with depth.
// ---------------------------------------------------------------------------
struct DepthPoint { size_t depth; double ours_meps, base_meps; size_t final_live; };

template <size_t Cap>
static bool depth_point(size_t D, size_t depth_events, int reps, DepthPoint& out) {
    using Book = OrderBook<Cap, kPriceRange>;
    auto make_o = [] { return std::make_unique<Book>(); };
    auto make_b = [] { return std::make_unique<BaselineBook>(kPriceRange); };

    // Preload D resting orders across a band whose width grows with D, then
    // generate a steady-state stream that holds the depth near D.
    auto oracle = std::make_unique<Gen>();
    Rng rng(kSeed ^ (D * 0x9E3779B97F4A7C15ULL));
    const double width = std::max(50.0, std::min(3500.0, D / 8.0));
    std::vector<Event> prefill;
    prefill.reserve(D);
    OrderId id = 1;
    for (size_t i = 0; i < D; ++i) {
        Event e{};
        e.type = Ev::Add;
        e.side = (i & 1) ? Side::Buy : Side::Sell;
        int64_t off = 1 + static_cast<int64_t>(rng.below(static_cast<uint64_t>(width)));
        e.price = static_cast<uint32_t>(e.side == Side::Buy ? 5000 - off : 5000 + off);
        e.qty = 1 + static_cast<uint32_t>(rng.expo(40.0));
        e.id = id++;
        oracle->add_order(e.id, e.price, e.qty, e.side);
        prefill.push_back(e);
    }
    std::vector<OrderId> live;
    for (const Event& e : prefill) live.push_back(e.id);

    StreamParams P;
    P.events = depth_events;
    P.regulate = true;
    P.target_depth = D;
    P.mean_offset = width / 4.0;
    P.mean_mkt_qty = 150.0;
    P.first_id = id;
    StreamStats ds;
    int64_t mid = 5000;
    std::vector<Event> s = generate_stream(P, rng, *oracle, live, ds, mid);
    if (ds.max_live * 10 > Cap * 9) { std::fprintf(stderr, "depth %zu needs more capacity\n", D); return false; }

    Totals to, tb;
    timed_pass<Book>(make_o, prefill, s, to);                   // warm-up
    timed_pass<BaselineBook>(make_b, prefill, s, tb);
    if (!(to == tb)) { std::fprintf(stderr, "MISMATCH in depth sweep D=%zu\n", D); return false; }
    std::vector<double> a, b;
    for (int r = 0; r < reps; ++r) {
        a.push_back(timed_pass<Book>(make_o, prefill, s, to));
        b.push_back(timed_pass<BaselineBook>(make_b, prefill, s, tb));
    }
    auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    out = {D, s.size() / med(a) / 1e6, s.size() / med(b) / 1e6, ds.final_live};
    return true;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string out_dir = "docs/results";
    StreamParams main_params;
    size_t depth_events = 1000000;
    int reps = 7;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (std::fprintf(stderr, "missing value for %s\n", a.c_str()), std::exit(1), nullptr); };
        if (a == "--out") out_dir = next();
        else if (a == "--events") main_params.events = std::strtoull(next(), nullptr, 10);
        else if (a == "--depth-events") depth_events = std::strtoull(next(), nullptr, 10);
        else if (a == "--reps") reps = std::atoi(next());
        else if (a == "--quick") { main_params.events = 300000; depth_events = 100000; reps = 2; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }

    pin_and_prioritise();
    calibrate_tsc();

    // TSC read overhead (back-to-back begin/end), reported so readers can discount it.
    std::vector<uint32_t> ov(100000);
    for (auto& x : ov) { uint64_t a = tsc_begin(); compiler_barrier(); uint64_t b = tsc_end(); x = static_cast<uint32_t>(b - a); }
    std::sort(ov.begin(), ov.end());
    double timer_overhead_ns = ov[ov.size() / 2] / g_ticks_per_ns;

    double timer_granularity_ns = 0;
    {
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < 200000; ++i) {
            uint64_t a = tsc_begin(), b = tsc_begin();
            if (b > a && b - a < best) best = b - a;
        }
        std::vector<uint64_t> deltas;
        uint64_t prev = tsc_begin();
        while (deltas.size() < 2000) {
            uint64_t cur = tsc_begin();
            if (cur != prev) { deltas.push_back(cur - prev); prev = cur; }
        }
        std::sort(deltas.begin(), deltas.end());
        timer_granularity_ns = deltas[deltas.size() / 2] / g_ticks_per_ns;   // typical step between distinct readings
        (void)best;
    }
    std::string cpu = cpu_brand();
    std::printf("CPU: %s\nTSC: %.4f GHz   timer overhead (median): %.1f ns, granularity ~%.1f ns\n", cpu.c_str(), g_ticks_per_ns, timer_overhead_ns, timer_granularity_ns);
#ifdef __VERSION__
    std::printf("Compiler: %s\n", __VERSION__);
#endif

    // ---------------- main stream -----------------------------------------
    std::printf("\nGenerating %zu-event stream (seed %#llx)...\n", main_params.events, (unsigned long long)kSeed);
    StreamStats st;
    std::vector<Event> stream;
    {
        auto oracle = std::make_unique<Gen>();
        Rng rng(kSeed);
        std::vector<OrderId> live;
        int64_t mid = kPriceRange / 2;
        stream = generate_stream(main_params, rng, *oracle, live, st, mid);
    }
    if (st.max_live * 10 > kMainCapacity * 9) {
        std::fprintf(stderr, "stream needs %zu resting orders; raise kMainCapacity\n", st.max_live);
        return 2;
    }
    std::printf("  adds=%zu (%.1f%%) cancels=%zu (%.1f%%, %.1f%% hit) marketable=%zu (%.1f%%)\n"
                "  events that matched: %zu   max resting orders: %zu   final: %zu\n",
                st.adds, 100.0 * st.adds / stream.size(), st.cancels, 100.0 * st.cancels / stream.size(),
                100.0 * st.cancel_hits / std::max<size_t>(1, st.cancels), st.marketable,
                100.0 * st.marketable / stream.size(), st.orders_that_matched, st.max_live, st.final_live);

    auto make_ours = [] { return std::make_unique<Ours>(); };
    auto make_base = [] { return std::make_unique<BaselineBook>(kPriceRange); };
    const std::vector<Event> no_prefill;

    // ---------------- warm-up ---------------------------------------------
    std::printf("\nWarm-up...\n");
    Totals warm_o, warm_b;
    timed_pass<Ours>(make_ours, no_prefill, stream, warm_o);
    timed_pass<BaselineBook>(make_base, no_prefill, stream, warm_b);
    if (!(warm_o == warm_b)) {
        std::fprintf(stderr, "MISMATCH: books disagree on filled/cancel/rested totals\n");
        return 3;
    }
    std::printf("  both books agree: filled=%llu cancel_hits=%llu rested=%llu\n",
                (unsigned long long)warm_o.filled, (unsigned long long)warm_o.cancel_hits,
                (unsigned long long)warm_o.rested);

    // ---------------- throughput ------------------------------------------
    std::printf("\nThroughput (%d reps, fresh book each)...\n", reps);
    std::vector<double> t_ours, t_base;
    for (int r = 0; r < reps; ++r) {   // interleave to spread any drift evenly
        Totals t;
        t_ours.push_back(timed_pass<Ours>(make_ours, no_prefill, stream, t));
        t_base.push_back(timed_pass<BaselineBook>(make_base, no_prefill, stream, t));
    }
    auto meps = [&](double s) { return stream.size() / s / 1e6; };
    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    double ours_med = meps(median(t_ours)), base_med = meps(median(t_base));
    std::printf("  lob::OrderBook   median %.2f M events/s   (min %.2f, max %.2f)\n", ours_med,
                meps(*std::max_element(t_ours.begin(), t_ours.end())), meps(*std::min_element(t_ours.begin(), t_ours.end())));
    std::printf("  std baseline     median %.2f M events/s   (min %.2f, max %.2f)\n", base_med,
                meps(*std::max_element(t_base.begin(), t_base.end())), meps(*std::min_element(t_base.begin(), t_base.end())));
    std::printf("  speedup: %.1fx\n", ours_med / base_med);

    // ---------------- latency ---------------------------------------------
    std::printf("\nLatency pass (per-event rdtsc)...\n");
    LatencyResult lat_o = latency_pass<Ours>(make_ours, stream);
    LatencyResult lat_b = latency_pass<BaselineBook>(make_base, stream);

    // ---------------- write CSVs ------------------------------------------
    auto open_csv = [&](const char* name) {
        std::string path = out_dir + "/" + name;
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot write %s (create the directory first)\n", path.c_str()); std::exit(4); }
        return f;
    };

    FILE* f = open_csv("throughput.csv");
    std::fprintf(f, "impl,rep,events,seconds,m_events_per_sec\n");
    for (int r = 0; r < reps; ++r) {
        std::fprintf(f, "lob::OrderBook,%d,%zu,%.6f,%.4f\n", r, stream.size(), t_ours[r], meps(t_ours[r]));
        std::fprintf(f, "std baseline,%d,%zu,%.6f,%.4f\n", r, stream.size(), t_base[r], meps(t_base[r]));
    }
    std::fclose(f);

    f = open_csv("latency_summary.csv");
    std::fprintf(f, "impl,op,count,min_ns,p50_ns,p90_ns,p99_ns,p99_9_ns,max_ns,mean_ns\n");
    std::printf("\n%-16s %-7s %9s %8s %8s %8s %9s\n", "impl", "op", "count", "p50 ns", "p99 ns", "p99.9 ns", "max ns");
    struct Named { const char* name; LatencyResult* r; } impls[2] = {{"lob::OrderBook", &lat_o}, {"std baseline", &lat_b}};
    FILE* fc = open_csv("latency_cdf.csv");
    std::fprintf(fc, "impl,op,quantile,ns\n");
    for (auto& im : impls) {
        for (int op = 0; op < kOps; ++op) {
            auto& v = im.r->ticks[op];
            std::sort(v.begin(), v.end());
            if (v.empty()) continue;
            double sum = 0;
            for (uint32_t x : v) sum += x;
            std::fprintf(f, "%s,%s,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f\n", im.name, kOpName[op], v.size(),
                         pct(v, 0), pct(v, 0.5), pct(v, 0.9), pct(v, 0.99), pct(v, 0.999), pct(v, 1.0),
                         sum / v.size() / g_ticks_per_ns);
            std::printf("%-16s %-7s %9zu %8.1f %8.1f %9.1f %9.1f\n", im.name, kOpName[op], v.size(),
                        pct(v, 0.5), pct(v, 0.99), pct(v, 0.999), pct(v, 1.0));
            // Quantile grid: dense in the body and geometrically denser toward the tail.
            std::vector<double> qs;
            for (int i = 0; i <= 500; ++i) qs.push_back(0.99 * i / 500.0);
            for (int i = 1; i <= 1500; ++i) qs.push_back(1.0 - 0.01 * std::pow(10.0, -4.0 * i / 1500.0));
            qs.push_back(1.0);
            for (double q : qs) std::fprintf(fc, "%s,%s,%.8f,%.1f\n", im.name, kOpName[op], q, pct(v, q));
        }
    }
    std::fclose(f);
    std::fclose(fc);

    // ---------------- depth sweep -----------------------------------------
    std::printf("\nDepth sweep (%zu events per point, depth-regulated mix)...\n", depth_events);
    std::vector<DepthPoint> depth_pts;
    const int depth_reps = std::max(3, reps / 2);
    for (size_t D : {size_t(1000), size_t(10000), size_t(100000), size_t(1000000)}) {
        DepthPoint pt{};
        bool ok = D <= 1000    ? depth_point<(1u << 12)>(D, depth_events, depth_reps, pt)
                : D <= 10000   ? depth_point<(1u << 15)>(D, depth_events, depth_reps, pt)
                : D <= 100000  ? depth_point<(1u << 18)>(D, depth_events, depth_reps, pt)
                               : depth_point<(1u << 21)>(D, depth_events, depth_reps, pt);
        if (!ok) return 3;
        depth_pts.push_back(pt);
        std::printf("  depth %8zu (resting at end: %zu): ours %.2f  baseline %.2f M events/s  (%.1fx)\n", D,
                    pt.final_live, pt.ours_meps, pt.base_meps, pt.ours_meps / pt.base_meps);
    }
    f = open_csv("depth_sweep.csv");
    std::fprintf(f, "depth,impl,events,m_events_per_sec\n");
    for (auto& p : depth_pts) {
        std::fprintf(f, "%zu,lob::OrderBook,%zu,%.4f\n", p.depth, depth_events, p.ours_meps);
        std::fprintf(f, "%zu,std baseline,%zu,%.4f\n", p.depth, depth_events, p.base_meps);
    }
    std::fclose(f);

    // ---------------- environment -----------------------------------------
    f = open_csv("environment.csv");
    std::fprintf(f, "key,value\n");
    std::fprintf(f, "cpu,\"%s\"\n", cpu.c_str());
#ifdef __VERSION__
    std::fprintf(f, "compiler,\"%s\"\n", __VERSION__);
#endif
#ifdef _WIN32
    std::fprintf(f, "os,Windows (thread pinned to core 2, HIGH_PRIORITY_CLASS)\n");
#endif
    std::fprintf(f, "flags,-O3 -DNDEBUG (no -march=native)\n");
    std::fprintf(f, "tsc_ghz,%.4f\n", g_ticks_per_ns);
    std::fprintf(f, "timer_overhead_ns,%.1f\n", timer_overhead_ns);
    std::fprintf(f, "timer_granularity_ns,%.1f\n", timer_granularity_ns);
    std::fprintf(f, "seed,%#llx\n", (unsigned long long)kSeed);
    std::fprintf(f, "events,%zu\n", stream.size());
    std::fprintf(f, "throughput_reps,%d\n", reps);
    std::fprintf(f, "price_range,%zu\n", kPriceRange);
    std::fprintf(f, "pool_capacity_main,%zu\n", kMainCapacity);
    std::fprintf(f, "stream_adds_pct,%.1f\n", 100.0 * st.adds / stream.size());
    std::fprintf(f, "stream_cancels_pct,%.1f\n", 100.0 * st.cancels / stream.size());
    std::fprintf(f, "stream_marketable_pct,%.1f\n", 100.0 * st.marketable / stream.size());
    std::fprintf(f, "stream_max_resting,%zu\n", st.max_live);
    std::fclose(f);

    std::printf("\nResults written to %s/\n", out_dir.c_str());
    return 0;
}
