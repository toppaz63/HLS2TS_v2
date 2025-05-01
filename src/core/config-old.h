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
     * @return Map de niveaux d'alerte vers durées de rétention en secondes
     */
    std::map<std::string, int> getAlertRetention() const;
    
    /**
     * @brief Récupère la configuration de journalisation
     * @return Configuration de journalisation en JSON
     */
    nlohmann::json getLoggingConfig() const;
    
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
};

} // namespace hls_to_dvb