#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "nlohmann/json.hpp"

namespace hls_to_dvb {

/**
 * @struct StreamConfig
 * @brief Configuration d'un flux de streaming
 */
struct StreamConfig {
    std::string id;              ///< Identifiant unique du flux
    std::string name;            ///< Nom convivial du flux
    std::string hlsInput;        ///< URL du flux HLS d'entrée
    std::string multicastOutput; ///< Adresse IP multicast de sortie
    int multicastPort;           ///< Port multicast de sortie
    size_t bufferSize;           ///< Taille du buffer (nombre de chunks)
    
    // Constructeur avec valeurs par défaut
    StreamConfig() : multicastPort(1234), bufferSize(3) {}
};

/**
 * @struct LoggingFileConfig
 * @brief Configuration de journalisation dans un fichier
 */
struct LoggingFileConfig {
    bool enabled;             ///< Activation de la journalisation dans un fichier
    std::string path;         ///< Chemin du fichier de log
    size_t rotationSize;      ///< Taille du fichier avant rotation
    size_t maxFiles;          ///< Nombre maximum de fichiers de rotation

    // Constructeur avec valeurs par défaut
    LoggingFileConfig() : enabled(true), path("logs/hls-to-dvb.log"), rotationSize(10485760), maxFiles(5) {}
};

/**
 * @struct LoggingConfig
 * @brief Configuration globale de journalisation
 */
struct LoggingConfig {
    std::string level;        ///< Niveau de log (debug, info, warning, error)
    bool console;             ///< Activation de la journalisation dans la console
    LoggingFileConfig file;   ///< Configuration de journalisation dans un fichier

    // Constructeur avec valeurs par défaut
    LoggingConfig() : level("info"), console(true) {}
};

/**
 * @struct ServerConfig
 * @brief Configuration du serveur
 */
struct ServerConfig {
    int port;                 ///< Port d'écoute du serveur
    std::string address;      ///< Adresse d'écoute du serveur
    int workerThreads;        ///< Nombre de threads de travail

    // Constructeur avec valeurs par défaut
    ServerConfig() : port(8080), address("0.0.0.0"), workerThreads(4) {}
};

/**
 * @struct AlertRetentionConfig
 * @brief Configuration de rétention des alertes
 */
struct AlertRetentionConfig {
    int info;                 ///< Durée de rétention des alertes de niveau info (en secondes)
    int warning;              ///< Durée de rétention des alertes de niveau warning (en secondes)
    int error;                ///< Durée de rétention des alertes de niveau error (en secondes)

    // Constructeur avec valeurs par défaut
    AlertRetentionConfig() : info(7200), warning(86400), error(604800) {}
};

/**
 * @class Config
 * @brief Gestion de la configuration de l'application
 * 
 * Charge et maintient la configuration depuis un fichier JSON.
 * Thread-safe pour les accès concurrents.
 */
class Config {
public:
    /**
     * @brief Constructeur
     * @param configPath Chemin vers le fichier de configuration JSON
     */
    explicit Config(const std::string& configPath);
    
    /**
     * @brief Charge la configuration depuis le fichier
     * @return true si le chargement a réussi
     */
    bool load();
    
    /**
     * @brief Sauvegarde la configuration dans le fichier
     * @return true si la sauvegarde a réussi
     */
    bool save();
    
    /**
     * @brief Récupère la liste des configurations de flux
     * @return Vecteur de configurations de flux
     */
    std::vector<StreamConfig> getStreams() const;
    
    /**
     * @brief Récupère une configuration de flux par son ID
     * @param id Identifiant du flux
     * @return Configuration du flux ou nullptr si non trouvé
     */
    const StreamConfig* getStream(const std::string& id) const;
    
    /**
     * @brief Ajoute ou met à jour la configuration d'un flux
     * @param stream Configuration du flux
     * @return true si l'opération a réussi
     */
    bool setStream(const StreamConfig& stream);
    
    /**
     * @brief Supprime un flux de la configuration
     * @param id Identifiant du flux à supprimer
     * @return true si le flux a été supprimé avec succès
     */
    bool removeStream(const std::string& id);
    
    /**
     * @brief Récupère le port du serveur web
     * @return Numéro de port
     */
    int getWebServerPort() const;
    
    /**
     * @brief Récupère le niveau de log
     * @return Niveau de log (debug, info, warning, error)
     */
    std::string getLogLevel() const;
    
    /**
     * @brief Récupère la configuration complète du serveur
     * @return Configuration du serveur
     */
    ServerConfig getServerConfig() const;
    
    /**
     * @brief Récupère l'adresse du serveur web
     * @return Adresse IP
     */
    std::string getServerAddress() const;
    
    /**
     * @brief Récupère le nombre de threads de travail du serveur
     * @return Nombre de threads
     */
    int getServerWorkerThreads() const;
    
    /**
     * @brief Récupère la configuration de rétention des alertes
     * @return Configuration de rétention des alertes
     */
    AlertRetentionConfig getAlertRetention() const;
    
    /**
     * @brief Récupère la configuration de journalisation
     * @return Configuration de journalisation
     */
    LoggingConfig getLoggingConfig() const;
    
private:
    std::string configPath_;                 ///< Chemin du fichier de configuration
    std::vector<StreamConfig> streams_;      ///< Liste des flux configurés
    std::map<std::string, std::string> settings_; ///< Paramètres généraux
    mutable std::mutex mutex_;               ///< Mutex pour les accès concurrents
    
    /**
     * @brief Génère un identifiant unique pour un flux
     * @return Identifiant unique au format hexadécimal
     */
    std::string generateUniqueId() const;
    
    /**
     * @brief Convertit un objet JSON en structure LoggingConfig
     * @param json Objet JSON contenant la configuration de journalisation
     * @return Structure LoggingConfig
     */
    LoggingConfig parseLoggingConfig(const nlohmann::json& json) const;
    
    /**
     * @brief Convertit un objet JSON en structure ServerConfig
     * @param json Objet JSON contenant la configuration du serveur
     * @return Structure ServerConfig
     */
    ServerConfig parseServerConfig(const nlohmann::json& json) const;
    
    /**
     * @brief Convertit un objet JSON en structure AlertRetentionConfig
     * @param json Objet JSON contenant la configuration de rétention des alertes
     * @return Structure AlertRetentionConfig
     */
    AlertRetentionConfig parseAlertRetentionConfig(const nlohmann::json& json) const;
};

} // namespace hls_to_dvb
