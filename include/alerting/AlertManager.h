#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <chrono>

namespace hls_to_dvb {

enum class AlertLevel {
    INFO,
    WARNING,
    ERROR
};

struct Alert {
    AlertLevel level;
    std::string message;
    std::string component;
    std::chrono::system_clock::time_point timestamp;
    bool persistent;
    std::string id;
    
    Alert() = default;
    Alert(AlertLevel lvl, const std::string& comp, const std::string& msg, bool persist = false);
};

class AlertManager {
public:
    static AlertManager& getInstance();
    
    std::string addAlert(AlertLevel level, const std::string& component, 
                      const std::string& message, bool persistent = false);
    
    bool resolveAlert(const std::string& alertId);
    
private:
    AlertManager();
    ~AlertManager();
    
    AlertManager(const AlertManager&) = delete;
    AlertManager& operator=(const AlertManager&) = delete;
    
    std::mutex alertsMutex_;
    std::vector<Alert> alerts_;
    std::map<std::string, Alert> persistentAlerts_;
};

} // namespace hls_to_dvb
