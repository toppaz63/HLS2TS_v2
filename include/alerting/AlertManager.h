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
    
    /**
     * @brief Définit la durée de rétention pour un niveau d'alerte donné
     * @param level Niveau d'alerte
     * @param seconds Durée de rétention en secondes
     */
    void setRetention(AlertLevel level, int seconds);
    
    /**
     * @brief Récupère toutes les alertes actives
     * @return Vecteur d'alertes actives
     */
    std::vector<Alert> getActiveAlerts() const;
    
    /**
     * @brief Enregistre une fonction de rappel pour les nouvelles alertes
     * @param callback Fonction à appeler lors de l'ajout d'une alerte
     * @return ID de l'enregistrement (pour désenregistrement ultérieur)
     */
    int registerCallback(std::function<void(const Alert&)> callback);
    
    /**
     * @brief Supprime une fonction de rappel enregistrée
     * @param callbackId ID de la fonction de rappel à supprimer
     * @return true si la fonction a été désenregistrée, false sinon
     */
    bool unregisterCallback(int callbackId);
    
private:
    AlertManager();
    ~AlertManager();
    
    AlertManager(const AlertManager&) = delete;
    AlertManager& operator=(const AlertManager&) = delete;
    
    mutable std::mutex alertsMutex_;
    std::vector<Alert> alerts_;
    std::map<std::string, Alert> persistentAlerts_;
    
    std::map<AlertLevel, int> retention_; // Durée de rétention par niveau (en secondes)
    
    std::map<int, std::function<void(const Alert&)>> callbacks_;
    int nextCallbackId_ = 0;
    
    /**
     * @brief Nettoie les alertes expirées
     */
    void cleanupExpiredAlerts();
};

} // namespace hls_to_dvb
