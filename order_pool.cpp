#include <iostream>
#include <vector>
#include <cstdint>

//Base Types Just for Local File Testing / Tutorial 

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

template <size_t Capacity>
class OrderPool
{
private:
    std::vector<Order> pool_;
    Order* free_head_{nullptr};
public:
    OrderPool(){
        pool_.resize(Capacity);

        for (size_t i =0; i = Capacity-1;i++){
            pool_[i].next = &pool_[i+1];
        }
        pool_[Capacity - 1].next = nullptr;
        free_head_ = &pool_[0];

    }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete; 

    Order* allocate(){
        if (!free_head_){
            return nullptr;
        }

        Order* order = free_head_;
        free_head_ = free_head_ -> next; 

        order-> prev = nullptr;
        order-> next = nullptr;
        return order; 
    }

    void deallocate(Order* Order){
        order -> prev = nullptr;
        order -> next = free_head_;
        free_head_ = order;
    }

};
