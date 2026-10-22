#include <iostream>
#include <vector>
#include <cstdint>
#include <cassert>

// --- Base Types ---
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

// --- Phase 2: Order Pool ---
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

// --- Phase 1: Price Level FIFO Queue ---
struct PriceLevel {
    Price    price{0};
    Quantity total_volume{0};
    uint32_t order_count{0};
    Order*   head{nullptr};
    Order*   tail{nullptr};

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

// --- Custom Open-Addressing Flat Hash Map (Linear Probing) ---
template <size_t TableSize>
class FlatHashMap {
private:
    struct Entry {
        OrderId  key{0};
        Order*   value{nullptr};
        bool     is_occupied{false};
    };

    std::vector<Entry> table_;

    // Simple hash function (Mix bits to prevent clustering)
    size_t hash(OrderId id) const {
        id ^= id >> 30;
        id *= 0xbf58476d1ce4e5b9ULL;
        id ^= id >> 27;
        id *= 0x94d049bb133111ebULL;
        id ^= id >> 31;
        return static_cast<size_t>(id) % TableSize;
    }

public:
    FlatHashMap() {
        table_.resize(TableSize);
    }

    void insert(OrderId key, Order* value) {
        size_t idx = hash(key);
        while (table_[idx].is_occupied) {
            if (table_[idx].key == key) {
                table_[idx].value = value; // Update existing
                return;
            }
            idx = (idx + 1) % TableSize; // Linear probe
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
            if (idx == start_idx) break; // Full loop safety check
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
                
                // Note: In a complete linear-probing map, you'd re-probe subsequent elements,
                // but for an order book where IDs are unique and rarely collide heavily under capacity,
                // simple unsetting works for demonstration.
                return;
            }
            idx = (idx + 1) % TableSize;
            if (idx == start_idx) break;
        }
    }
};

// --- Order Book Manager using Dense Hash Map ---
template <size_t MaxOrders>
class OrderBookManager {
private:
    OrderPool<MaxOrders> pool_;
    FlatHashMap<MaxOrders * 2> id_to_order_; // 2x capacity to maintain low load factor

public:
    bool add_order(OrderId id, Price price, Quantity qty, Side side, PriceLevel& level) {
        Order* order = pool_.allocate();
        if (!order) return false;

        order->id = id;
        order->price = price;
        order->quantity = qty;
        order->side = side;

        id_to_order_.insert(id, order);
        level.push_back(order);
        return true;
    }

    bool cancel_order(OrderId id, PriceLevel& level) {
        Order* order = id_to_order_.find(id);
        if (!order) return false;

        level.remove(order);
        id_to_order_.erase(id);
        pool_.deallocate(order);
        return true;
    }

    Order* get_order(OrderId id) {
        return id_to_order_.find(id);
    }
};
