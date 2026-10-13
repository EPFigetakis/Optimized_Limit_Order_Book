#include <cstdint>
#include <iostream>
#include <cassert>
#include <algorithm>




using OrderId = uint64_t;
using Price = uint64_t;
using Quantity = uint32_t;


enum class Side : uint8_t{
    Buy,
    Sell
};

struct Order
{
    OrderId id;
    Price price;
    Quantity quantity; 
    Side side;

    Order* prev{nullptr};
    Order* next{nullptr};
};


struct PriceLevel
{
    Price price{0};
    Quantity total_volume{0};
    uint32_t order_count{0};
    Order* head{nullptr};
    Order* tail{nullptr};


    void push_back(Order* order){
        order -> prev = tail;
        order -> next = nullptr;
        if (tail){
            tail -> next = order ;
        } else {
            head = order;
        }

        tail = order;

        total_volume += order -> quantity;
        order_count++;

    }

    void remove(Order* order){
        if (order -> prev){
            order->prev->next = order->next;
        }else{
            head = order -> next;
        }

        if (order->next){
            order->next->prev = order->prev;
        } else{
            tail = order -> prev;
        }
        total_volume -= order->quantity;
        order_count --;

        order -> prev = nullptr;
        order -> next = nullptr; 

    }
};