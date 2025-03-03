// OrderBook.h - 高性能订单簿实现
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <map>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <boost/lockfree/spsc_queue.hpp>

// 前向声明
namespace fix {
    class Session;
}

namespace hft {

// 价格类型使用整数表示以避免浮点误差
using Price = int64_t;
using Size = int64_t;
using OrderID = std::string;

// 价格转换工具
class PriceUtils {
public:
    static constexpr int PRICE_MULTIPLIER = 10000;
    
    static Price toInternalPrice(double price) {
        return static_cast<Price>(price * PRICE_MULTIPLIER);
    }
    
    static double toExternalPrice(Price price) {
        return static_cast<double>(price) / PRICE_MULTIPLIER;
    }
};

// 订单类型
enum class OrderType {
    LIMIT,      // 限价单
    MARKET,     // 市价单
    IOC,        // 立即成交或取消
    FOK,        // 全部成交或取消
    STOP,       // 止损单
    STOP_LIMIT  // 止损限价单
};

// 订单方向
enum class OrderSide {
    BUY,
    SELL
};

// 订单状态
enum class OrderStatus {
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    REJECTED,
    EXPIRED
};

// 订单结构
struct Order {
    OrderID id;
    std::string symbol;
    OrderSide side;
    OrderType type;
    Price price;
    Size quantity;
    Size filled;
    OrderStatus status;
    int64_t timestamp;
    std::string clientOrderId;
    
    Order(const OrderID& id, const std::string& symbol, OrderSide side, 
          OrderType type, Price price, Size quantity)
        : id(id), symbol(symbol), side(side), type(type), price(price),
          quantity(quantity), filled(0), status(OrderStatus::NEW),
          timestamp(0), clientOrderId("") {}
};

// 订单簿价格级别
struct PriceLevel {
    Price price;
    Size totalSize;
    std::vector<std::shared_ptr<Order>> orders;
    
    PriceLevel(Price price) : price(price), totalSize(0) {}
    
    void addOrder(std::shared_ptr<Order> order) {
        orders.push_back(order);
        totalSize += order->quantity - order->filled;
    }
    
    void removeOrder(const OrderID& orderId) {
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            if ((*it)->id == orderId) {
                totalSize -= ((*it)->quantity - (*it)->filled);
                orders.erase(it);
                return;
            }
        }
    }
    
    bool isEmpty() const {
        return orders.empty();
    }
};

// 成交记录
struct Trade {
    std::string symbol;
    Price price;
    Size quantity;
    int64_t timestamp;
    OrderID buyOrderId;
    OrderID sellOrderId;
    
    Trade(const std::string& symbol, Price price, Size quantity, 
          int64_t timestamp, const OrderID& buyOrderId, const OrderID& sellOrderId)
        : symbol(symbol), price(price), quantity(quantity), timestamp(timestamp),
          buyOrderId(buyOrderId), sellOrderId(sellOrderId) {}
};

// 订单簿更新事件类型
enum class BookUpdateType {
    NEW_ORDER,
    CANCEL_ORDER,
    MODIFY_ORDER,
    TRADE,
    SNAPSHOT
};

// 订单簿更新事件
struct BookUpdateEvent {
    BookUpdateType type;
    std::string symbol;
    std::shared_ptr<Order> order;
    std::shared_ptr<Trade> trade;
    int64_t timestamp;
    
    BookUpdateEvent(BookUpdateType type, const std::string& symbol, 
                   std::shared_ptr<Order> order = nullptr,
                   std::shared_ptr<Trade> trade = nullptr)
        : type(type), symbol(symbol), order(order), trade(trade),
          timestamp(0) {}
};

// 订单簿更新订阅接口
class OrderBookListener {
public:
    virtual ~OrderBookListener() = default;
    virtual void onBookUpdate(const BookUpdateEvent& event) = 0;
};

// 高性能订单簿实现
class OrderBook {
private:
    std::string symbol_;
    std::map<Price, PriceLevel, std::greater<Price>> bids_; // 降序排列买单
    std::map<Price, PriceLevel, std::less<Price>> asks_;    // 升序排列卖单
    std::unordered_map<OrderID, std::shared_ptr<Order>> ordersById_;
    std::vector<std::shared_ptr<OrderBookListener>> listeners_;
    std::mutex mutex_;
    
    // 无锁队列用于事件传播
    using EventQueue = boost::lockfree::spsc_queue<BookUpdateEvent>;
    std::unique_ptr<EventQueue> eventQueue_;
    std::atomic<bool> running_;
    std::thread eventThread_;

public:
    OrderBook(const std::string& symbol, size_t eventQueueSize = 10000)
        : symbol_(symbol), running_(false) {
        eventQueue_ = std::make_unique<EventQueue>(eventQueueSize);
        startEventProcessing();
    }
    
    ~OrderBook() {
        stopEventProcessing();
    }
    
    // 添加订单
    bool addOrder(std::shared_ptr<Order> order) {
        if (order->symbol != symbol_) {
            return false;
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查订单ID是否已存在
        if (ordersById_.find(order->id) != ordersById_.end()) {
            return false;
        }
        
        // 添加到内部数据结构
        ordersById_[order->id] = order;
        
        if (order->side == OrderSide::BUY) {
            auto& level = bids_[order->price];
            if (level.price == 0) {
                level.price = order->price;
            }
            level.addOrder(order);
        } else {
            auto& level = asks_[order->price];
            if (level.price == 0) {
                level.price = order->price;
            }
            level.addOrder(order);
        }
        
        // 发布更新事件
        publishEvent(BookUpdateEvent(BookUpdateType::NEW_ORDER, symbol_, order));
        
        // 尝试匹配订单（实际订单匹配逻辑更复杂，这里简化处理）
        matchOrders();
        
        return true;
    }
    
    // 取消订单
    bool cancelOrder(const OrderID& orderId) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = ordersById_.find(orderId);
        if (it == ordersById_.end()) {
            return false;
        }
        
        auto order = it->second;
        order->status = OrderStatus::CANCELED;
        
        // 从价格级别中移除
        if (order->side == OrderSide::BUY) {
            auto bid_it = bids_.find(order->price);
            if (bid_it != bids_.end()) {
                bid_it->second.removeOrder(orderId);
                if (bid_it->second.isEmpty()) {
                    bids_.erase(bid_it);
                }
            }
        } else {
            auto ask_it = asks_.find(order->price);
            if (ask_it != asks_.end()) {
                ask_it->second.removeOrder(orderId);
                if (ask_it->second.isEmpty()) {
                    asks_.erase(ask_it);
                }
            }
        }
        
        // 发布更新事件
        publishEvent(BookUpdateEvent(BookUpdateType::CANCEL_ORDER, symbol_, order));
        
        // 从订单映射中移除
        ordersById_.erase(it);
        
        return true;
    }
    
    // 修改订单
    bool modifyOrder(const OrderID& orderId, Price newPrice, Size newQuantity) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = ordersById_.find(orderId);
        if (it == ordersById_.end()) {
            return false;
        }
        
        // 简化处理：取消旧订单，添加新订单
        auto oldOrder = it->second;
        auto newOrder = std::make_shared<Order>(
            oldOrder->id, oldOrder->symbol, oldOrder->side,
            oldOrder->type, newPrice, newQuantity);
        newOrder->clientOrderId = oldOrder->clientOrderId;
        newOrder->timestamp = oldOrder->timestamp;
        
        // 从价格级别中移除旧订单
        if (oldOrder->side == OrderSide::BUY) {
            auto bid_it = bids_.find(oldOrder->price);
            if (bid_it != bids_.end()) {
                bid_it->second.removeOrder(orderId);
                if (bid_it->second.isEmpty()) {
                    bids_.erase(bid_it);
                }
            }
        } else {
            auto ask_it = asks_.find(oldOrder->price);
            if (ask_it != asks_.end()) {
                ask_it->second.removeOrder(orderId);
                if (ask_it->second.isEmpty()) {
                    asks_.erase(ask_it);
                }
            }
        }
        
        // 添加新订单到相应价格级别
        if (newOrder->side == OrderSide::BUY) {
            auto& level = bids_[newOrder->price];
            if (level.price == 0) {
                level.price = newOrder->price;
            }
            level.addOrder(newOrder);
        } else {
            auto& level = asks_[newOrder->price];
            if (level.price == 0) {
                level.price = newOrder->price;
            }
            level.addOrder(newOrder);
        }
        
        // 更新订单映射
        ordersById_[orderId] = newOrder;
        
        // 发布更新事件
        publishEvent(BookUpdateEvent(BookUpdateType::MODIFY_ORDER, symbol_, newOrder));
        
        // 尝试匹配订单
        matchOrders();
        
        return true;
    }
    
    // 获取订单
    std::shared_ptr<Order> getOrder(const OrderID& orderId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ordersById_.find(orderId);
        return (it != ordersById_.end()) ? it->second : nullptr;
    }
    
    // 获取最佳买价
    Price getBestBidPrice() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bids_.empty() ? 0 : bids_.begin()->first;
    }
    
    // 获取最佳卖价
    Price getBestAskPrice() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return asks_.empty() ? 0 : asks_.begin()->first;
    }
    
    // 获取指定深度的买单
    std::vector<PriceLevel> getBidLevels(size_t depth) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PriceLevel> levels;
        
        auto it = bids_.begin();
        for (size_t i = 0; i < depth && it != bids_.end(); ++i, ++it) {
            levels.push_back(it->second);
        }
        
        return levels;
    }
    
    // 获取指定深度的卖单
    std::vector<PriceLevel> getAskLevels(size_t depth) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PriceLevel> levels;
        
        auto it = asks_.begin();
        for (size_t i = 0; i < depth && it != asks_.end(); ++i, ++it) {
            levels.push_back(it->second);
        }
        
        return levels;
    }
    
    // 计算买单不平衡比例 (用于微观结构分析)
    double calculateBidImbalance(size_t depth = 5) const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        Size totalBidSize = 0;
        Size totalAskSize = 0;
        
        // 计算买单总量
        auto bid_it = bids_.begin();
        for (size_t i = 0; i < depth && bid_it != bids_.end(); ++i, ++bid_it) {
            totalBidSize += bid_it->second.totalSize;
        }
        
        // 计算卖单总量
        auto ask_it = asks_.begin();
        for (size_t i = 0; i < depth && ask_it != asks_.end(); ++i, ++ask_it) {
            totalAskSize += ask_it->second.totalSize;
        }
        
        // 计算不平衡比例
        if (totalBidSize + totalAskSize == 0) {
            return 0.0;
        }
        
        return static_cast<double>(totalBidSize - totalAskSize) / (totalBidSize + totalAskSize);
    }
    
    // 计算平均价差
    double calculateSpread() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (bids_.empty() || asks_.empty()) {
            return 0.0;
        }
        
        Price bestBid = bids_.begin()->first;
        Price bestAsk = asks_.begin()->first;
        
        return PriceUtils::toExternalPrice(bestAsk - bestBid);
    }
    
    // 添加订单簿监听器
    void addListener(std::shared_ptr<OrderBookListener> listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        listeners_.push_back(listener);
    }
    
    // 移除订单簿监听器
    void removeListener(std::shared_ptr<OrderBookListener> listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (*it == listener) {
                listeners_.erase(it);
                break;
            }
        }
    }
    
private:
    // 订单匹配逻辑
    void matchOrders() {
        // 只有在有买单和卖单的情况下才进行匹配
        if (bids_.empty() || asks_.empty()) {
            return;
        }
        
        // 获取最佳买价和最佳卖价
        Price bestBidPrice = bids_.begin()->first;
        Price bestAskPrice = asks_.begin()->first;
        
        // 当最佳买价大于等于最佳卖价时，可以匹配
        while (!bids_.empty() && !asks_.empty() && bestBidPrice >= bestAskPrice) {
            auto& bidLevel = bids_.begin()->second;
            auto& askLevel = asks_.begin()->second;
            
            if (bidLevel.orders.empty() || askLevel.orders.empty()) {
                break;
            }
            
            // 获取要匹配的订单
            auto& buyOrder = bidLevel.orders.front();
            auto& sellOrder = askLevel.orders.front();
            
            // 确定成交量
            Size tradeSize = std::min(buyOrder->quantity - buyOrder->filled, 
                                     sellOrder->quantity - sellOrder->filled);
            
            // 更新订单的成交量
            buyOrder->filled += tradeSize;
            sellOrder->filled += tradeSize;
            
            // 更新订单状态
            if (buyOrder->filled == buyOrder->quantity) {
                buyOrder->status = OrderStatus::FILLED;
            } else {
                buyOrder->status = OrderStatus::PARTIALLY_FILLED;
            }
            
            if (sellOrder->filled == sellOrder->quantity) {
                sellOrder->status = OrderStatus::FILLED;
            } else {
                sellOrder->status = OrderStatus::PARTIALLY_FILLED;
            }
            
            // 创建成交记录
            int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count();
            
            auto trade = std::make_shared<Trade>(
                symbol_, askLevel.price, tradeSize, timestamp, 
                buyOrder->id, sellOrder->id);
            
            // 发布成交事件
            publishEvent(BookUpdateEvent(BookUpdateType::TRADE, symbol_, nullptr, trade));
            
            // 处理已完全成交的订单
            if (buyOrder->status == OrderStatus::FILLED) {
                bidLevel.removeOrder(buyOrder->id);
                if (bidLevel.isEmpty()) {
                    bids_.erase(bids_.begin());
                }
            }
            
            if (sellOrder->status == OrderStatus::FILLED) {
                askLevel.removeOrder(sellOrder->id);
                if (askLevel.isEmpty()) {
                    asks_.erase(asks_.begin());
                }
            }
            
            // 更新最佳价格，准备下一轮匹配
            if (bids_.empty() || asks_.empty()) {
                break;
            }
            
            bestBidPrice = bids_.begin()->first;
            bestAskPrice = asks_.begin()->first;
        }
    }
    
    // 发布事件到事件队列
    void publishEvent(const BookUpdateEvent& event) {
        // 添加当前时间戳
        BookUpdateEvent eventWithTimestamp = event;
        eventWithTimestamp.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
        // 添加到事件队列
        while (!eventQueue_->push(eventWithTimestamp)) {
            // 队列满，稍等片刻再尝试
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
    
    // 启动事件处理线程
    void startEventProcessing() {
        running_ = true;
        eventThread_ = std::thread([this]() {
            BookUpdateEvent event(BookUpdateType::SNAPSHOT, "");
            
            while (running_) {
                bool processed = false;
                
                // 尝试从队列获取事件
                while (eventQueue_->pop(event)) {
                    // 通知所有监听器
                    for (const auto& listener : listeners_) {
                        listener->onBookUpdate(event);
                    }
                    processed = true;
                }
                
                // 如果没有处理任何事件，短暂休眠以降低CPU使用率
                if (!processed) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }
        });
    }
    
    // 停止事件处理线程
    void stopEventProcessing() {
        running_ = false;
        if (eventThread_.joinable()) {
            eventThread_.join();
        }
    }
};

} // namespace hft