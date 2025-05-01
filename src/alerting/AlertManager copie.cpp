#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <random>
#include <sstream>
#include <iomanip>

namespace hls_to_dvb {

Alert::Alert(AlertLevel lvl, const std::string& comp, const std::string& msg, bool persist)
    : level(lvl), message(msg), component(comp), 
      timestamp(std::chrono::system_clock::now()), persistent(persist), id("") {
}

AlertManager& AlertManager::getInstance() {
    static AlertManager instance;
    return instance;
}

AlertManager::AlertManager() {
}

AlertManager::~AlertManager() {
}

std::string AlertManager::addAlert(AlertLevel level, const std::string& component, 
                               const std::string& message, bool persistent) {
    Alert alert(level, component, message, persistent);
    
    std::string alertId;
    if (persistent) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<uint32_t> dis;
        
        std::stringstream ss;
        ss << std::hex << dis(gen);
        alertId = ss.str();
        alert.id = alertId;
    }
    
    {
        std::lock_guard<std::mutex> lock(alertsMutex_);
        alerts_.push_back(alert);
        
        if (persistent) {
            persistentAlerts_[alertId] = alert;
        }
    }
    
    // Log l'alerte
    std::string logMsg = component + ": " + message;
    
    switch (level) {
        case AlertLevel::INFO:
            spdlog::info(logMsg);
            break;
        case AlertLevel::WARNING:
            spdlog::warn(logMsg);
            break;
        case AlertLevel::ERROR:
            spdlog::error(logMsg);
            break;
    }
    
    return alertId;
}

bool AlertManager::resolveAlert(const std::string& alertId) {
    std::lock_guard<std::mutex> lock(alertsMutex_);
    
    auto it = persistentAlerts_.find(alertId);
    if (it != persistentAlerts_.end()) {
        spdlog::info("Alert resolved: {} - {}", it->second.component, it->second.message);
        persistentAlerts_.erase(it);
        return true;
    }
    
    return false;
}

} // namespace hls_to_dvb
