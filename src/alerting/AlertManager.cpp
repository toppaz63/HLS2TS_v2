#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>

namespace hls_to_dvb {

// Constructeur de Alert
Alert::Alert(AlertLevel level, std::string source, std::string message, bool persistent)
    : level(level), source(std::move(source)), message(std::move(message)), 
      persistent(persistent), timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()) {
}

// Singleton pattern - Instance unique
AlertManager& AlertManager::getInstance() {
    static AlertManager instance;
    return instance;
}

// Constructeur privé
AlertManager::AlertManager()
    : maxAlerts_(100), retentionInfo_(7200), retentionWarning_(86400), 
      retentionError_(604800), nextAlertId_(1), nextSubscriptionId_(1) {
}

// Méthode pour ajouter une alerte
int AlertManager::addAlert(AlertLevel level, const std::string& source, 
                          const std::string& message, bool persistent) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Générer un ID pour l'alerte
    int alertId = nextAlertId_++;
    
    // Créer l'alerte
    Alert alert(level, source, message, persistent);
    
    // Ajouter l'alerte à la liste
    alerts_.push_back(alert);
    
    // Limiter le nombre d'alertes si nécessaire
    if (maxAlerts_ > 0 && alerts_.size() > maxAlerts_) {
        // Supprimer les alertes non persistantes les plus anciennes
        auto it = std::find_if(alerts_.begin(), alerts_.end(), 
                              [](const Alert& a) { return !a.persistent; });
        if (it != alerts_.end()) {
            alerts_.erase(it);
        }
    }
    
    // Supprimer les alertes expirées
    pruneExpiredAlerts();
    
    // Notifier les abonnés
    for (const auto& callback : callbacks_) {
        callback.second(alert);
    }
    
    return alertId;
}

// Méthode pour supprimer une alerte
bool AlertManager::removeAlert(int alertId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Rechercher l'alerte avec l'ID spécifié
    auto it = std::find_if(alerts_.begin(), alerts_.end(), 
                          [alertId](const Alert& a) { 
                              // Ici, nous devrions comparer avec un ID que nous n'avons pas dans la struct Alert
                              // Pour l'instant, nous allons simplement comparer l'index
                              return false; // Remplacer par la vraie condition quand l'ID est ajouté à Alert
                          });
    
    if (it != alerts_.end()) {
        alerts_.erase(it);
        return true;
    }
    
    return false;
}

// Méthode pour récupérer toutes les alertes
std::vector<Alert> AlertManager::getAllAlerts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Alert>(alerts_.begin(), alerts_.end());
}

// Méthode pour récupérer les alertes par niveau
std::vector<Alert> AlertManager::getAlertsByLevel(AlertLevel level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Alert> result;
    for (const auto& alert : alerts_) {
        if (alert.level == level) {
            result.push_back(alert);
        }
    }
    
    return result;
}

// Méthode pour récupérer les alertes par source
std::vector<Alert> AlertManager::getAlertsBySource(const std::string& source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Alert> result;
    for (const auto& alert : alerts_) {
        if (alert.source == source) {
            result.push_back(alert);
        }
    }
    
    return result;
}

// Méthode pour s'abonner aux alertes
int AlertManager::subscribeToAlerts(AlertCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int subscriptionId = nextSubscriptionId_++;
    callbacks_.emplace_back(subscriptionId, std::move(callback));
    
    return subscriptionId;
}

// Méthode pour se désabonner
bool AlertManager::unsubscribeFromAlerts(int subscriptionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::find_if(callbacks_.begin(), callbacks_.end(),
                          [subscriptionId](const std::pair<int, AlertCallback>& pair) {
                              return pair.first == subscriptionId;
                          });
    
    if (it != callbacks_.end()) {
        callbacks_.erase(it);
        return true;
    }
    
    return false;
}

// Méthode pour définir le nombre maximum d'alertes
void AlertManager::setMaxAlerts(size_t maxAlerts) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxAlerts_ = maxAlerts;
    
    // Supprimer les alertes en excès si nécessaire
    if (maxAlerts_ > 0 && alerts_.size() > maxAlerts_) {
        // Garder les alertes les plus récentes
        size_t toRemove = alerts_.size() - maxAlerts_;
        for (size_t i = 0; i < toRemove; ++i) {
            // Rechercher l'alerte non persistante la plus ancienne
            auto it = std::find_if(alerts_.begin(), alerts_.end(), 
                                  [](const Alert& a) { return !a.persistent; });
            if (it != alerts_.end()) {
                alerts_.erase(it);
            } else {
                break; // Toutes les alertes restantes sont persistantes
            }
        }
    }
}

// Méthode pour définir la durée de rétention
void AlertManager::setRetention(AlertLevel level, int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    switch (level) {
        case AlertLevel::INFO:
            retentionInfo_ = seconds;
            break;
        case AlertLevel::WARNING:
            retentionWarning_ = seconds;
            break;
        case AlertLevel::ERROR:
            retentionError_ = seconds;
            break;
    }
    
    // Supprimer les alertes expirées
    pruneExpiredAlerts();
}

// Méthode pour supprimer toutes les alertes
void AlertManager::clearAlerts() {
    std::lock_guard<std::mutex> lock(mutex_);
    alerts_.clear();
}

// Méthode privée pour supprimer les alertes expirées
void AlertManager::pruneExpiredAlerts() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Supprimer les alertes expirées non persistantes
    alerts_.erase(
        std::remove_if(alerts_.begin(), alerts_.end(),
                      [this, now](const Alert& alert) {
                          if (alert.persistent) return false;
                          
                          int retention;
                          switch (alert.level) {
                              case AlertLevel::INFO:
                                  retention = retentionInfo_;
                                  break;
                              case AlertLevel::WARNING:
                                  retention = retentionWarning_;
                                  break;
                              case AlertLevel::ERROR:
                                  retention = retentionError_;
                                  break;
                              default:
                                  retention = 0;
                          }
                          
                          // Si la rétention est 0, l'alerte ne expire jamais
                          if (retention == 0) return false;
                          
                          // Vérifier si l'alerte a expiré
                          return (now - alert.timestamp) > retention * 1000LL;
                      }),
        alerts_.end()
    );
}

} // namespace hls_to_dvb
