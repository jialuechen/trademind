// main.cpp - 系统启动与配置

#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <fstream>
#include <yaml-cpp/yaml.h>

#include "OrderBook.h"
#include "FixEngine.h"
#include "MessageBus.h"

using namespace hft;

// 配置加载器
class ConfigLoader {
private:
    YAML::Node config_;

public:
    ConfigLoader(const std::string& config_file) {
        try {
            config_ = YAML::LoadFile(config_file);
        } catch (const YAML::Exception& e) {
            std::cerr << "配置文件加载错误: " << e.what() << std::endl;
            throw;
        }
    }

    YAML::Node get_config() const {
        return config_;
    }

    // 获取通用配置
    std::string get_string(const std::string& key, const std::string& default_value = "") const {
        try {
            return config_[key].as<std::string>();
        } catch (...) {
            return default_value;
        }
    }

    int get_int(const std::string& key, int default_value = 0) const {
        try {
            return config_[key].as<int>();
        } catch (...) {
            return default_value;
        }
    }

    double get_double(const std::string& key, double default_value = 0.0) const {
        try {
            return config_[key].as<double>();
        } catch (...) {
            return default_value;
        }
    }

    bool get_bool(const std::string& key, bool default_value = false) const {
        try {
            return config_[key].as<bool>();
        } catch (...) {
            return default_value;
        }
    }

    // 获取交易所配置
    std::vector<FixSessionConfig> get_exchange_configs() const {
        std::vector<FixSessionConfig> configs;
        
        try {
            const YAML::Node& exchanges = config_["exchanges"];
            if (exchanges.IsSequence()) {
                for (const auto& exchange : exchanges) {
                    FixSessionConfig config;
                    config.host = exchange["host"].as<std::string>();
                    config.port = exchange["port"].as<int>();
                    config.senderCompID = exchange["sender_comp_id"].as<std::string>();
                    config.targetCompID = exchange["target_comp_id"].as<std::string>();
                    config.username = exchange["username"].as<std::string>();
                    config.password = exchange["password"].as<std::string>();
                    config.fixVersion = exchange["fix_version"].as<std::string>();
                    config.heartbeatInterval = exchange["heartbeat_interval"].as<std::string>();
                    config.resetSeqNumFlag = exchange["reset_seq_num"].as<bool>();
                    
                    configs.push_back(config);
                }
            }
        } catch (const YAML::Exception& e) {
            std::cerr << "交易所配置解析错误: " << e.what() << std::endl;
        }
        
        return configs;
    }
    
    // 获取策略配置
    std::vector<StrategyService::StrategyConfig> get_strategy_configs() const {
        std::vector<StrategyService::StrategyConfig> configs;
        
        try {
            const YAML::Node& strategies = config_["strategies"];
            if (strategies.IsSequence()) {
                for (const auto& strategy : strategies) {
                    StrategyService::StrategyConfig config;
                    config.id = strategy["id"].as<std::string>();
                    config.type = strategy["type"].as<std::string>();
                    config.symbol = strategy["symbol"].as<std::string>();
                    config.active = strategy["active"].as<bool>();
                    
                    // 解析参数
                    const YAML::Node& params = strategy["parameters"];
                    if (params.IsMap()) {
                        for (const auto& param : params) {
                            config.parameters[param.first.as<std::string>()] = 
                                param.second.as<std::string>();
                        }
                    }
                    
                    configs.push_back(config);
                }
            }
        } catch (const YAML::Exception& e) {
            std::cerr << "策略配置解析错误: " << e.what() << std::endl;
        }
        
        return configs;
    }
};

// 系统初始化和启动
int main(int argc, char* argv[]) {
    try {
        // 默认配置文件路径
        std::string config_file = "config.yaml";
        
        // 如果命令行提供了配置文件路径，则使用命令行参数
        if (argc > 1) {
            config_file = argv[1];
        }
        
        std::cout << "正在加载配置文件: " << config_file << std::endl;
        
        // 加载配置
        ConfigLoader config_loader(config_file);
        auto config = config_loader.get_config();
        
        // 创建消息总线
        std::string publisher_endpoint = config_loader.get_string("message_bus.publisher_endpoint", "tcp://*:5555");
        std::string subscriber_endpoint = config_loader.get_string("message_bus.subscriber_endpoint", "tcp://localhost:5555");
        
        auto message_bus = std::make_shared<ZeroMQMessageBus>(publisher_endpoint, subscriber_endpoint);
        
        std::cout << "初始化消息总线" << std::endl;
        if (!message_bus->start()) {
            throw std::runtime_error("无法启动消息总线");
        }
        
        // 创建FIX引擎
        auto fix_engine = std::make_shared<FixEngine>();
        
        std::cout << "初始化FIX引擎" << std::endl;
        
        // 加载交易所配置
        auto exchange_configs = config_loader.get_exchange_configs();
        for (const auto& exchange_config : exchange_configs) {
            std::string session_name = exchange_config.targetCompID;
            fix_engine->addSession(session_name, exchange_config);
            std::cout << "已添加FIX会话: " << session_name << std::endl;
        }
        
        if (!fix_engine->start()) {
            throw std::runtime_error("无法启动FIX引擎");
        }
        
        // 创建服务
        std::cout << "初始化核心服务" << std::endl;
        
        // 市场数据服务
        auto market_data_service = std::make_shared<MarketDataService>(message_bus);
        
        // 添加数据源配置
        YAML::Node data_sources = config["data_sources"];
        if (data_sources.IsSequence()) {
            for (const auto& source : data_sources) {
                MarketDataService::DataSourceConfig data_source_config;
                data_source_config.type = source["type"].as<std::string>();
                data_source_config.endpoint = source["endpoint"].as<std::string>();
                
                // 解析参数
                const YAML::Node& params = source["parameters"];
                if (params.IsMap()) {
                    for (const auto& param : params) {
                        data_source_config.parameters[param.first.as<std::string>()] = 
                            param.second.as<std::string>();
                    }
                }
                
                market_data_service->add_data_source(data_source_config);
                std::cout << "已添加数据源: " << data_source_config.type << std::endl;
            }
        }
        
        // 订单管理服务
        auto order_manager_service = std::make_shared<OrderManagerService>(message_bus, market_data_service);
        
        // 策略服务
        auto strategy_service = std::make_shared<StrategyService>(message_bus, order_manager_service);
        
        // 加载策略配置
        auto strategy_configs = config_loader.get_strategy_configs();
        for (const auto& strategy_config : strategy_configs) {
            strategy_service->add_strategy(strategy_config);
            std::cout << "已添加策略: " << strategy_config.id << std::endl;
        }
        
        // 监控服务
        auto monitoring_service = std::make_shared<MonitoringService>(message_bus);
        
        // 创建服务管理器
        std::cout << "初始化服务管理器" << std::endl;
        auto service_manager = std::make_shared<ServiceManager>(
            message_bus, 
            config_loader.get_string("service_manager.id", "service_manager"),
            config_loader.get_int("service_manager.heartbeat_interval_ms", 1000)
        );
        
        // 注册服务
        service_manager->register_service(market_data_service);
        service_manager->register_service(order_manager_service);
        service_manager->register_service(strategy_service);
        service_manager->register_service(monitoring_service);
        
        // 启动服务管理器（会自动启动所有服务）
        std::cout << "启动服务管理器" << std::endl;
        if (!service_manager->start()) {
            throw std::runtime_error("无法启动服务管理器");
        }
        
        std::cout << "系统启动完成" << std::endl;
        
        // 运行直到收到退出信号
        std::string input;
        std::cout << "输入 'q' 退出系统" << std::endl;
        while (true) {
            std::getline(std::cin, input);
            if (input == "q" || input == "quit" || input == "exit") {
                break;
            }
        }
        
        // 关闭系统
        std::cout << "正在关闭系统..." << std::endl;
        service_manager->stop();
        fix_engine->stop();
        message_bus->stop();
        
        std::cout << "系统已关闭" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "系统初始化错误: " << e.what() << std::endl;
        return 1;
    }
}