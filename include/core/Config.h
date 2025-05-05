#pragma once

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

namespace hls_to_dvb {

/**
 * @brief Structure représentant la configuration d'un flux HLS vers MPEG-TS
 */
struct StreamConfig {
    std::string id;               ///< Identifiant unique du flux
    std::string name;             ///< Nom lisible du flux
    std::string hlsInput;         ///< URL d'entrée du flux HLS
    std::string mcastOutput;      ///< Adresse IP multicast de sortie
    int mcastPort;                ///< Port multicast de sortie
    std::string mcastInterface;   ///< Interface réseau pour la sortie multicast
    size_t bufferSize;            ///< Taille du buffer en nombre de segments
    bool enabled;                 ///< Si le flux est activé
    
    StreamConfig() : mcastPort(1234), bufferSize(3), enabled(true) {}
};

/**
 * @brief Configuration du serveur web pour l'interface utilisateur
 */
struct ServerConfig {
    std::string address;          ///< Adresse d'écoute du serveur
    int port;                     ///< Port d'écoute du serveur
    int workerThreads;            ///< Nombre de threads de travail
    
    ServerConfig() : address("0.0.0.0"), port(8080), workerThreads(4) {}
};

/**
 * @brief Configuration du système de journalisation
 */
struct LoggingConfig {
    std::string level;                ///< Niveau de log minimum (debug, info, warning, error)
    bool console;                     ///< Afficher les logs sur la console
    
    struct {
        bool enabled;                 ///< Journalisation dans un fichier
        std::string path;             ///< Chemin du fichier de log
        size_t rotationSize;          ///< Taille en octets avant rotation
        int maxFiles;                 ///< Nombre maximum de fichiers de log
    } file;
    
    LoggingConfig() : level("info"), console(true) {
        file.enabled = true;
        file.path = "logs/hls-to-dvb.log";
        file.rotationSize = 10 * 1024 * 1024; // 10 MB
        file.maxFiles = 5;
    }
};

/**
 * @brief Configuration du système d'alertes
 */
struct AlertsConfig {
    struct Retention {
        int info;                     ///< Durée de conservation des alertes INFO en secondes
        int warning;                  ///< Durée de conservation des alertes WARNING en secondes
        int error;                    ///< Durée de conservation des alertes ERROR en secondes
    } retention;
    
    struct {
        struct {
            bool enabled;             ///< Notifications par email activées
            std::string server;       ///< Serveur SMTP
            int port;                 ///< Port SMTP
            std::string username;     ///< Nom d'utilisateur SMTP
            std::string password;     ///< Mot de passe SMTP
            std::vector<std::string> recipients; ///< Destinataires des emails
            std::string minLevel;     ///< Niveau minimum pour envoyer un email
        } email;
        
        struct {
            bool enabled;             ///< Notifications par webhook activées
            std::string url;          ///< URL du webhook
            std::string minLevel;     ///< Niveau minimum pour déclencher le webhook
        } webhook;
    } notifications;
    
    AlertsConfig() {
        retention.info = 7200;      // 2 heures
        retention.warning = 86400;  // 24 heures
        retention.error = 604800;   // 7 jours
        
        notifications.email.enabled = false;
        notifications.email.server = "smtp.example.com";
        notifications.email.port = 587;
        notifications.email.minLevel = "error";
        
        notifications.webhook.enabled = false;
        notifications.webhook.url = "https://example.com/webhook";
        notifications.webhook.minLevel = "warning";
    }
};

/**
 * @brief Classe gérant la configuration globale de l'application
 */
class Config {
public:
    /**
     * @brief Constructeur
     * @param configPath Chemin vers le fichier de configuration
     */
    explicit Config(const std::string& configPath);
    
    /**
     * @brief Charge la configuration depuis un fichier JSON
     * @return true si la configuration a été chargée avec succès, false sinon
     */
    bool load();
    
    /**
     * @brief Charge la configuration depuis un fichier JSON
     * @param configPath Chemin vers le fichier de configuration
     * @return true si la configuration a été chargée avec succès, false sinon
     */
    bool loadFromFile(const std::string& configPath);
    
    /**
     * @brief Charge la configuration depuis une chaîne JSON
     * @param jsonString Chaîne JSON contenant la configuration
     * @return true si la configuration a été chargée avec succès, false sinon
     */
    bool loadFromString(const std::string& jsonString);
    
    /**
     * @brief Sauvegarde la configuration dans un fichier
     * @param configPath Chemin du fichier où sauvegarder la configuration
     * @return true si la configuration a été sauvegardée avec succès, false sinon
     */
    bool saveToFile(const std::string& configPath) const;
    
    /**
     * @brief Récupère la configuration pour un flux spécifique
     * @param streamId Identifiant du flux
     * @return Configuration du flux ou nullptr si non trouvé
     */
    const StreamConfig* getStreamConfig(const std::string& streamId) const;
    
    /**
     * @brief Récupère toutes les configurations de flux
     * @return Vecteur de configurations de flux
     */
    const std::vector<StreamConfig>& getStreamConfigs() const;
    
    /**
     * @brief Ajoute ou met à jour la configuration d'un flux
     * @param config Configuration du flux à ajouter/mettre à jour
     * @return true si l'opération a réussi, false sinon
     */
    bool updateStreamConfig(const StreamConfig& config);
    
    /**
     * @brief Supprime la configuration d'un flux
     * @param streamId Identifiant du flux à supprimer
     * @return true si le flux a été supprimé, false s'il n'existait pas
     */
    bool removeStreamConfig(const std::string& streamId);
    
    /**
     * @brief Récupère la configuration du serveur
     * @return Configuration du serveur
     */
    const ServerConfig& getServerConfig() const;
    
    /**
     * @brief Récupère la configuration de journalisation
     * @return Configuration de journalisation
     */
    const LoggingConfig& getLoggingConfig() const;
    
    /**
     * @brief Récupère la configuration des alertes
     * @return Configuration des alertes
     */
    const AlertsConfig& getAlertsConfig() const;
    
    /**
     * @brief Récupère la configuration de rétention des alertes
     * @return Configuration de rétention des alertes
     */
    const AlertsConfig::Retention& getAlertRetention() const;
    
    /**
     * @brief Met à jour la configuration du serveur
     * @param config Nouvelle configuration du serveur
     */
    void updateServerConfig(const ServerConfig& config);
    
    /**
     * @brief Met à jour la configuration de journalisation
     * @param config Nouvelle configuration de journalisation
     */
    void updateLoggingConfig(const LoggingConfig& config);
    
    /**
     * @brief Met à jour la configuration des alertes
     * @param config Nouvelle configuration des alertes
     */
    void updateAlertsConfig(const AlertsConfig& config);
    
    /**
     * @brief Convertit la configuration complète en JSON
     * @return JSON représentant la configuration
     */
    nlohmann::json toJson() const;
    
    /**
     * @brief Récupère le chemin du fichier de configuration
     * @return Chemin du fichier de configuration
     */
    std::string getConfigPath() const;

    /**
    * @brief Journalise la configuration complète pour vérification
    */
    void logConfiguration() const;
    
private:
    std::string configPath_;
    std::vector<StreamConfig> streams_;
    ServerConfig server_;
    LoggingConfig logging_;
    AlertsConfig alerts_;
    
    // Maps pour accès rapide par ID
    std::map<std::string, size_t> streamIndexMap_;
};

} // namespace hls_to_dvb
