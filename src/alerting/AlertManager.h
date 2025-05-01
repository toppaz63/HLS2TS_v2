#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <deque>
#include <functional>

namespace hls_to_dvb {
// Niveaux d'alertes
enum class AlertLevel {
    INFO,      // Information
    WARNING,   // Avertissement
    ERROR      // Erreur
};

// Structure représentant une alerte
struct Alert {
    AlertLevel level;             // Niveau de l'alerte
    std::string source;           // Source de l'alerte (composant)
    std::string message;          // Message détaillé
    bool persistent;              // Indique si l'alerte doit être conservée
    int64_t timestamp;            // Horodatage de l'alerte (millisecondes depuis l'époque)

    Alert(AlertLevel level, std::string source, std::string message, bool persistent);
};

// Type de fonction de rappel pour les nouvelles alertes
using AlertCallback = std::function<void(const Alert&)>;

/**
 * @class AlertManager
 * @brief Gestionnaire des alertes de l'application
 * 
 * Singleton qui centralise la gestion des alertes de tous les composants.
 * Permet d'ajouter, récupérer et filtrer les alertes, ainsi que de s'abonner
 * aux nouvelles alertes.
 */
class AlertManager {
public:
    /**
     * @brief Récupère l'instance unique du gestionnaire d'alertes
     * @return Référence à l'instance unique
     */
    static AlertManager& getInstance();

    /**
     * @brief Ajoute une nouvelle alerte
     * @param level Niveau de l'alerte
     * @param source Source de l'alerte (nom du composant)
     * @param message Message détaillé
     * @param persistent Indique si l'alerte doit être conservée
     * @return Identifiant de l'alerte ajoutée
     */
    int addAlert(AlertLevel level, const std::string& source, const std::string& message, bool persistent);

    /**
     * @brief Supprime une alerte par son identifiant
     * @param alertId Identifiant de l'alerte à supprimer
     * @return true si l'alerte a été supprimée, false sinon
     */
    bool removeAlert(int alertId);

    /**
     * @brief Récupère toutes les alertes
     * @return Vecteur contenant toutes les alertes
     */
    std::vector<Alert> getAllAlerts() const;

    /**
     * @brief Récupère les alertes filtrées par niveau
     * @param level Niveau d'alerte à filtrer
     * @return Vecteur contenant les alertes filtrées
     */
    std::vector<Alert> getAlertsByLevel(AlertLevel level) const;

    /**
     * @brief Récupère les alertes filtrées par source
     * @param source Source à filtrer
     * @return Vecteur contenant les alertes filtrées
     */
    std::vector<Alert> getAlertsBySource(const std::string& source) const;

    /**
     * @brief Abonne une fonction de rappel pour être notifiée des nouvelles alertes
     * @param callback Fonction à appeler lors d'une nouvelle alerte
     * @return Identifiant de l'abonnement
     */
    int subscribeToAlerts(AlertCallback callback);

    /**
     * @brief Désabonne une fonction de rappel
     * @param subscriptionId Identifiant de l'abonnement à supprimer
     * @return true si l'abonnement a été supprimé, false sinon
     */
    bool unsubscribeFromAlerts(int subscriptionId);

    /**
     * @brief Définit le nombre maximum d'alertes à conserver
     * @param maxAlerts Nombre maximum d'alertes (0 pour pas de limite)
     */
    void setMaxAlerts(size_t maxAlerts);

    /**
     * @brief Définit la durée de rétention pour chaque niveau d'alerte
     * @param level Niveau d'alerte concerné
     * @param seconds Durée de rétention en secondes (0 pour conserver indéfiniment)
     */
    void setRetention(AlertLevel level, int seconds);

    /**
     * @brief Supprime toutes les alertes
     */
    void clearAlerts();

private:
    // Constructeur privé (singleton)
    AlertManager();
    
    // Interdire la copie et l'assignation
    AlertManager(const AlertManager&) = delete;
    AlertManager& operator=(const AlertManager&) = delete;

    // Supprime les alertes expirées selon les règles de rétention
    void pruneExpiredAlerts();

    // Données membres
    mutable std::mutex mutex_;                   // Mutex pour l'accès concurrent
    std::deque<Alert> alerts_;                   // Liste des alertes
    std::vector<std::pair<int, AlertCallback>> callbacks_; // Liste des abonnements
    size_t maxAlerts_;                           // Nombre maximum d'alertes
    int retentionInfo_;                          // Durée de rétention des alertes INFO (secondes)
    int retentionWarning_;                       // Durée de rétention des alertes WARNING (secondes)
    int retentionError_;                         // Durée de rétention des alertes ERROR (secondes)
    int nextAlertId_;                            // Prochain identifiant d'alerte
    int nextSubscriptionId_;                     // Prochain identifiant d'abonnement
};

} // namespace hls_to_dvb