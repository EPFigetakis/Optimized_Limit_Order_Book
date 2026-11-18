// baseline_book.hpp -- deliberately naive reference book built from standard
// containers: std::map<price, std::list<order>> per side + std::unordered_map
// for id lookup.  Used as (a) the oracle in the differential test and (b) the
// comparison baseline in the benchmark.  Semantics match lob::OrderBook exactly,
// including rejection rules and "pool exhausted" behaviour (see max_orders).
#pragma once

#include "lob.hpp"

#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace lob {

class BaselineBook {
public:
    // price_range: valid prices are 1 .. price_range-1.
    // max_orders:  0 = unlimited; otherwise a new order cannot rest once this
    //              many orders are resting (mirrors the fixed-size pool).
    explicit BaselineBook(size_t price_range, size_t max_orders = 0)
        : price_range_(price_range), max_orders_(max_orders) {}

    // If `touched` is non-null, ids of every resting order whose quantity
    // changed or that was removed by matching are appended to it.
    AddResult add_order(OrderId id, Price price, Quantity qty, Side side,
                        std::vector<OrderId>* touched = nullptr) {
        AddResult r;
        if (price == 0 || price >= price_range_) { r.status = AddStatus::RejectedPrice;     return r; }
        if (qty == 0)                            { r.status = AddStatus::RejectedQuantity;  return r; }
        if (index_.count(id))                    { r.status = AddStatus::RejectedDuplicate; return r; }

        Quantity remaining = qty;
        if (side == Side::Buy) match(asks_, remaining, touched, [&](Price p) { return p <= price; });
        else                   match(bids_, remaining, touched, [&](Price p) { return p >= price; });
        r.filled = qty - remaining;
        if (remaining == 0) return r;

        if (max_orders_ && index_.size() >= max_orders_) { r.status = AddStatus::PoolExhausted; return r; }

        if (side == Side::Buy) rest(bids_, id, price, remaining, side);
        else                   rest(asks_, id, price, remaining, side);
        r.rested = remaining;
        return r;
    }

    bool cancel_order(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Loc loc = it->second;
        if (loc.side == Side::Buy) erase_from(bids_, loc);
        else                       erase_from(asks_, loc);
        index_.erase(it);
        return true;
    }

    Price get_best_bid() const { return bids_.empty() ? 0 : bids_.begin()->first; }
    Price get_best_ask() const { return asks_.empty() ? 0 : asks_.begin()->first; }

    Quantity order_quantity(OrderId id) const {
        auto it = index_.find(id);
        return it == index_.end() ? 0 : it->second.it->quantity;
    }
    size_t order_count() const { return index_.size(); }

    uint64_t volume_at(Side side, Price price) const {
        uint64_t v = 0;
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            if (it != bids_.end()) for (const auto& o : it->second) v += o.quantity;
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end()) for (const auto& o : it->second) v += o.quantity;
        }
        return v;
    }

    // Visit every resting order as (id, price, side, remaining quantity).
    template <class F>
    void for_each_order(F&& f) const {
        for (const auto& kv : index_)
            f(kv.first, kv.second.it->price, kv.second.side, kv.second.it->quantity);
    }

private:
    struct Rest { OrderId id; Price price; Quantity quantity; };
    using Level = std::list<Rest>;
    template <class Cmp> using Book = std::map<Price, Level, Cmp>;
    using Bids = Book<std::greater<Price>>;
    using Asks = Book<std::less<Price>>;

    struct Loc { Side side; Price price; Level::iterator it; };

    template <class B, class Pred>
    void match(B& book, Quantity& remaining, std::vector<OrderId>* touched, Pred crosses) {
        while (remaining > 0 && !book.empty() && crosses(book.begin()->first)) {
            Level& level = book.begin()->second;
            while (!level.empty() && remaining > 0) {
                Rest& o = level.front();
                Quantity fill = std::min(remaining, o.quantity);
                o.quantity -= fill;
                remaining -= fill;
                if (touched) touched->push_back(o.id);
                if (o.quantity == 0) {
                    index_.erase(o.id);
                    level.pop_front();
                }
            }
            if (level.empty()) book.erase(book.begin());
        }
    }

    template <class B>
    void rest(B& book, OrderId id, Price price, Quantity qty, Side side) {
        Level& level = book[price];
        level.push_back({id, price, qty});
        index_[id] = Loc{side, price, std::prev(level.end())};
    }

    template <class B>
    void erase_from(B& book, const Loc& loc) {
        auto lv = book.find(loc.price);
        lv->second.erase(loc.it);
        if (lv->second.empty()) book.erase(lv);
    }

    size_t price_range_;
    size_t max_orders_;
    Bids   bids_;
    Asks   asks_;
    std::unordered_map<OrderId, Loc> index_;
};

}  // namespace lob
