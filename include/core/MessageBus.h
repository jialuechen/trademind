// MessageBus.h - 低延迟消息系统
#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <zmq.hpp>

#include "OrderBook.h"  // 引用前面定义的订单簿组件

namespace hft {

// 消息类型定义
enum class MessageType {
    MARKET_DATA,         // 市场数据
    ORDER,               // 订单相关消息
    TRADE,               // 成交相关消息
    HEARTBEAT,           // 心跳消息
    STRATEGY_COMMAND,    // 策略命令
    SYSTEM_COMMAND,      // 系统命令
    LOG                  // 日志消息
};

// 基础消息结构
struct Message {
    MessageType type;
    std::string source;
    std::string target;
    int64_t timestamp;
    std::string payload;  // JSON格式数据

    Message() : type(MessageType::HEARTBEAT), timestamp(0) {}
    
    Message(MessageType type, const std::string& source, 
            const std::string& target, const std::string& payload)
        : type(type), source(source), target(target), payload(payload) {
        timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
};

// 消息序列化与反序列化
class MessageSerializer {
public:
    // 消息序列化为二进制
    static std::vector<uint8_t> serialize(const Message& message) {
        // 在实际系统中，可使用Protobuf、FlatBuffers或自定义二进制格式
        // 这里简化为字符串格式用于示例
        std::string data = std::to_string(static_cast<int>(message.type)) + "|" +
                          message.source + "|" +
                          message.target + "|" +
                          std::to_string(message.timestamp) + "|" +
                          message.payload;

        std::vector<uint8_t> result(data.begin(), data.end());
        return result;
    }

    // 从二进制反序列化为消息
    static Message deserialize(const std::vector<uint8_t>& data) {
        std::string str_data(data.begin(), data.end());
        
        Message message;
        size_t pos = 0;
        size_t next_pos = str_data.find('|', pos);
        
        if (next_pos != std::string::npos) {
            message.type = static_cast<MessageType>(std::stoi(str_data.substr(pos, next_pos - pos)));
            pos = next_pos + 1;
        }
        
        next_pos = str_data.find('|', pos);
        if (next_pos != std::string::npos) {
            message.source = str_data.substr(pos, next_pos - pos);
            pos = next_pos + 1;
        }
        
        next_pos = str_data.find('|', pos);
        if (next_pos != std::string::npos) {
            message.target = str_data.substr(pos, next_pos - pos);
            pos = next_pos + 1;
        }
        
        next_pos = str_data.find('|', pos);
        if (next_pos != std::string::npos) {
            message.timestamp = std::stoll(str_data.substr(pos, next_pos - pos));
            pos = next_pos + 1;
        }
        
        message.payload = str_data.substr(pos);
        
        return message;
    }
};

// 消息总线接口
class MessageBus {
public:
    virtual ~MessageBus() = default;
    
    // 发布消息
    virtual bool publish(const Message& message) = 0;
    
    // 订阅特定类型的消息
    virtual bool subscribe(MessageType type, 
                           const std::function<void(const Message&)>& callback) = 0;
    
    // 取消订阅
    virtual bool unsubscribe(MessageType type) = 0;
    
    // 启动消息总线
    virtual bool start() = 0;
    
    // 停止消息总线
    virtual bool stop() = 0;
};

// 基于ZeroMQ实现的高性能消息总线
class ZeroMQMessageBus : public MessageBus {
private:
    std::string publisher_endpoint_;
    std::string subscriber_endpoint_;
    
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> publisher_;
    std::unique_ptr<zmq::socket_t> subscriber_;
    
    std::thread subscriber_thread_;
    std::atomic<bool> running_;
    
    std::map<MessageType, std::vector<std::function<void(const Message&)>>> callbacks_;
    std::mutex callbacks_mutex_;

public:
    ZeroMQMessageBus(const std::string& publisher_endpoint = "tcp://*:5555",
                    const std::string& subscriber_endpoint = "tcp://localhost:5555")
        : publisher_endpoint_(publisher_endpoint),
          subscriber_endpoint_(subscriber_endpoint),
          running_(false) {
    }
    
    ~ZeroMQMessageBus() {
        stop();
    }
    
    // 发布消息
    bool publish(const Message& message) override {
        if (!publisher_ || !running_) {
            return false;
        }
        
        try {
            auto serialized = MessageSerializer::serialize(message);
            
            // 使用消息类型作为主题
            std::string topic = std::to_string(static_cast<int>(message.type));
            
            zmq::message_t topic_msg(topic.size());
            memcpy(topic_msg.data(), topic.data(), topic.size());
            publisher_->send(topic_msg, zmq::send_flags::sndmore);
            
            zmq::message_t body_msg(serialized.size());
            memcpy(body_msg.data(), serialized.data(), serialized.size());
            publisher_->send(body_msg, zmq::send_flags::none);
            
            return true;
        } catch (const zmq::error_t& e) {
            std::cerr << "ZeroMQ发布消息错误: " << e.what() << std::endl;
            return false;
        }
    }
    
    // 订阅特定类型的消息
    bool subscribe(MessageType type, 
                  const std::function<void(const Message&)>& callback) override {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_[type].push_back(callback);
        
        if (subscriber_) {
            try {
                // 订阅指定类型的消息
                std::string topic = std::to_string(static_cast<int>(type));
                subscriber_->set(zmq::sockopt::subscribe, topic);
                return true;
            } catch (const zmq::error_t& e) {
                std::cerr << "ZeroMQ订阅错误: " << e.what() << std::endl;
                return false;
            }
        }
        
        return true;
    }
    
    // 取消订阅
    bool unsubscribe(MessageType type) override {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        
        if (subscriber_) {
            try {
                // 取消订阅指定类型的消息
                std::string topic = std::to_string(static_cast<int>(type));
                subscriber_->set(zmq::sockopt::unsubscribe, topic);
            } catch (const zmq::error_t& e) {
                std::cerr << "ZeroMQ取消订阅错误: " << e.what() << std::endl;
                return false;
            }
        }
        
        callbacks_.erase(type);
        return true;
    }
    
    // 启动消息总线
    bool start() override {
        if (running_) {
            return true;
        }
        
        try {
            // 创建ZeroMQ上下文
            context_ = std::make_unique<zmq::context_t>(1);
            
            // 创建发布者套接字
            publisher_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::pub);
            publisher_->bind(publisher_endpoint_);
            
            // 创建订阅者套接字
            subscriber_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::sub);
            subscriber_->connect(subscriber_endpoint_);
            
            // 默认不订阅任何主题
            // 需要调用subscribe方法来订阅特定主题
            
            // 设置高水位标记以控制缓冲区大小
            publisher_->set(zmq::sockopt::sndhwm, 1000);
            subscriber_->set(zmq::sockopt::rcvhwm, 1000);
            
            // 设置为非阻塞模式
            publisher_->set(zmq::sockopt::sndtimeo, 1);
            subscriber_->set(zmq::sockopt::rcvtimeo, 1);
            
            running_ = true;
            
            // 启动订阅线程
            subscriber_thread_ = std::thread([this]() {
                subscriber_loop();
            });
            
            return true;
        } catch (const zmq::error_t& e) {
            std::cerr << "ZeroMQ启动错误: " << e.what() << std::endl;
            return false;
        }
    }
    
    // 停止消息总线
    bool stop() override {
        if (!running_) {
            return true;
        }
        
        running_ = false;
        
        if (subscriber_thread_.joinable()) {
            subscriber_thread_.join();
        }
        
        subscriber_.reset();
        publisher_.reset();
        context_.reset();
        
        return true;
    }
    
private:
    // 订阅者线程循环
    void subscriber_loop() {
        while (running_) {
            try {
                // 接收主题
                zmq::message_t topic_msg;
                auto result = subscriber_->recv(topic_msg, zmq::recv_flags::dontwait);
                
                if (!result.has_value()) {
                    // 没有消息，短暂休眠以降低CPU使用率
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
                
                // 接收消息体
                zmq::message_t body_msg;
                subscriber_->recv(body_msg);
                
                // 解析主题
                std::string topic(static_cast<char*>(topic_msg.data()), topic_msg.size());
                MessageType type = static_cast<MessageType>(std::stoi(topic));
                
                // 反序列化消息
                std::vector<uint8_t> data(static_cast<uint8_t*>(body_msg.data()),
                                         static_cast<uint8_t*>(body_msg.data()) + body_msg.size());
                Message message = MessageSerializer::deserialize(data);
                
                // 调用回调函数
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                auto it = callbacks_.find(type);
                if (it != callbacks_.end()) {
                    for (const auto& callback : it->second) {
                        callback(message);
                    }
                }
            } catch (const zmq::error_t& e) {
                if (zmq_errno() != EAGAIN) {
                    std::cerr << "ZeroMQ接收错误: " << e.what() << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "订阅者线程错误: " << e.what() << std::endl;
            }
        }
    }
};

// 分布式系统服务接口
class Service {
public:
    virtual ~Service() = default;
    
    // 服务名称
    virtual std::string name() const = 0;
    
    // 启动服务
    virtual bool start() = 0;
    
    // 停止服务
    virtual bool stop() = 0;
    
    // 是否正在运行
    virtual bool is_running() const = 0;
    
    // 处理消息
    virtual void handle_message(const Message& message) = 0;
};

// 服务管理器 - 用于协调和管理分布式系统中的多个服务
class ServiceManager {
private:
    std::shared_ptr<MessageBus> message_bus_;
    std::map<std::string, std::shared_ptr<Service>> services_;
    std::mutex services_mutex_;
    std::atomic<bool> running_;
    
    // 心跳检测线程
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_;
    
    // 配置
    struct Config {
        int heartbeat_interval_ms;
        std::string manager_id;
    };
    
    Config config_;

public:
    ServiceManager(std::shared_ptr<MessageBus> message_bus, 
                  const std::string& manager_id = "service_manager",
                  int heartbeat_interval_ms = 1000)
        : message_bus_(message_bus), running_(false), heartbeat_running_(false) {
        
        config_.heartbeat_interval_ms = heartbeat_interval_ms;
        config_.manager_id = manager_id;
    }
    
    ~ServiceManager() {
        stop();
    }
    
    // 注册服务
    bool register_service(std::shared_ptr<Service> service) {
        std::lock_guard<std::mutex> lock(services_mutex_);
        
        if (services_.find(service->name()) != services_.end()) {
            std::cerr << "服务已存在: " << service->name() << std::endl;
            return false;
        }
        
        services_[service->name()] = service;
        return true;
    }
    
    // 注销服务
    bool unregister_service(const std::string& service_name) {
        std::lock_guard<std::mutex> lock(services_mutex_);
        
        auto it = services_.find(service_name);
        if (it == services_.end()) {
            std::cerr << "服务不存在: " << service_name << std::endl;
            return false;
        }
        
        if (it->second->is_running()) {
            it->second->stop();
        }
        
        services_.erase(it);
        return true;
    }
    
    // 启动所有服务
    bool start() {
        if (running_) {
            return true;
        }
        
        if (!message_bus_->start()) {
            std::cerr << "无法启动消息总线" << std::endl;
            return false;
        }
        
        // 订阅系统命令消息
        message_bus_->subscribe(MessageType::SYSTEM_COMMAND, 
            [this](const Message& message) {
                handle_system_command(message);
            });
        
        // 启动所有服务
        std::lock_guard<std::mutex> lock(services_mutex_);
        for (auto& [name, service] : services_) {
            if (!service->start()) {
                std::cerr << "无法启动服务: " << name << std::endl;
                return false;
            }
        }
        
        running_ = true;
        
        // 启动心跳线程
        heartbeat_running_ = true;
        heartbeat_thread_ = std::thread([this]() {
            heartbeat_loop();
        });
        
        return true;
    }
    
    // 停止所有服务
    bool stop() {
        if (!running_) {
            return true;
        }
        
        // 停止心跳线程
        heartbeat_running_ = false;
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        
        // 停止所有服务
        std::lock_guard<std::mutex> lock(services_mutex_);
        for (auto& [name, service] : services_) {
            service->stop();
        }
        
        // 取消订阅
        message_bus_->unsubscribe(MessageType::SYSTEM_COMMAND);
        
        // 停止消息总线
        message_bus_->stop();
        
        running_ = false;
        
        return true;
    }
    
private:
    // 处理系统命令
    void handle_system_command(const Message& message) {
        if (message.target != config_.manager_id && !message.target.empty()) {
            return;  // 不是发给此管理器的命令
        }
        
        // 解析命令JSON
        // 在实际系统中，使用JSON库如nlohmann/json或RapidJSON解析
        // 这里简化为字符串匹配示例
        
        if (message.payload.find("\"command\":\"start_service\"") != std::string::npos) {
            // 启动特定服务
            size_t pos = message.payload.find("\"service_name\":\"");
            if (pos != std::string::npos) {
                pos += 16;  // 跳过 "service_name":"
                size_t end = message.payload.find("\"", pos);
                if (end != std::string::npos) {
                    std::string service_name = message.payload.substr(pos, end - pos);
                    
                    std::lock_guard<std::mutex> lock(services_mutex_);
                    auto it = services_.find(service_name);
                    if (it != services_.end()) {
                        it->second->start();
                    }
                }
            }
        } else if (message.payload.find("\"command\":\"stop_service\"") != std::string::npos) {
            // 停止特定服务
            size_t pos = message.payload.find("\"service_name\":\"");
            if (pos != std::string::npos) {
                pos += 16;  // 跳过 "service_name":"
                size_t end = message.payload.find("\"", pos);
                if (end != std::string::npos) {
                    std::string service_name = message.payload.substr(pos, end - pos);
                    
                    std::lock_guard<std::mutex> lock(services_mutex_);
                    auto it = services_.find(service_name);
                    if (it != services_.end()) {
                        it->second->stop();
                    }
                }
            }
        } else if (message.payload.find("\"command\":\"status\"") != std::string::npos) {
            // 返回状态信息
            send_status_response(message.source);
        }
    }
    
    // 发送状态响应
    void send_status_response(const std::string& target) {
        std::string status = "{\"status\":\"ok\",\"services\":[";
        
        std::lock_guard<std::mutex> lock(services_mutex_);
        bool first = true;
        for (const auto& [name, service] : services_) {
            if (!first) {
                status += ",";
            }
            status += "{\"name\":\"" + name + "\",\"running\":" + 
                     (service->is_running() ? "true" : "false") + "}";
            first = false;
        }
        
        status += "]}";
        
        Message response(MessageType::SYSTEM_COMMAND, config_.manager_id, 
                        target, status);
        message_bus_->publish(response);
    }
    
    // 心跳循环
    void heartbeat_loop() {
        while (heartbeat_running_) {
            // 发送心跳消息
            Message heartbeat(MessageType::HEARTBEAT, config_.manager_id, 
                             "", "{\"status\":\"alive\"}");
            message_bus_->publish(heartbeat);
            
            // 等待一段时间
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval_ms));
        }
    }
};

// 基础服务实现类
class BaseService : public Service {
protected:
    std::string name_;
    std::shared_ptr<MessageBus> message_bus_;
    std::atomic<bool> running_;
    
    // 消息处理线程
    std::thread worker_thread_;
    
    // 消息队列
    std::mutex message_queue_mutex_;
    std::condition_variable message_queue_cv_;
    std::queue<Message> message_queue_;

public:
    BaseService(const std::string& name, std::shared_ptr<MessageBus> message_bus)
        : name_(name), message_bus_(message_bus), running_(false) {
    }
    
    virtual ~BaseService() {
        stop();
    }
    
    // 实现Service接口
    
    std::string name() const override {
        return name_;
    }
    
    bool start() override {
        if (running_) {
            return true;
        }
        
        // 订阅相关消息
        subscribe_to_messages();
        
        running_ = true;
        
        // 启动工作线程
        worker_thread_ = std::thread([this]() {
            worker_loop();
        });
        
        return true;
    }
    
    bool stop() override {
        if (!running_) {
            return true;
        }
        
        running_ = false;
        
        // 唤醒工作线程
        {
            std::lock_guard<std::mutex> lock(message_queue_mutex_);
            message_queue_cv_.notify_all();
        }
        
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        
        // 取消订阅
        unsubscribe_from_messages();
        
        return true;
    }
    
    bool is_running() const override {
        return running_;
    }
    
    void handle_message(const Message& message) override {
        if (!running_) {
            return;
        }
        
        // 将消息添加到队列
        {
            std::lock_guard<std::mutex> lock(message_queue_mutex_);
            message_queue_.push(message);
        }
        
        // 通知工作线程
        message_queue_cv_.notify_one();
    }
    
protected:
    // 订阅相关消息，子类可重写
    virtual void subscribe_to_messages() {
        // 默认订阅所有类型的消息
        for (int i = 0; i < static_cast<int>(MessageType::LOG) + 1; ++i) {
            MessageType type = static_cast<MessageType>(i);
            message_bus_->subscribe(type, [this](const Message& message) {
                this->handle_message(message);
            });
        }
    }
    
    // 取消订阅，子类可重写
    virtual void unsubscribe_from_messages() {
        // 取消订阅所有类型的消息
        for (int i = 0; i < static_cast<int>(MessageType::LOG) + 1; ++i) {
            MessageType type = static_cast<MessageType>(i);
            message_bus_->unsubscribe(type);
        }
    }
    
    // 处理消息的具体逻辑，子类必须重写
    virtual void process_message(const Message& message) = 0;
    
private:
    // 工作线程循环
    void worker_loop() {
        while (running_) {
            Message message;
            bool has_message = false;
            
            // 获取下一个消息
            {
                std::unique_lock<std::mutex> lock(message_queue_mutex_);
                
                if (message_queue_.empty()) {
                    // 等待新消息或停止信号
                    message_queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                        [this]() { return !message_queue_.empty() || !running_; });
                    
                    if (!running_) {
                        break;
                    }
                    
                    if (message_queue_.empty()) {
                        continue;
                    }
                }
                
                message = message_queue_.front();
                message_queue_.pop();
                has_message = true;
            }
            
            // 处理消息
            if (has_message) {
                try {
                    process_message(message);
                } catch (const std::exception& e) {
                    std::cerr << "处理消息错误[" << name_ << "]: " << e.what() << std::endl;
                }
            }
        }
    }
};

// 市场数据服务
class MarketDataService : public BaseService {
private:
    std::map<std::string, std::shared_ptr<OrderBook>> order_books_;
    std::mutex order_books_mutex_;
    
    // 数据源配置
    struct DataSourceConfig {
        std::string type;  // "fix", "websocket", "file", etc.
        std::string endpoint;
        std::map<std::string, std::string> parameters;
    };
    
    std::vector<DataSourceConfig> data_sources_;

public:
    MarketDataService(std::shared_ptr<MessageBus> message_bus)
        : BaseService("MarketDataService", message_bus) {
    }
    
    // 添加数据源
    void add_data_source(const DataSourceConfig& config) {
        data_sources_.push_back(config);
    }
    
    // 获取订单簿
    std::shared_ptr<OrderBook> get_order_book(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(order_books_mutex_);
        return order_books_[symbol];
    }
    
protected:
    // 只订阅相关消息类型
    void subscribe_to_messages() override {
        message_bus_->subscribe(MessageType::MARKET_DATA, [this](const Message& message) {
            this->handle_message(message);
        });
        
        message_bus_->subscribe(MessageType::SYSTEM_COMMAND, [this](const Message& message) {
            this->handle_message(message);
        });
    }
    
    // 处理消息
    void process_message(const Message& message) override {
        if (message.type == MessageType::MARKET_DATA) {
            // 处理市场数据更新
            process_market_data(message);
        } else if (message.type == MessageType::SYSTEM_COMMAND) {
            // 处理系统命令
            process_system_command(message);
        }
    }
    
private:
    // 处理市场数据
    void process_market_data(const Message& message) {
        // 解析JSON数据
        // 在实际系统中使用JSON库解析
        // 这里简化处理
        
        size_t symbol_pos = message.payload.find("\"symbol\":\"");
        if (symbol_pos == std::string::npos) {
            return;
        }
        
        symbol_pos += 10;  // 跳过 "symbol":"
        size_t symbol_end = message.payload.find("\"", symbol_pos);
        if (symbol_end == std::string::npos) {
            return;
        }
        
        std::string symbol = message.payload.substr(symbol_pos, symbol_end - symbol_pos);
        
        // 确保订单簿存在
        std::shared_ptr<OrderBook> order_book;
        {
            std::lock_guard<std::mutex> lock(order_books_mutex_);
            auto it = order_books_.find(symbol);
            if (it == order_books_.end()) {
                order_books_[symbol] = std::make_shared<OrderBook>(symbol);
            }
            order_book = order_books_[symbol];
        }
        
        // 根据消息内容更新订单簿
        // 这里简化处理，实际系统需要完整解析JSON数据
        
        // 示例：处理限价单更新
        if (message.payload.find("\"type\":\"limit\"") != std::string::npos) {
            // 解析价格和数量
            double price = 0.0;
            int quantity = 0;
            OrderSide side = OrderSide::BUY;
            
            size_t price_pos = message.payload.find("\"price\":");
            if (price_pos != std::string::npos) {
                price_pos += 8;  // 跳过 "price":
                price = std::stod(message.payload.substr(price_pos));
            }
            
            size_t quantity_pos = message.payload.find("\"quantity\":");
            if (quantity_pos != std::string::npos) {
                quantity_pos += 11;  // 跳过 "quantity":
                quantity = std::stoi(message.payload.substr(quantity_pos));
            }
            
            size_t side_pos = message.payload.find("\"side\":\"");
            if (side_pos != std::string::npos) {
                side_pos += 8;  // 跳过 "side":"
                size_t side_end = message.payload.find("\"", side_pos);
                if (side_end != std::string::npos) {
                    std::string side_str = message.payload.substr(side_pos, side_end - side_pos);
                    if (side_str == "buy") {
                        side = OrderSide::BUY;
                    } else if (side_str == "sell") {
                        side = OrderSide::SELL;
                    }
                }
            }
            
            // 创建订单并添加到订单簿
            auto order = std::make_shared<Order>(
                "md_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()),
                symbol,
                side,
                OrderType::LIMIT,
                PriceUtils::toInternalPrice(price),
                quantity
            );
            
            order_book->addOrder(order);
        }
    }
    
    // 处理系统命令
    void process_system_command(const Message& message) {
        // 处理特定于市场数据服务的命令
        if (message.payload.find("\"command\":\"subscribe\"") != std::string::npos) {
            // 订阅特定交易品种的市场数据
            size_t symbol_pos = message.payload.find("\"symbol\":\"");
            if (symbol_pos != std::string::npos) {
                symbol_pos += 10;  // 跳过 "symbol":"
                size_t symbol_end = message.payload.find("\"", symbol_pos);
                if (symbol_end != std::string::npos) {
                    std::string symbol = message.payload.substr(symbol_pos, symbol_end - symbol_pos);
                    
                    // 实际系统中，这里会连接到数据源并订阅
                    // 简化示例中，只创建空的订单簿
                    std::lock_guard<std::mutex> lock(order_books_mutex_);
                    if (order_books_.find(symbol) == order_books_.end()) {
                        order_books_[symbol] = std::make_shared<OrderBook>(symbol);
                    }
                }
            }
        }
    }
};

// 订单管理服务
class OrderManagerService : public BaseService, public OrderBookListener {
private:
    std::shared_ptr<MarketDataService> market_data_service_;
    std::map<std::string, std::shared_ptr<Order>> active_orders_;
    std::mutex orders_mutex_;

public:
    OrderManagerService(std::shared_ptr<MessageBus> message_bus,
                       std::shared_ptr<MarketDataService> market_data_service)
        : BaseService("OrderManagerService", message_bus),
          market_data_service_(market_data_service) {
    }
    
    // 实现OrderBookListener接口
    void onBookUpdate(const BookUpdateEvent& event) override {
        // 根据订单簿更新处理订单
        if (event.type == BookUpdateType::TRADE && event.trade) {
            // 当有成交时，向策略发送成交信息
            send_trade_notification(event.trade);
        }
    }
    
protected:
    // 订阅相关消息
    void subscribe_to_messages() override {
        message_bus_->subscribe(MessageType::ORDER, [this](const Message& message) {
            this->handle_message(message);
        });
    }
    
    // 处理消息
    void process_message(const Message& message) override {
        if (message.type == MessageType::ORDER) {
            // 处理订单消息
            process_order_message(message);
        }
    }
    
private:
    // 处理订单消息
    void process_order_message(const Message& message) {
        // 解析JSON数据
        // 在实际系统中使用JSON库解析
        // 这里简化处理
        
        if (message.payload.find("\"action\":\"new\"") != std::string::npos) {
            // 新订单
            create_order(message);
        } else if (message.payload.find("\"action\":\"cancel\"") != std::string::npos) {
            // 取消订单
            cancel_order(message);
        } else if (message.payload.find("\"action\":\"modify\"") != std::string::npos) {
            // 修改订单
            modify_order(message);
        }
    }
    
    // 创建新订单
    void create_order(const Message& message) {
        // 解析订单详情
        std::string symbol, client_order_id;
        OrderSide side = OrderSide::BUY;
        OrderType type = OrderType::LIMIT;
        double price = 0.0;
        int quantity = 0;
        
        // 解析 symbol
        size_t pos = message.payload.find("\"symbol\":\"");
        if (pos != std::string::npos) {
            pos += 10;
            size_t end = message.payload.find("\"", pos);
            if (end != std::string::npos) {
                symbol = message.payload.substr(pos, end - pos);
            }
        }
        
        // 解析 client_order_id
        pos = message.payload.find("\"client_order_id\":\"");
        if (pos != std::string::npos) {
            pos += 19;
            size_t end = message.payload.find("\"", pos);
            if (end != std::string::npos) {
                client_order_id = message.payload.substr(pos, end - pos);
            }
        }
        
        // 解析 side
        pos = message.payload.find("\"side\":\"");
        if (pos != std::string::npos) {
            pos += 8;
            size_t end = message.payload.find("\"", pos);
            if (end != std::string::npos) {
                std::string side_str = message.payload.substr(pos, end - pos);
                if (side_str == "buy") {
                    side = OrderSide::BUY;
                } else if (side_str == "sell") {
                    side = OrderSide::SELL;
                }
            }
        }
        
        // 解析 type
        pos = message.payload.find("\"type\":\"");
        if (pos != std::string::npos) {
            pos += 8;
            size_t end = message.payload.find("\"", pos);
            if (end != std::string::npos) {
                std::string type_str = message.payload.substr(pos, end - pos);
                if (type_str == "market") {
                    type = OrderType::MARKET;
                } else if (type_str == "limit") {
                    type = OrderType::LIMIT;
                } else if (type_str == "ioc") {
                    type = OrderType::IOC;
                }
                // 可添加其他类型
            }
        }
        
        // 解析 price
        pos = message.payload.find("\"price\":");
        if (pos != std::string::npos) {
            pos += 8;
            price = std::stod(message.payload.substr(pos));
        }
        
        // 解析 quantity
        pos = message.payload.find("\"quantity\":");
        if (pos != std::string::npos) {
            pos += 11;
            quantity = std::stoi(message.payload.substr(pos));
        }
        
        // 创建订单
        auto order = std::make_shared<Order>(
            client_order_id.empty() ? 
                std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) : 
                client_order_id,
            symbol,
            side,
            type,
            PriceUtils::toInternalPrice(price),
            quantity
        );
        
        // 将订单添加到活跃订单列表
        {
            std::lock_guard<std::mutex> lock(orders_mutex_);
            active_orders_[order->id] = order;
        }
        
        // 获取对应的订单簿
        auto order_book = market_data_service_->get_order_book(symbol);
        if (order_book) {
            // 注册为订单簿监听器
            order_book->addListener(
                std::static_pointer_cast<OrderBookListener>(
                    std::shared_ptr<OrderManagerService>(this)
                )
            );
            
            // 添加订单到订单簿
            order_book->addOrder(order);
        }
        
        // 发送订单确认
        send_order_confirmation(order);
    }
    
    // 取消订单
    void cancel_order(const Message& message) {
        std::string order_id;
        
        // 解析 order_id
        size_t pos = message.payload.find("\"order_id\":\"");
        if (pos != std::string::npos) {
            pos += 12;
            size_t end = message.payload.find("\"", pos);
            if (end != std::string::npos) {
                order_id = message.payload.substr(pos, end - pos);
            }
        }
        
        if (order_id.empty()) {
            return;
        }
        
        // 查找订单
        std::shared_ptr<Order> order;
        {
            std::lock_guard<std::mutex> lock(orders_mutex_);
            auto it = active_orders_.find(order_id);
            if (it == active_orders_.end()) {
                return;
            }
            order = it->second;
        }
        
        // 获取订单簿
        auto order_book = market_data_service_->get_order_book(order->symbol);
        if (order_book) {
            // 从订单簿中取消订单
            order_book->cancelOrder(order_id);
        }
        
        // 从活跃订单中移除
        {
            std::lock_guard<std::mutex> lock(orders_mutex_);
            active_orders_.erase(order_id);
        }
        
        // 发送取消确认
        send_cancel_confirmation(order_id);
    }
    
    // 修改订单
    void modify_order(const Message& message) {
        std::string order_id;
        double new_price = 0.0;
        int new_quantity = 0;
        
        // 解析 order_id
        size_t pos = message.payload.find("\"order_id\":\"");
        if (pos != std::string::npos) {
            pos += 12;
            size_t end = message.payload.find("\"", pos);
            if (end != std::string::npos) {
                order_id = message.payload.substr(pos, end - pos);
            }
        }
        
        // 解析 price
        pos = message.payload.find("\"price\":");
        if (pos != std::string::npos) {
            pos += 8;
            new_price = std::stod(message.payload.substr(pos));
        }
        
        // 解析 quantity
        pos =