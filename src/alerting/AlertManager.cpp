#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <uuid/uuid.h> // Une bibliothèque pour générer des UUID
#include <random>       // Pour mt19937 et autres générateurs aléatoires
#include <sstream>      // Pour stringstream
#include <iomanip>      // Pour setw, setfill
#include <chrono>       // Pour fonctionnalités de temps
#include <algorithm>    // Pour std::find_if, std::remove_if


namespace hls_to_dvb {
// Fonction pour générer un ID unique
std::string generateUniqueId() {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    auto value = now_ms.time_since_epoch().count();
    
    static std::mt19937 gen(static_cast<unsigned int>(value));
    std::uniform_int_distribution<> dis(0, 0xFFFF);
    
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << value;
    ss << "-";
    ss << std::hex << std::setfill('0') << std::setw(4) << dis(gen);
    ss << std::hex << std::setfill('0') << std::setw(4) << dis(gen);
    
    return ss.str();
}

Alert::Alert(AlertLevel lvl, const std::string& comp, const std::string& msg, bool persist)
    : level(lvl), component(comp), message(msg), timestamp(std::chrono::system_clock::now()),
      persistent(persist), id(generateUniqueId())
{
}

AlertManager& AlertManager::getInstance() {
    static AlertManager instance;
    return instance;
}

AlertManager::AlertManager() {
    // Valeurs par défaut pour la rétention
    retention_[AlertLevel::INFO] = 7200;      // 2 heures
    retention_[AlertLevel::WARNING] = 86400;  // 24 heures
    retention_[AlertLevel::ERROR] = 604800;   // 7 jours
}

AlertManager::~AlertManager() {
    // Vider les ressources
}

std::string AlertManager::addAlert(AlertLevel level, const std::string& component, 
                               const std::string& message, bool persistent) {
    std::lock_guard<std::mutex> lock(alertsMutex_);
    
    // Créer une nouvelle alerte
    Alert alert(level, component, message, persistent);
    
    // Nettoyer les alertes expirées
    cleanupExpiredAlerts();
    
    // Ajouter l'alerte à la liste appropriée
    if (persistent) {
        persistentAlerts_[alert.id] = alert;
    } else {
        alerts_.push_back(alert);
    }
    
    // Journaliser l'alerte
    std::string levelStr;
    switch (level) {
        case AlertLevel::INFO:
            levelStr = "INFO";
            spdlog::info("[{}] {}: {}", component, levelStr, message);
            break;
        case AlertLevel::WARNING:
            levelStr = "WARNING";
            spdlog::warn("[{}] {}: {}", component, levelStr, message);
            break;
        case AlertLevel::ERROR:
            levelStr = "ERROR";
            spdlog::error("[{}] {}: {}", component, levelStr, message);
            break;
    }
    
    // Appeler les fonctions de rappel enregistrées
    for (const auto& callback : callbacks_) {
        callback.second(alert);
    }
    
    return alert.id;
}

bool AlertManager::resolveAlert(const std::string& alertId) {
    std::lock_guard<std::mutex> lock(alertsMutex_);
    
    // Chercher dans les alertes persistantes
    auto it = persistentAlerts_.find(alertId);
    if (it != persistentAlerts_.end()) {
        persistentAlerts_.erase(it);
        return true;
    }
    
    // Chercher dans les alertes normales
    auto alertIt = std::find_if(alerts_.begin(), alerts_.end(),
                               [&alertId](const Alert& a) { return a.id == alertId; });
    
    if (alertIt != alerts_.end()) {
        alerts_.erase(alertIt);
        return true;
    }
    
    return false;
}

void AlertManager::setRetention(AlertLevel level, int seconds) {
    std::lock_guard<std::mutex> lock(alertsMutex_);
    retention_[level] = seconds;
    
    // Nettoyer les alertes qui pourraient déjà être expirées avec la nouvelle rétention
    cleanupExpiredAlerts();
}

std::vector<Alert> AlertManager::getActiveAlerts() const {
    std::lock_guard<std::mutex> lock(alertsMutex_);
    
    // Combiner les alertes normales et persistantes dans un seul vecteur
    std::vector<Alert> activeAlerts = alerts_;
    
    // Ajouter les alertes persistantes
    for (const auto& pair : persistentAlerts_) {
        activeAlerts.push_back(pair.second);
    }
    
    // Trier par horodatage, plus récent en premier
    std::sort(activeAlerts.begin(), activeAlerts.end(),
             [](const Alert& a, const Alert& b) {
                 return a.timestamp > b.timestamp;
             });
    
    return activeAlerts;
}

int AlertManager::registerCallback(std::function<void(const Alert&)> callback) {
    std::lock_guard<std::mutex> lock(alertsMutex_);
    int id = nextCallbackId_++;
    callbacks_[id] = callback;
    return id;
}

bool AlertManager::unregisterCallback(int callbackId) {
    std::lock_guard<std::mutex> lock(alertsMutex_);
    auto it = callbacks_.find(callbackId);
    if (it != callbacks_.end()) {
        callbacks_.erase(it);
        return true;
    }
    return false;
}

void AlertManager::cleanupExpiredAlerts() {
    auto now = std::chrono::system_clock::now();
    
    // Supprimer les alertes non persistantes expirées
    alerts_.erase(
        std::remove_if(alerts_.begin(), alerts_.end(),
                      [this, now](const Alert& alert) {
                          auto retention = retention_[alert.level];
                          auto expirationTime = alert.timestamp + std::chrono::seconds(retention);
                          return !alert.persistent && expirationTime < now;
                      }),
        alerts_.end()
    );
}

} // namespace hls_to_dvb