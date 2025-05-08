#pragma once

#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include "core/config.h"
#include "core/StreamManager.h"

// Forward declarations pour éviter les inclusions circulaires
namespace httplib {
    struct Server;
    struct Request;
    struct Response;
}

namespace hls_to_dvb {

/**
 * @class WebServer
 * @brief Serveur web pour l'interface utilisateur
 */
class WebServer {
public:
    /**
     * @brief Constructeur
     * @param config Configuration de l'application
     * @param streamManager Gestionnaire de flux
     * @param webRoot Répertoire racine des fichiers web
     */
    WebServer(Config& config, StreamManager& streamManager, const std::string& webRoot = "web");
    
    /**
     * @brief Destructeur
     */
    ~WebServer();
    
    /**
     * @brief Démarre le serveur web
     * @return true si le démarrage a réussi
     */
    bool start();
    
    /**
     * @brief Arrête le serveur web
     */
    void stop();
    
    /**
     * @brief Vérifie si le serveur est en cours d'exécution
     * @return true si le serveur est en cours d'exécution
     */
    bool isRunning() const;
    
private:
    /**
     * @brief Configure les routes du serveur
     */
    void setupRoutes();
    
    /**
     * @brief Gestionnaire pour la route GET /api/status
     */
    void handleGetStatus(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route GET /api/streams
     */
    void handleGetStreams(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route POST /api/streams
     */
    void handleCreateStream(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route GET /api/streams/:id
     */
    void handleGetStream(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route PUT /api/streams/:id
     */
    void handleUpdateStream(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route DELETE /api/streams/:id
     */
    void handleDeleteStream(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route POST /api/streams/:id/start
     */
    void handleStartStream(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route POST /api/streams/:id/stop
     */
    void handleStopStream(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route GET /api/stats
     */
    void handleGetStats(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route GET /api/alerts
     */
    void handleGetAlerts(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route POST /api/alerts/:id/resolve
     */
    void handleResolveAlert(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Gestionnaire pour la route GET /api/alerts/export
     */
    void handleExportAlerts(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief Génère un ID unique pour un nouveau flux
     * @param name Nom du flux
     * @return ID généré
     */
    std::string generateStreamId(const std::string& name);
    
    Config& config_;                          ///< Référence à la configuration
    StreamManager& streamManager_;            ///< Référence au gestionnaire de flux
    std::string webRoot_;                     ///< Répertoire racine des fichiers web
    std::unique_ptr<httplib::Server> server_; ///< Serveur HTTP
    std::thread serverThread_;                ///< Thread du serveur
    std::atomic<bool> running_;               ///< Indicateur d'exécution
};

} // namespace hls_to_dvb
