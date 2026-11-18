// lob.hpp -- single-threaded limit order book with price-time priority.
//
//   * intrusive doubly-linked FIFO per price level
//   * fixed-capacity order pool (no heap allocation on the hot path)
//   * array-indexed price levels (price == index)
//   * open-addressing id -> order map with backward-shift deletion
//
// Valid prices are the integer ticks 1 .. PriceRange-1.  Price 0 is reserved
// as the "no bid" sentinel and PriceRange as the "no ask" sentinel.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

using OrderId  = uint64_t;
using Price    = uint64_t;
using Quantity = uint32_t;

enum class Side : uint8_t { Buy, Sell };

struct Order {
    OrderId  id{0};
    Price    price{0};
    Quantity quantity{0};
    Side     side{Side::Buy};

    Order* prev{nullptr};
    Order* next{nullptr};
};

// ---------------------------------------------------------------------------
// Intrusive FIFO queue of orders resting at one price.
// ---------------------------------------------------------------------------
struct PriceLevel {
    uint64_t total_volume{0};
    uint32_t order_count{0};
    Order*   head{nullptr};  // oldest order (highest time priority)
    Order*   tail{nullptr};  // newest order

    void push_back(Order* order) {
        order->prev = tail;
        order->next = nullptr;
        if (tail) tail->next = order;
        else      head = order;
        tail = order;
        total_volume += order->quantity;
        ++order_count;
    }

    void remove(Order* order) {
        if (order->prev) order->prev->next = order->next;
        else             head = order->next;

        if (order->next) order->next->prev = order->prev;
        else             tail = order->prev;

        total_volume -= order->quantity;
        --order_count;
        order->prev = nullptr;
        order->next = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Fixed-capacity pool.  All memory is allocated once, in the constructor.
// ---------------------------------------------------------------------------
template <size_t Capacity>
class OrderPool {
    static_assert(Capacity > 0, "pool capacity must be positive");

public:
    OrderPool() : pool_(Capacity) {
        for (size_t i = 0; i + 1 < Capacity; ++i) pool_[i].next = &pool_[i + 1];
        free_head_ = &pool_[0];
    }
    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    Order* allocate() {
        if (!free_head_) return nullptr;
        Order* order = free_head_;
        free_head_ = order->next;
        order->prev = nullptr;
        order->next = nullptr;
        ++in_use_;
        return order;
    }

    void deallocate(Order* order) {
        order->prev = nullptr;
        order->next = free_head_;
        free_head_ = order;
        --in_use_;
    }

    size_t in_use() const { return in_use_; }

private:
    std::vector<Order> pool_;
    Order*             free_head_{nullptr};
    size_t             in_use_{0};
};

// ---------------------------------------------------------------------------
// Open-addressing (linear probing) map OrderId -> Order*.
// A slot is empty iff its value is nullptr.  erase() uses backward-shift
// deletion, so no tombstones are needed and probe chains stay unbroken.
// MinSlots is rounded up to a power of two (index = hash & mask).
// The caller must keep the load factor well below 1 (OrderBook uses <= 0.5).
// ---------------------------------------------------------------------------
template <size_t MinSlots>
class FlatHashMap {
    static constexpr size_t round_up_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

public:
    static constexpr size_t kSlots = round_up_pow2(MinSlots < 2 ? 2 : MinSlots);
    static constexpr size_t kMask  = kSlots - 1;

    FlatHashMap() : table_(kSlots) {}

    static size_t bucket(OrderId id) {
        id ^= id >> 30;
        id *= 0xbf58476d1ce4e5b9ULL;
        id ^= id >> 27;
        id *= 0x94d049bb133111ebULL;
        id ^= id >> 31;
        return static_cast<size_t>(id) & kMask;
    }

    size_t size() const { return size_; }

    // Inserts or overwrites.  `value` must be non-null.
    void insert(OrderId key, Order* value) {
        assert(value != nullptr);
        assert(size_ < kSlots && "FlatHashMap is full");
        size_t idx = bucket(key);
        while (table_[idx].value) {
            if (table_[idx].key == key) { table_[idx].value = value; return; }
            idx = (idx + 1) & kMask;
        }
        table_[idx] = {key, value};
        ++size_;
    }

    Order* find(OrderId key) const {
        size_t idx = bucket(key);
        while (table_[idx].value) {
            if (table_[idx].key == key) return table_[idx].value;
            idx = (idx + 1) & kMask;
        }
        return nullptr;
    }

    bool erase(OrderId key) {
        size_t i = bucket(key);
        while (true) {
            if (!table_[i].value) return false;
            if (table_[i].key == key) break;
            i = (i + 1) & kMask;
        }
        // Backward-shift: pull later members of the probe chain into the hole
        // whenever doing so keeps them reachable from their home bucket.
        size_t j = i;
        while (true) {
            j = (j + 1) & kMask;
            if (!table_[j].value) break;
            size_t home = bucket(table_[j].key);
            // Entry at j may fill hole i iff i lies cyclically in [home, j].
            if (((j - home) & kMask) >= ((j - i) & kMask)) {
                table_[i] = table_[j];
                i = j;
            }
        }
        table_[i] = Entry{};
        --size_;
        return true;
    }

private:
    struct Entry {
        OrderId key{0};
        Order*  value{nullptr};
    };
    std::vector<Entry> table_;
    size_t             size_{0};
};

// ---------------------------------------------------------------------------
// Order book + matching engine.
// ---------------------------------------------------------------------------
enum class AddStatus : uint8_t {
    Ok,                // processed; see AddResult for fills / resting qty
    RejectedPrice,     // price outside 1 .. PriceRange-1
    RejectedQuantity,  // quantity == 0
    RejectedDuplicate, // id is already resting in the book
    PoolExhausted      // matched what it could, but the remainder could not rest
};

struct AddResult {
    AddStatus status{AddStatus::Ok};
    Quantity  filled{0};   // quantity executed immediately
    Quantity  rested{0};   // quantity now resting in the book
};

template <size_t MaxOrders, size_t PriceRange = 10000>
class OrderBook {
    static_assert(PriceRange >= 2, "need at least one valid price");

public:
    static constexpr Price kNoBid = 0;
    static constexpr Price kNoAsk = PriceRange;

    OrderBook() : bids_(PriceRange), asks_(PriceRange) {}

    // Process an incoming limit order: match against the opposite side at
    // price-time priority, then rest any remainder.
    AddResult add_order(OrderId id, Price price, Quantity qty, Side side) {
        AddResult r;
        if (price == 0 || price >= PriceRange) { r.status = AddStatus::RejectedPrice;     return r; }
        if (qty == 0)                          { r.status = AddStatus::RejectedQuantity;  return r; }
        if (id_to_order_.find(id))             { r.status = AddStatus::RejectedDuplicate; return r; }

        Quantity remaining = qty;
        if (side == Side::Buy) {
            while (remaining > 0 && price >= best_ask_) {
                sweep_level(asks_[best_ask_], remaining);
                advance_best_ask();
            }
        } else {
            while (remaining > 0 && price <= best_bid_) {
                sweep_level(bids_[best_bid_], remaining);
                advance_best_bid();
            }
        }
        r.filled = qty - remaining;
        if (remaining == 0) return r;

        Order* order = pool_.allocate();
        if (!order) { r.status = AddStatus::PoolExhausted; return r; }
        order->id = id;
        order->price = price;
        order->quantity = remaining;
        order->side = side;
        id_to_order_.insert(id, order);
        r.rested = remaining;

        if (side == Side::Buy) {
            bids_[price].push_back(order);
            if (price > best_bid_) best_bid_ = price;
        } else {
            asks_[price].push_back(order);
            if (price < best_ask_) best_ask_ = price;
        }
        return r;
    }

    // Cancel a resting order (also valid after a partial fill).
    bool cancel_order(OrderId id) {
        Order* order = id_to_order_.find(id);
        if (!order) return false;

        if (order->side == Side::Buy) {
            bids_[order->price].remove(order);
            if (order->price == best_bid_) advance_best_bid();
        } else {
            asks_[order->price].remove(order);
            if (order->price == best_ask_) advance_best_ask();
        }
        id_to_order_.erase(id);
        pool_.deallocate(order);
        return true;
    }

    // Best prices; 0 means that side of the book is empty (see has_bid/has_ask).
    Price get_best_bid() const { return best_bid_; }
    Price get_best_ask() const { return best_ask_ == kNoAsk ? 0 : best_ask_; }
    bool  has_bid() const { return best_bid_ != kNoBid; }
    bool  has_ask() const { return best_ask_ != kNoAsk; }

    // Remaining quantity of a resting order, 0 if the id is not resting.
    Quantity order_quantity(OrderId id) const {
        const Order* o = id_to_order_.find(id);
        return o ? o->quantity : 0;
    }
    size_t order_count() const { return pool_.in_use(); }

    // Total resting volume at a price (0 for out-of-range prices).
    uint64_t volume_at(Side side, Price price) const {
        if (price == 0 || price >= PriceRange) return 0;
        return (side == Side::Buy ? bids_ : asks_)[price].total_volume;
    }

    // O(PriceRange + orders) structural self-check, for tests.
    bool validate() const {
        size_t seen = 0;
        Price  lo_ask = kNoAsk, hi_bid = kNoBid;
        for (size_t side = 0; side < 2; ++side) {
            const auto& levels = side == 0 ? bids_ : asks_;
            for (size_t p = 0; p < PriceRange; ++p) {
                const PriceLevel& lv = levels[p];
                uint64_t vol = 0;
                uint32_t cnt = 0;
                const Order* prev = nullptr;
                for (const Order* o = lv.head; o; o = o->next) {
                    if (o->prev != prev || o->price != p || o->quantity == 0) return false;
                    if ((o->side == Side::Buy) != (side == 0)) return false;
                    if (id_to_order_.find(o->id) != o) return false;
                    vol += o->quantity; ++cnt; prev = o;
                }
                if (prev != lv.tail || vol != lv.total_volume || cnt != lv.order_count) return false;
                if ((lv.head == nullptr) != (cnt == 0)) return false;
                seen += cnt;
                if (cnt) {
                    if (side == 0) hi_bid = std::max<Price>(hi_bid, p);
                    else           lo_ask = std::min<Price>(lo_ask, p);
                }
            }
        }
        return seen == pool_.in_use() && seen == id_to_order_.size()
            && hi_bid == best_bid_ && lo_ask == best_ask_
            && (!has_bid() || !has_ask() || best_bid_ < best_ask_);
    }

private:
    // Fill from the front of `level` until `remaining` is zero or it is empty.
    void sweep_level(PriceLevel& level, Quantity& remaining) {
        Order* current = level.head;
        while (current && remaining > 0) {
            Quantity fill = std::min(remaining, current->quantity);
            current->quantity -= fill;
            level.total_volume -= fill;
            remaining -= fill;
            if (current->quantity == 0) {
                Order* filled = current;
                current = current->next;
                id_to_order_.erase(filled->id);
                level.remove(filled);
                pool_.deallocate(filled);
            }
        }
    }

    // Move the best pointers to the next non-empty level (or the sentinel).
    void advance_best_bid() {
        while (best_bid_ != kNoBid && bids_[best_bid_].order_count == 0) --best_bid_;
    }
    void advance_best_ask() {
        while (best_ask_ != kNoAsk && asks_[best_ask_].order_count == 0) ++best_ask_;
    }

    OrderPool<MaxOrders>        pool_;
    FlatHashMap<MaxOrders * 2>  id_to_order_;
    std::vector<PriceLevel>     bids_;
    std::vector<PriceLevel>     asks_;
    Price                       best_bid_{kNoBid};
    Price                       best_ask_{kNoAsk};
};

}  // namespace lob
