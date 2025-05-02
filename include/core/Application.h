#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <chrono>

namespace hls_to_dvb {

// Forward declarations
class Config;
class StreamManager;
class WebServer;
}

namespace hls_to_dvb {
/**
 * @brief Classe principale de l'application qui coordonne tous les composants
 */
class Application {
public:
    /**
     * @brief Constructeur
     * @param configPath Chemin vers le fichier de configuration
     */
    Application(const std::string& configPath);
    
    /**
     * @brief Destructeur
     */
    ~Application();
    
    /**
     * @brief Initialise l'application
     * @return true si l'initialisation a réussi, false sinon
     */
    bool initialize();
    
    /**
     * @brief Démarre l'application et ses composants
     * @return true si le démarrage a réussi, false sinon
     */
    bool start();
    
    /**
     * @brief Arrête l'application et ses composants
     */
    void stop();
    
    /**
     * @brief Lance la boucle principale de l'application
     * Cette méthode est bloquante jusqu'à ce que stop() soit appelé
     */
    void run();
    
    /**
     * @brief Vérifie si l'application est en cours d'exécution
     * @return true si l'application est en cours d'exécution, false sinon
     */
    bool isRunning() const;
    
    /**
     * @brief Récupère le temps d'exécution de l'application
     * @return Temps d'exécution en secondes
     */
    uint64_t getUptime() const;
    
    /**
     * @brief Récupère la configuration
     * @return Référence vers la configuration
     */
    Config& getConfig();
    
    /**
     * @brief Récupère le gestionnaire de flux
     * @return Référence vers le gestionnaire de flux
     */
    StreamManager& getStreamManager();
    
private:
    std::string configPath_;
    std::unique_ptr<Config> config_;
    std::unique_ptr<StreamManager> streamManager_;
    std::unique_ptr<WebServer> webServer_;
    
    std::atomic<bool> running_;
    std::chrono::system_clock::time_point startTime_;
    
    // Méthodes privées
    bool initializeLogging();
    bool initializeAlertManager();
};

} // namespace hls_to_dvb