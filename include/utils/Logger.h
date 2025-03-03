// Logger.h - 日志系统头文件
#pragma once

#include <string>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <iomanip>

namespace hft {

// 日志级别
enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

// 日志级别转换为字符串
inline std::string log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:   return "TRACE";
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::FATAL:   return "FATAL";
        default:                return "UNKNOWN";
    }
}

// 日志输出接口
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(LogLevel level, const std::string& logger_name, 
                      const std::string& message) = 0;
    virtual void flush() = 0;
};

// 控制台日志输出
class ConsoleSink : public LogSink {
public:
    void write(LogLevel level, const std::string& logger_name, 
              const std::string& message) override;
    void flush() override;
};

// 文件日志输出
class FileSink : public LogSink {
private:
    std::string filename_;
    std::ofstream file_;
    std::mutex mutex_;
    size_t max_file_size_bytes_;
    size_t current_file_size_;
    int rotation_count_;
    int max_files_;

public:
    FileSink(const std::string& filename, 
           size_t max_file_size_mb = 10, 
           int max_files = 5);
    ~FileSink();

    void write(LogLevel level, const std::string& logger_name, 
              const std::string& message) override;
    void flush() override;

private:
    void check_file_size();
    void rotate_log_file();
};

// 日志记录器类
class Logger {
private:
    std::string name_;
    LogLevel level_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    std::mutex mutex_;

public:
    Logger(const std::string& name, LogLevel level = LogLevel::INFO);
    ~Logger() = default;

    // 添加日志输出接口
    void add_sink(std::shared_ptr<LogSink> sink);

    // 设置日志级别
    void set_level(LogLevel level);

    // 获取日志级别
    LogLevel get_level() const;

    // 日志记录方法
    void log(LogLevel level, const std::string& message);

    // 便捷日志方法
    void trace(const std::string& message);
    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void fatal(const std::string& message);

    // 格式化日志方法模板
    template<typename... Args>
    void log_format(LogLevel level, const std::string& format, Args&&... args);

    template<typename... Args>
    void trace_format(const std::string& format, Args&&... args);

    template<typename... Args>
    void debug_format(const std::string& format, Args&&... args);

    template<typename... Args>
    void info_format(const std::string& format, Args&&... args);

    template<typename... Args>
    void warning_format(const std::string& format, Args&&... args);

    template<typename... Args>
    void error_format(const std::string& format, Args&&... args);

    template<typename... Args>
    void fatal_format(const std::string& format, Args&&... args);
};

// 日志管理器单例
class LoggerManager {
private:
    std::unordered_map<std::string, std::shared_ptr<Logger>> loggers_;
    std::mutex mutex_;
    
    LogLevel default_level_;
    std::vector<std::shared_ptr<LogSink>> default_sinks_;
    
    // 单例实例
    static LoggerManager* instance_;
    
    // 私有构造函数
    LoggerManager();

public:
    // 禁止拷贝和赋值
    LoggerManager(const LoggerManager&) = delete;
    LoggerManager& operator=(const LoggerManager&) = delete;
    
    // 获取单例实例
    static LoggerManager& instance();
    
    // 获取日志记录器
    std::shared_ptr<Logger> get_logger(const std::string& name);
    
    // 设置默认日志级别
    void set_default_level(LogLevel level);
    
    // 添加默认日志输出接口
    void add_default_sink(std::shared_ptr<LogSink> sink);
    
    // 重置所有日志记录器
    void reset();
    
    // 初始化日志系统（从配置文件）
    bool initialize_from_config(const std::string& config_file);
};

// 获取默认日志记录器
inline std::shared_ptr<Logger> get_logger(const std::string& name) {
    return LoggerManager::instance().get_logger(name);
}

// 格式化字符串辅助函数
template<typename... Args>
std::string format_string(const std::string& format, Args&&... args) {
    // 简化实现，实际应使用更高效的格式化库如fmt
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), format.c_str(), std::forward<Args>(args)...);
    return std::string(buffer);
}

// Logger 模板方法实现
template<typename... Args>
void Logger::log_format(LogLevel level, const std::string& format, Args&&... args) {
    if (level < level_) return;
    log(level, format_string(format, std::forward<Args>(args)...));
}

template<typename... Args>
void Logger::trace_format(const std::string& format, Args&&... args) {
    log_format(LogLevel::TRACE, format, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::debug_format(const std::string& format, Args&&... args) {
    log_format(LogLevel::DEBUG, format, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::info_format(const std::string& format, Args&&... args) {
    log_format(LogLevel::INFO, format, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::warning_format(const std::string& format, Args&&... args) {
    log_format(LogLevel::WARNING, format, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::error_format(const std::string& format, Args&&... args) {
    log_format(LogLevel::ERROR, format, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::fatal_format(const std::string& format, Args&&... args) {
    log_format(LogLevel::FATAL, format, std::forward<Args>(args)...);
}

} // namespace hft


// Logger.cpp - 日志系统实现
#include "Logger.h"
#include <iostream>
#include <ctime>
#include <filesystem>
#include <yaml-cpp/yaml.h>

namespace hft {

// 获取当前时间格式化字符串
std::string get_current_time_str() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// ConsoleSink 实现
void ConsoleSink::write(LogLevel level, const std::string& logger_name, 
                       const std::string& message) {
    std::string time_str = get_current_time_str();
    std::string level_str = log_level_to_string(level);
    
    // 根据级别设置不同颜色
    std::string color_code;
    switch (level) {
        case LogLevel::TRACE:   color_code = "\033[90m"; break; // 灰色
        case LogLevel::DEBUG:   color_code = "\033[36m"; break; // 青色
        case LogLevel::INFO:    color_code = "\033[32m"; break; // 绿色
        case LogLevel::WARNING: color_code = "\033[33m"; break; // 黄色
        case LogLevel::ERROR:   color_code = "\033[31m"; break; // 红色
        case LogLevel::FATAL:   color_code = "\033[35m"; break; // 紫色
        default:                color_code = "\033[0m";  break; // 默认
    }
    
    std::string reset_code = "\033[0m";
    
    // 格式化输出
    std::cout << color_code << "[" << time_str << "] [" << level_str 
              << "] [" << logger_name << "] " << message << reset_code << std::endl;
}

void ConsoleSink::flush() {
    std::cout.flush();
}

// FileSink 实现
FileSink::FileSink(const std::string& filename, 
                 size_t max_file_size_mb, 
                 int max_files)
    : filename_(filename), 
      max_file_size_bytes_(max_file_size_mb * 1024 * 1024),
      current_file_size_(0),
      rotation_count_(0),
      max_files_(max_files) {
    
    // 创建目录（如果不存在）
    auto path = std::filesystem::path(filename_).parent_path();
    if (!path.empty() && !std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
    }
    
    // 打开日志文件
    file_.open(filename_, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "Failed to open log file: " << filename_ << std::endl;
    } else {
        // 获取当前文件大小
        file_.seekp(0, std::ios::end);
        current_file_size_ = static_cast<size_t>(file_.tellp());
        file_.seekp(0, std::ios::end);
    }
}

FileSink::~FileSink() {
    if (file_.is_open()) {
        file_.close();
    }
}

void FileSink::write(LogLevel level, const std::string& logger_name, 
                    const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    check_file_size();
    
    if (!file_.is_open()) {
        return;
    }
    
    std::string time_str = get_current_time_str();
    std::string level_str = log_level_to_string(level);
    
    // 格式化输出
    std::string log_line = "[" + time_str + "] [" + level_str
                         + "] [" + logger_name + "] " + message + "\n";
    
    file_ << log_line;
    current_file_size_ += log_line.size();
}

void FileSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

void FileSink::check_file_size() {
    if (!file_.is_open()) {
        return;
    }
    
    if (current_file_size_ >= max_file_size_bytes_) {
        rotate_log_file();
    }
}

void FileSink::rotate_log_file() {
    if (file_.is_open()) {
        file_.close();
    }
    
    // 重命名现有日志文件
    for (int i = max_files_ - 1; i > 0; --i) {
        std::string old_name = filename_ + "." + std::to_string(i - 1);
        std::string new_name = filename_ + "." + std::to_string(i);
        
        if (std::filesystem::exists(old_name)) {
            if (std::filesystem::exists(new_name)) {
                std::filesystem::remove(new_name);
            }
            std::filesystem::rename(old_name, new_name);
        }
    }
    
    std::string backup_name = filename_ + ".0";
    if (std::filesystem::exists(filename_)) {
        if (std::filesystem::exists(backup_name)) {
            std::filesystem::remove(backup_name);
        }
        std::filesystem::rename(filename_, backup_name);
    }
    
    // 重新打开日志文件
    file_.open(filename_, std::ios::app);
    current_file_size_ = 0;
    
    if (!file_.is_open()) {
        std::cerr << "Failed to open log file after rotation: " << filename_ << std::endl;
    }
    
    rotation_count_++;
}

// Logger 实现
Logger::Logger(const std::string& name, LogLevel level)
    : name_(name), level_(level) {
}

void Logger::add_sink(std::shared_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(sink);
}

void Logger::set_level(LogLevel level) {
    level_ = level;
}

LogLevel Logger::get_level() const {
    return level_;
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < level_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        sink->write(level, name_, message);
    }
}

void Logger::trace(const std::string& message) {
    log(LogLevel::TRACE, message);
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void Logger::fatal(const std::string& message) {
    log(LogLevel::FATAL, message);
}

// LoggerManager 实现
LoggerManager* LoggerManager::instance_ = nullptr;

LoggerManager::LoggerManager()
    : default_level_(LogLevel::INFO) {
    // 默认添加控制台输出
    default_sinks_.push_back(std::make_shared<ConsoleSink>());
}

LoggerManager& LoggerManager::instance() {
    if (instance_ == nullptr) {
        instance_ = new LoggerManager();
    }
    return *instance_;
}

std::shared_ptr<Logger> LoggerManager::get_logger(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = loggers_.find(name);
    if (it != loggers_.end()) {
        return it->second;
    }
    
    // 创建新的日志记录器
    auto logger = std::make_shared<Logger>(name, default_level_);
    
    // 添加默认输出接口
    for (auto& sink : default_sinks_) {
        logger->add_sink(sink);
    }
    
    loggers_[name] = logger;
    return logger;
}

void LoggerManager::set_default_level(LogLevel level) {
    default_level_ = level;
    
    // 更新现有日志记录器的级别
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, logger] : loggers_) {
        logger->set_level(level);
    }
}

void LoggerManager::add_default_sink(std::shared_ptr<LogSink> sink) {
    default_sinks_.push_back(sink);
    
    // 将新的输出接口添加到现有日志记录器
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, logger] : loggers_) {
        logger->add_sink(sink);
    }
}

void LoggerManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    loggers_.clear();
    default_sinks_.clear();
    default_level_ = LogLevel::INFO;
    
    // 重新添加默认控制台输出
    default_sinks_.push_back(std::make_shared<ConsoleSink>());
}

bool LoggerManager::initialize_from_config(const std::string& config_file) {
    try {
        YAML::Node config = YAML::LoadFile(config_file);
        
        // 解析默认日志级别
        if (config["logging"]["level"]) {
            std::string level_str = config["logging"]["level"].as<std::string>();
            LogLevel level = LogLevel::INFO;
            
            if (level_str == "trace") level = LogLevel::TRACE;
            else if (level_str == "debug") level = LogLevel::DEBUG;
            else if (level_str == "info") level = LogLevel::INFO;
            else if (level_str == "warning") level = LogLevel::WARNING;
            else if (level_str == "error") level = LogLevel::ERROR;
            else if (level_str == "fatal") level = LogLevel::FATAL;
            
            set_default_level(level);
        }
        
        // 解析控制台输出配置
        if (config["logging"]["console"]) {
            bool enable_console = config["logging"]["console"]["enable"].as<bool>(true);
            if (enable_console) {
                // 已经有默认控制台，无需再添加
            } else {
                // 移除默认控制台
                default_sinks_.clear();
            }
        }
        
        // 解析文件输出配置
        if (config["logging"]["file"]) {
            bool enable_file = config["logging"]["file"]["enable"].as<bool>(false);
            if (enable_file) {
                std::string filename = config["logging"]["file"]["filename"].as<std::string>("logs/app.log");
                size_t max_size_mb = config["logging"]["file"]["max_size_mb"].as<size_t>(10);
                int max_files = config["logging"]["file"]["max_files"].as<int>(5);
                
                auto file_sink = std::make_shared<FileSink>(filename, max_size_mb, max_files);
                add_default_sink(file_sink);
            }
        }
        
        // 解析特定日志记录器配置
        if (config["logging"]["loggers"] && config["logging"]["loggers"].IsSequence()) {
            for (const auto& logger_config : config["logging"]["loggers"]) {
                std::string name = logger_config["name"].as<std::string>();
                std::string level_str = logger_config["level"].as<std::string>();
                
                LogLevel level = LogLevel::INFO;
                if (level_str == "trace") level = LogLevel::TRACE;
                else if (level_str == "debug") level = LogLevel::DEBUG;
                else if (level_str == "info") level = LogLevel::INFO;
                else if (level_str == "warning") level = LogLevel::WARNING;
                else if (level_str == "error") level = LogLevel::ERROR;
                else if (level_str == "fatal") level = LogLevel::FATAL;
                
                auto logger = get_logger(name);
                logger->set_level(level);
            }
        }
        
        return true;
    } catch (const YAML::Exception& e) {
        std::cerr << "Failed to parse log configuration: " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error initializing logging: " << e.what() << std::endl;
        return false;
    }
}

} // namespace hft