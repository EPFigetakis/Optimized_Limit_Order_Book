#include <iostream>
#include <vector>
#include <cstdint>
#include <cassert>
#include <algorithm>

// ==========================================
// BASE TYPES & STRUCTS
// ==========================================
using OrderId  = uint64_t;
using Price    = uint64_t;
using Quantity = uint32_t;

enum class Side : uint8_t { Buy, Sell };

struct Order {
    OrderId  id;
    Price    price;
    Quantity quantity;
    Side     side;

    Order* prev{nullptr};
    Order* next{nullptr};
};


// ==========================================
// PHASE 1: INTRUSIVE FIFO PRICE LEVEL QUEUE
// ==========================================
struct PriceLevel {
    Price    price{0};
    Quantity total_volume{0};
    uint32_t order_count{0};
    Order*   head{nullptr}; // Oldest order (FIFO priority)
    Order*   tail{nullptr}; // Newest order

    void push_back(Order* order) {
        order->prev = tail;
        order->next = nullptr;
        if (tail) { tail->next = order; }
        else { head = order; }
        tail = order;
        total_volume += order->quantity;
        order_count++;
    }

    void remove(Order* order) {
        if (order->prev) { order->prev->next = order->next; }
        else { head = order->next; }

        if (order->next) { order->next->prev = order->prev; }
        else { tail = order->prev; }

        total_volume -= order->quantity;
        order_count--;
        order->prev = nullptr;
        order->next = nullptr;
    }
};


// ==========================================
// PHASE 2: ZERO-ALLOCATION STATIC ORDER POOL
// ==========================================
template <size_t Capacity>
class OrderPool {
private:
    std::vector<Order> pool_;
    Order* free_head_{nullptr};

public:
    OrderPool() {
        pool_.resize(Capacity);
        for (size_t i = 0; i < Capacity - 1; ++i) {
            pool_[i].next = &pool_[i + 1];
        }
        pool_[Capacity - 1].next = nullptr;
        free_head_ = &pool_[0];
    }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    Order* allocate() {
        if (!free_head_) return nullptr;
        Order* order = free_head_;
        free_head_ = free_head_->next;
        order->prev = nullptr;
        order->next = nullptr;
        return order;
    }

    void deallocate(Order* order) {
        order->prev = nullptr;
        order->next = free_head_;
        free_head_ = order;
    }
};


// ==========================================
// PHASE 3: OPEN-ADDRESSING FLAT HASH MAP
// ==========================================
template <size_t TableSize>
class FlatHashMap {
private:
    struct Entry {
        OrderId  key{0};
        Order*   value{nullptr};
        bool     is_occupied{false};
    };

    std::vector<Entry> table_;

    size_t hash(OrderId id) const {
        id ^= id >> 30;
        id *= 0xbf58476d1ce4e5b9ULL;
        id ^= id >> 27;
        id *= 0x94d049bb133111ebULL;
        id ^= id >> 31;
        return static_cast<size_t>(id) % TableSize;
    }

public:
    FlatHashMap() { table_.resize(TableSize); }

    void insert(OrderId key, Order* value) {
        size_t idx = hash(key);
        while (table_[idx].is_occupied) {
            if (table_[idx].key == key) {
                table_[idx].value = value;
                return;
            }
            idx = (idx + 1) % TableSize;
        }
        table_[idx] = {key, value, true};
    }

    Order* find(OrderId key) const {
        size_t idx = hash(key);
        size_t start_idx = idx;

        while (table_[idx].is_occupied) {
            if (table_[idx].key == key) {
                return table_[idx].value;
            }
            idx = (idx + 1) % TableSize;
            if (idx == start_idx) break;
        }
        return nullptr;
    }

    void erase(OrderId key) {
        size_t idx = hash(key);
        size_t start_idx = idx;

        while (table_[idx].is_occupied) {
            if (table_[idx].key == key) {
                table_[idx].is_occupied = false;
                table_[idx].key = 0;
                table_[idx].value = nullptr;
                return;
            }
            idx = (idx + 1) % TableSize;
            if (idx == start_idx) break;
        }
    }
};


// ==========================================
// PHASE 4: TOP-LEVEL ORDER BOOK & MATCHING ENGINE
// ==========================================
template <size_t MaxOrders, size_t PriceRange = 10000>
class OrderBook {
private:
    OrderPool<MaxOrders> pool_;
    FlatHashMap<MaxOrders * 2> id_to_order_;

    // Fixed price-level arrays representing Bids and Asks
    // Index represents the price tick offset from a base price
    PriceLevel bids_[PriceRange];
    PriceLevel asks_[PriceRange];

    Price best_bid_{0};
    Price best_ask_{UINT64_MAX};

    void update_best_bid() {
        while (best_bid_ > 0 && bids_[best_bid_].order_count == 0) {
            best_bid_--;
        }
        if (best_bid_ > 0 && bids_[best_bid_].order_count == 0) {
            best_bid_ = 0;
        }
    }

    void update_best_ask() {
        while (best_ask_ < PriceRange && asks_[best_ask_].order_count == 0) {
            best_ask_++;
        }
        if (best_ask_ < PriceRange && asks_[best_ask_].order_count == 0) {
            best_ask_ = UINT64_MAX;
        }
    }

public:
    OrderBook() {
        for (size_t i = 0; i < PriceRange; ++i) {
            bids_[i].price = i;
            asks_[i].price = i;
        }
    }

    // Process an incoming limit order (may match immediately or rest in book)
    void add_order(OrderId id, Price price, Quantity qty, Side side) {
        Quantity incoming_qty = qty;

        // --- AGGRESSOR PHASE (Matching against opposite side) ---
        if (side == Side::Buy) {
            // Match against Asks if buy price crosses or meets best ask
            while (incoming_qty > 0 && best_ask_ != UINT64_MAX && price >= best_ask_) {
                PriceLevel& level = asks_[best_ask_];
                Order* current = level.head;

                while (current && incoming_qty > 0) {
                    Quantity fill_qty = std::min(incoming_qty, current->quantity);
                    current->quantity -= fill_qty;
                    incoming_qty -= fill_qty;
                    level.total_volume -= fill_qty;

                    if (current->quantity == 0) {
                        Order* filled = current;
                        current = current->next;
                        id_to_order_.erase(filled->id);
                        level.remove(filled);
                        pool_.deallocate(filled);
                    }
                }
                update_best_ask();
            }

            // --- RESTING PHASE (If quantity remains, add to Bids) ---
            if (incoming_qty > 0) {
                Order* order = pool_.allocate();
                if (!order) return; // Pool exhausted

                order->id = id;
                order->price = price;
                order->quantity = incoming_qty;
                order->side = side;

                id_to_order_.insert(id, order);
                bids_[price].push_back(order);

                if (price > best_bid_) {
                    best_bid_ = price;
                }
            }

        } else { // Side::Sell
            // Match against Bids if sell price crosses or meets best bid
            while (incoming_qty > 0 && best_bid_ != 0 && price <= best_bid_) {
                PriceLevel& level = bids_[best_bid_];
                Order* current = level.head;

                while (current && incoming_qty > 0) {
                    Quantity fill_qty = std::min(incoming_qty, current->quantity);
                    current->quantity -= fill_qty;
                    incoming_qty -= fill_qty;
                    level.total_volume -= fill_qty;

                    if (current->quantity == 0) {
                        Order* filled = current;
                        current = current->next;
                        id_to_order_.erase(filled->id);
                        level.remove(filled);
                        pool_.deallocate(filled);
                    }
                }
                update_best_bid();
            }

            // --- RESTING PHASE (If quantity remains, add to Asks) ---
            if (incoming_qty > 0) {
                Order* order = pool_.allocate();
                if (!order) return;

                order->id = id;
                order->price = price;
                order->quantity = incoming_qty;
                order->side = side;

                id_to_order_.insert(id, order);
                asks_[price].push_back(order);

                if (price < best_ask_) {
                    best_ask_ = price;
                }
            }
        }
    }

    // O(1) Cancellation
    bool cancel_order(OrderId id) {
        Order* order = id_to_order_.find(id);
        if (!order) return false;

        if (order->side == Side::Buy) {
            bids_[order->price].remove(order);
            update_best_bid();
        } else {
            asks_[order->price].remove(order);
            update_best_ask();
        }

        id_to_order_.erase(id);
        pool_.deallocate(order);
        return true;
    }

    Price get_best_bid() const { return best_bid_ == 0 ? 0 : best_bid_; }
    Price get_best_ask() const { return best_ask_ == UINT64_MAX ? 0 : best_ask_; }
};


// ==========================================
// TEST SUITE
// ==========================================
void test_full_matching_engine() {
    std::cout << "[RUN] Testing Full OrderBook Matching & Crossing...\n";

    OrderBook<1000, 10000> book;

    // 1. Add resting limit sells (Asks)
    book.add_order(1, 1010, 100, Side::Sell); // Ask at $10.10
    book.add_order(2, 1015, 200, Side::Sell); // Ask at $10.15

    assert(book.get_best_ask() == 1010);

    // 2. Add an aggressive buy order that crosses the spread and matches partially
    book.add_order(3, 1010, 60, Side::Buy); // Buys 60 from Order 1 at $10.10
    
    // Order 1 should have 40 remaining at $10.10, best ask is still $10.10
    assert(book.get_best_ask() == 1010);

    // 3. Add another buy order that clears the rest of Order 1 and rests remainder
    book.add_order(4, 1012, 50, Side::Buy); 
    // This clears the remaining 40 of Order 1, and the remaining 10 of buy order rests as a new Bid at $10.12!
    
    assert(book.get_best_ask() == 1015); // Best ask is now Order 2 at $10.15
    assert(book.get_best_bid() == 1012); // Best bid is now our resting buy at $10.12

    std::cout << "  [PASS] Full matching engine and cross verification passed successfully!\n\n";
}

int main() {
    std::cout << "=== Running Phase 4: Complete Engine Tests ===\n\n";
    test_full_matching_engine();
    std::cout << "All LOB phases completed successfully!\n";
    return 0;
}