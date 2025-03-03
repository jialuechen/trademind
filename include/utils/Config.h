// Config.h - 配置管理工具头文件
#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <any>
#include <typeindex>
#include <functional>
#include <yaml-cpp/yaml.h>

namespace hft {

// 配置值类
class ConfigValue {
private:
    std::any value_;
    std::type_index type_idx_;
    
    // 类型名称字符串
    std::string type_name_;
    
    // 描述
    std::string description_;
    
    // 是否可修改
    bool read_only_;
    
    // 验证函数 (返回true表示验证通过)
    std::function<bool(const std::any&)> validator_;
    
    // 发生变更时的回调函数
    std::vector<std::function<void(const std::any&)>> change_callbacks_;
    
public:
    template<typename T>
    ConfigValue(const T& value, 
               const std::string& description = "", 
               bool read_only = false,
               std::function<bool(const T&)> validator = nullptr)
        : value_(value), 
          type_idx_(typeid(T)), 
          type_name_(typeid(T).name()),
          description_(description),
          read_only_(read_only) {
        
        // 包装验证函数以处理std::any
        if (validator) {
            validator_ = [validator](const std::any& value) -> bool {
                try {
                    const T& typed_value = std::any_cast<T>(value);
                    return validator(typed_value);
                } catch (const std::bad_any_cast&) {
                    return false;
                }
            };
        }
    }
    
    // 获取类型ID
    std::type_index type_index() const {
        return type_idx_;
    }
    
    // 获取类型名称
    const std::string& type_name() const {
        return type_name_;
    }
    
    // 获取描述
    const std::string& description() const {
        return description_;
    }
    
    // 判断是否只读
    bool is_read_only() const {
        return read_only_;
    }
    
    // 获取值
    template<typename T>
    T get() const {
        return std::any_cast<T>(value_);
    }
    
    // 设置值
    template<typename T>
    bool set(const T& value) {
        // 检查类型是否匹配
        if (typeid(T) != type_idx_) {
            return false;
        }
        
        // 检查是否只读
        if (read_only_) {
            return false;
        }
        
        // 验证新值
        if (validator_) {
            if (!validator_(value)) {
                return false;
            }
        }
        
        // 设置新值
        value_ = value;
        
        // 调用变更回调
        for (const auto& callback : change_callbacks_) {
            callback(value_);
        }
        
        return true;
    }
    
    // 添加变更回调
    template<typename T>
    void add_change_callback(std::function<void(const T&)> callback) {
        if (typeid(T) != type_idx_) {
            return;
        }
        
        // 包装回调函数以处理std::any
        auto wrapper = [callback](const std::any& value) {
            try {
                const T& typed_value = std::any_cast<T>(value);
                callback(typed_value);
            } catch (const std::bad_any_cast&) {
                // 类型不匹配，忽略
            }
        };
        
        change_callbacks_.push_back(wrapper);
    }
};

// 配置管理器类
class ConfigManager {
private:
    std::map<std::string, std::shared_ptr<ConfigValue>> configs_;
    std::mutex mutex_;
    
    // 单例实例
    static ConfigManager* instance_;
    
    // 私有构造函数
    ConfigManager() {}
    
public:
    // 禁止拷贝和赋值
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    
    // 获取单例实例
    static ConfigManager& instance() {
        if (instance_ == nullptr) {
            instance_ = new ConfigManager();
        }
        return *instance_;
    }
    
    // 注册配置项
    template<typename T>
    bool register_config(const std::string& key, 
                        const T& default_value, 
                        const std::string& description = "",
                        bool read_only = false,
                        std::function<bool(const T&)> validator = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (configs_.find(key) != configs_.end()) {
            return false;  // 配置项已存在
        }
        
        configs_[key] = std::make_shared<ConfigValue>(
            default_value, description, read_only, validator);
        
        return true;
    }
    
    // 获取配置值
    template<typename T>
    T get(const std::string& key, const T& default_value = T()) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = configs_.find(key);
        if (it == configs_.end()) {
            // 配置项不存在，自动注册
            configs_[key] = std::make_shared<ConfigValue>(default_value);
            return default_value;
        }
        
        try {
            return it->second->get<T>();
        } catch (const std::bad_any_cast&) {
            // 类型不匹配
            return default_value;
        }
    }
    
    // 设置配置值
    template<typename T>
    bool set(const std::string& key, const T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = configs_.find(key);
        if (it == configs_.end()) {
            // 配置项不存在，自动注册
            configs_[key] = std::make_shared<ConfigValue>(value);
            return true;
        }
        
        return it->second->set(value);
    }
    
    // 检查配置项是否存在
    bool exists(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return configs_.find(key) != configs_.end();
    }
    
    // 添加配置变更回调
    template<typename T>
    bool add_change_callback(const std::string& key, std::function<void(const T&)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = configs_.find(key);
        if (it == configs_.end()) {
            return false;  // 配置项不存在
        }
        
        it->second->add_change_callback<T>(callback);
        return true;
    }
    
    // 从YAML文件加载配置
    bool load_from_yaml(const std::string& file_path) {
        try {
            YAML::Node config = YAML::LoadFile(file_path);
            return load_from_yaml_node(config);
        } catch (const YAML::Exception& e) {
            // 处理YAML解析错误
            return false;
        }
    }
    
    // 从YAML节点加载配置
    bool load_from_yaml_node(const YAML::Node& node, const std::string& prefix = "") {
        if (!node.IsMap()) {
            return false;
        }
        
        bool success = true;
        
        for (const auto& it : node) {
            std::string key = it.first.as<std::string>();
            std::string full_key = prefix.empty() ? key : prefix + "." + key;
            
            const YAML::Node& value = it.second;
            
            if (value.IsMap()) {
                // 递归处理子节点
                success = load_from_yaml_node(value, full_key) && success;
            } else {
                // 设置配置值
                try {
                    if (value.IsScalar()) {
                        // 尝试不同类型的转换
                        if (exists(full_key)) {
                            // 根据已存在的配置项类型进行转换
                            auto config_type = configs_[full_key]->type_name();
                            
                            if (config_type == typeid(std::string).name()) {
                                set(full_key, value.as<std::string>());
                            } else if (config_type == typeid(int).name()) {
                                set(full_key, value.as<int>());
                            } else if (config_type == typeid(double).name()) {
                                set(full_key, value.as<double>());
                            } else if (config_type == typeid(bool).name()) {
                                set(full_key, value.as<bool>());
                            } else {
                                // 不支持的类型
                                success = false;
                            }
                        } else {
                            // 配置项不存在，根据YAML值类型自动推断
                            if (value.IsNull()) {
                                set(full_key, std::string(""));
                            } else if (value.IsSequence()) {
                                // 暂不支持序列
                                success = false;
                            } else {
                                // 尝试转换为字符串
                                set(full_key, value.as<std::string>());
                            }
                        }
                    } else if (value.IsSequence()) {
                        // 暂不支持序列
                        success = false;
                    }
                } catch (const YAML::Exception&) {
                    success = false;
                }
            }
        }
        
        return success;
    }
    
    // 保存配置到YAML文件
    bool save_to_yaml(const std::string& file_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        try {
            YAML::Node root;
            
            for (const auto& [key, value] : configs_) {
                std::vector<std::string> parts;
                
                // 分割键名
                size_t start = 0;
                size_t pos;
                while ((pos = key.find('.', start)) != std::string::npos) {
                    parts.push_back(key.substr(start, pos - start));
                    start = pos + 1;
                }
                parts.push_back(key.substr(start));
                
                // 构建YAML节点路径
                YAML::Node* current = &root;
                for (size_t i = 0; i < parts.size() - 1; ++i) {
                    if (!(*current)[parts[i]]) {
                        (*current)[parts[i]] = YAML::Node(YAML::NodeType::Map);
                    }
                    current = &(*current)[parts[i]];
                }
                
                // 设置值
                std::string type_name = value->type_name();
                if (type_name == typeid(std::string).name()) {
                    (*current)[parts.back()] = value->get<std::string>();
                } else if (type_name == typeid(int).name()) {
                    (*current)[parts.back()] = value->get<int>();
                } else if (type_name == typeid(double).name()) {
                    (*current)[parts.back()] = value->get<double>();
                } else if (type_name == typeid(bool).name()) {
                    (*current)[parts.back()] = value->get<bool>();
                }
                // 其他类型暂不支持
            }
            
            // 写入文件
            std::ofstream fout(file_path);
            if (!fout) {
                return false;
            }
            
            fout << YAML::Dump(root);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    // 获取所有配置项
    std::vector<std::string> get_all_keys() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<std::string> keys;
        keys.reserve(configs_.size());
        
        for (const auto& [key, _] : configs_) {
            keys.push_back(key);
        }
        
        return keys;
    }
};

// 初始化单例指针
ConfigManager* ConfigManager::instance_ = nullptr;

// 便捷函数：获取配置
template<typename T>
T get_config(const std::string& key, const T& default_value = T()) {
    return ConfigManager::instance().get<T>(key, default_value);
}

// 便捷函数：设置配置
template<typename T>
bool set_config(const std::string& key, const T& value) {
    return ConfigManager::instance().set<T>(key, value);
}

// 便捷函数：注册配置
template<typename T>
bool register_config(const std::string& key, 
                    const T& default_value, 
                    const std::string& description = "",
                    bool read_only = false,
                    std::function<bool(const T&)> validator = nullptr) {
    return ConfigManager::instance().register_config<T>(key, default_value, description, read_only, validator);
}

} // namespace hft