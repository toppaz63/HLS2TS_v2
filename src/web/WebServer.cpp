#include "WebServer.h"
#include "../alerting/AlertManager.h"
#include "../core/Config.h"
#include "../core/StreamManager.h"
#include "spdlog/spdlog.h"
#include "nlohmann/json.hpp"

#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <ctime>

// Définir CPPHTTPLIB_OPENSSL_SUPPORT avant d'inclure httplib.h si vous utilisez SSL
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

// Définir l'espace de noms pour JSON
using json = nlohmann::json;

namespace hls_to_dvb {

WebServer::WebServer(Config* config, StreamManager* streamManager)
    : config_(config), streamManager_(streamManager), running_(false) {
    
    // Vérifier les paramètres
    if (!config || !streamManager) {
        throw std::invalid_argument("Config et StreamManager ne peuvent pas être null");
    }
}


void WebServer::start() {
    if (running_) {
        spdlog::warn("Le serveur web est déjà en cours d'exécution");
        return;
    }
    
    try {
        // Récupérer le port du serveur web
        int port = config_->getWebServerPort();
        
        spdlog::info("Démarrage du serveur web sur le port {}", port);
        
        // Initialiser le serveur HTTP
        server_ = std::make_unique<httplib::Server>();
        
        // Configuration des routes
        configureRoutes();
        
        // Démarrer le serveur dans un thread séparé
        serverThread_ = std::thread([this, port]() {
            // Journaliser le début du serveur
            spdlog::info("Serveur web en écoute sur le port {}", port);
            
            // Démarrer le serveur
            if (!server_->listen("0.0.0.0", port)) {
                spdlog::error("Erreur lors du démarrage du serveur web sur le port {}", port);
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "WebServer",
                    "Erreur lors du démarrage du serveur web sur le port " + std::to_string(port),
                    true
                );
            }
        });
        
        running_ = true;
        
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "WebServer",
            "Serveur web démarré sur le port " + std::to_string(port),
            false
        );
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors du démarrage du serveur web: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "WebServer",
            std::string("Erreur lors du démarrage du serveur web: ") + e.what(),
            true
        );
        
        throw;
    }
}

void WebServer::stop() {
    if (!running_) {
        spdlog::warn("Le serveur web n'est pas en cours d'exécution");
        return;
    }
    
    spdlog::info("Arrêt du serveur web");
    
    // Arrêter le serveur
    if (server_) {
        server_->stop();
    }
    
    // Attendre la fin du thread
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    
    running_ = false;
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "WebServer",
        "Serveur web arrêté",
        false
    );
}

void WebServer::configureRoutes() {
    // Servir les fichiers statiques
    server_->set_mount_point("/", "./web");
    
    // Route principale - redirection vers l'index.html
    server_->Get("/", [](const httplib::Request& req, httplib::Response& res) {
        res.set_redirect("/index.html");
    });
    
    // API pour récupérer la configuration
    server_->Get("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        handleGetConfig(req, res);
    });
    
    // API pour mettre à jour la configuration
    server_->Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        handlePostConfig(req, res);
    });
    
    // API pour récupérer la liste des flux
    server_->Get("/api/streams", [this](const httplib::Request& req, httplib::Response& res) {
        handleGetStreams(req, res);
    });
    
    // API pour récupérer un flux spécifique
    server_->Get(R"(/api/streams/(\w+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto streamId = req.matches[1];
        handleGetStream(streamId, req, res);
    });
    
    // API pour créer ou mettre à jour un flux
    server_->Post("/api/streams", [this](const httplib::Request& req, httplib::Response& res) {
        handlePostStream(req, res);
    });
    
    // API pour supprimer un flux
    server_->Delete(R"(/api/streams/(\w+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto streamId = req.matches[1];
        handleDeleteStream(streamId, req, res);
    });
    
    // API pour démarrer un flux
    server_->Post(R"(/api/streams/(\w+)/start)", [this](const httplib::Request& req, httplib::Response& res) {
        auto streamId = req.matches[1];
        handleStartStream(streamId, req, res);
    });
    
    // API pour arrêter un flux
    server_->Post(R"(/api/streams/(\w+)/stop)", [this](const httplib::Request& req, httplib::Response& res) {
        auto streamId = req.matches[1];
        handleStopStream(streamId, req, res);
    });
    
    // API pour récupérer les statistiques d'un flux
    server_->Get(R"(/api/streams/(\w+)/stats)", [this](const httplib::Request& req, httplib::Response& res) {
        auto streamId = req.matches[1];
        handleGetStreamStats(streamId, req, res);
    });
    
    // API pour récupérer les alertes
    server_->Get("/api/alerts", [this](const httplib::Request& req, httplib::Response& res) {
        handleGetAlerts(req, res);
    });
    
    // API pour supprimer une alerte
    server_->Delete(R"(/api/alerts/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto alertId = std::stoi(req.matches[1]);
        handleDeleteAlert(alertId, req, res);
    });
    
    // API pour récupérer l'état du système
    server_->Get("/api/system", [this](const httplib::Request& req, httplib::Response& res) {
        handleGetSystemStatus(req, res);
    });
}

void WebServer::handleGetConfig(const httplib::Request& req, httplib::Response& res) {
    try {
        // Récupérer les paramètres de configuration
        int webServerPort = config_->getWebServerPort();
        std::string logLevel = config_->getLogLevel();
        
        // Construire la réponse JSON
        json response = {
            {"webServerPort", webServerPort},
            {"logLevel", logLevel}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la récupération de la configuration: " + std::string(e.what()));
    }
}

void WebServer::handlePostConfig(const httplib::Request& req, httplib::Response& res) {
    try {
        // Analyser le corps de la requête
        json requestBody = json::parse(req.body);
        
        // Mettre à jour les paramètres (à implémenter dans la classe Config)
        // Pour l'instant, nous utilisons une approche directe
        
        if (requestBody.contains("logLevel")) {
            std::string logLevel = requestBody["logLevel"];
            
            // Mettre à jour le niveau de log
            if (logLevel == "debug") {
                spdlog::set_level(spdlog::level::debug);
            }
            else if (logLevel == "info") {
                spdlog::set_level(spdlog::level::info);
            }
            else if (logLevel == "warning") {
                spdlog::set_level(spdlog::level::warn);
            }
            else if (logLevel == "error") {
                spdlog::set_level(spdlog::level::err);
            }
        }
        
        // Sauvegarder la configuration
        config_->save();
        
        // Construire la réponse
        json response = {
            {"success", true},
            {"message", "Configuration mise à jour avec succès"}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const json::exception& e) {
        handleError(res, 400, "JSON invalide: " + std::string(e.what()));
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la mise à jour de la configuration: " + std::string(e.what()));
    }
}

void WebServer::handleGetStreams(const httplib::Request& req, httplib::Response& res) {
    try {
        // Récupérer la liste des flux
        auto streams = config_->getStreams();
        
        // Construire la réponse JSON
        json response = json::array();
        
        for (const auto& stream : streams) {
            response.push_back({
                {"id", stream.id},
                {"name", stream.name},
                {"hlsInput", stream.hlsInput},
                {"multicastOutput", stream.multicastOutput},
                {"multicastPort", stream.multicastPort},
                {"bufferSize", stream.bufferSize},
                {"running", streamManager_->isStreamRunning(stream.id)}
            });
        }
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la récupération des flux: " + std::string(e.what()));
    }
}

void WebServer::handleGetStream(const std::string& streamId, const httplib::Request& req, httplib::Response& res) {
    try {
        // Récupérer le flux spécifié
        const StreamConfig* stream = config_->getStream(streamId);
        
        if (!stream) {
            handleError(res, 404, "Flux non trouvé: " + streamId);
            return;
        }
        
        // Construire la réponse JSON
        json response = {
            {"id", stream->id},
            {"name", stream->name},
            {"hlsInput", stream->hlsInput},
            {"multicastOutput", stream->multicastOutput},
            {"multicastPort", stream->multicastPort},
            {"bufferSize", stream->bufferSize},
            {"running", streamManager_->isStreamRunning(stream->id)}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la récupération du flux: " + std::string(e.what()));
    }
}

void WebServer::handlePostStream(const httplib::Request& req, httplib::Response& res) {
    try {
        // Analyser le corps de la requête
        json requestBody = json::parse(req.body);
        
        // Créer une nouvelle configuration de flux
        StreamConfig stream;
        
        // ID du flux (si fourni, sinon sera généré)
        if (requestBody.contains("id") && !requestBody["id"].is_null()) {
            stream.id = requestBody["id"];
        }
        
        // Nom du flux
        if (requestBody.contains("name")) {
            stream.name = requestBody["name"];
        }
        
        // URL du flux HLS d'entrée
        if (requestBody.contains("hlsInput")) {
            stream.hlsInput = requestBody["hlsInput"];
        }
        
        // Adresse IP multicast de sortie
        if (requestBody.contains("multicastOutput")) {
            stream.multicastOutput = requestBody["multicastOutput"];
        }
        
        // Port multicast de sortie
        if (requestBody.contains("multicastPort")) {
            stream.multicastPort = requestBody["multicastPort"];
        }
        
        // Taille du buffer
        if (requestBody.contains("bufferSize")) {
            stream.bufferSize = requestBody["bufferSize"];
        }
        
        // Ajouter ou mettre à jour le flux
        bool success = config_->setStream(stream);
        
        if (!success) {
            handleError(res, 500, "Erreur lors de l'enregistrement du flux");
            return;
        }
        
        // Démarrer le flux si demandé
        bool startStream = false;
        if (requestBody.contains("autoStart") && requestBody["autoStart"].is_boolean()) {
            startStream = requestBody["autoStart"];
        }
        
        if (startStream) {
            streamManager_->startStream(stream.id);
        }
        
        // Construire la réponse
        json response = {
            {"success", true},
            {"message", "Flux enregistré avec succès"},
            {"id", stream.id}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const json::exception& e) {
        handleError(res, 400, "JSON invalide: " + std::string(e.what()));
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de l'enregistrement du flux: " + std::string(e.what()));
    }
}

void WebServer::handleDeleteStream(const std::string& streamId, const httplib::Request& req, httplib::Response& res) {
    try {
        // Vérifier si le flux existe
        const StreamConfig* stream = config_->getStream(streamId);
        if (!stream) {
            handleError(res, 404, "Flux non trouvé: " + streamId);
            return;
        }
        
        // Arrêter le flux s'il est en cours d'exécution
        if (streamManager_->isStreamRunning(streamId)) {
            streamManager_->stopStream(streamId);
        }
        
        // Supprimer le flux
        bool success = config_->removeStream(streamId);
        
        if (!success) {
            handleError(res, 500, "Erreur lors de la suppression du flux");
            return;
        }
        
        // Construire la réponse
        json response = {
            {"success", true},
            {"message", "Flux supprimé avec succès"}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la suppression du flux: " + std::string(e.what()));
    }
}

void WebServer::handleStartStream(const std::string& streamId, const httplib::Request& req, httplib::Response& res) {
    try {
        // Vérifier si le flux existe
        const StreamConfig* stream = config_->getStream(streamId);
        if (!stream) {
            handleError(res, 404, "Flux non trouvé: " + streamId);
            return;
        }
        
        // Vérifier si le flux est déjà en cours d'exécution
        if (streamManager_->isStreamRunning(streamId)) {
            // Succès même si déjà démarré
            json response = {
                {"success", true},
                {"message", "Le flux est déjà en cours d'exécution"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }
        
        // Démarrer le flux
        bool success = streamManager_->startStream(streamId);
        
        if (!success) {
            handleError(res, 500, "Erreur lors du démarrage du flux");
            return;
        }
        
        // Construire la réponse
        json response = {
            {"success", true},
            {"message", "Flux démarré avec succès"}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors du démarrage du flux: " + std::string(e.what()));
    }
}

void WebServer::handleStopStream(const std::string& streamId, const httplib::Request& req, httplib::Response& res) {
    try {
        // Vérifier si le flux existe
        const StreamConfig* stream = config_->getStream(streamId);
        if (!stream) {
            handleError(res, 404, "Flux non trouvé: " + streamId);
            return;
        }
        
        // Vérifier si le flux est en cours d'exécution
        if (!streamManager_->isStreamRunning(streamId)) {
            // Succès même si déjà arrêté
            json response = {
                {"success", true},
                {"message", "Le flux n'est pas en cours d'exécution"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }
        
        // Arrêter le flux
        bool success = streamManager_->stopStream(streamId);
        
        if (!success) {
            handleError(res, 500, "Erreur lors de l'arrêt du flux");
            return;
        }
        
        // Construire la réponse
        json response = {
            {"success", true},
            {"message", "Flux arrêté avec succès"}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de l'arrêt du flux: " + std::string(e.what()));
    }
}

void WebServer::handleGetStreamStats(const std::string& streamId, const httplib::Request& req, httplib::Response& res) {
    try {
        // Vérifier si le flux existe
        const StreamConfig* stream = config_->getStream(streamId);
        if (!stream) {
            handleError(res, 404, "Flux non trouvé: " + streamId);
            return;
        }
        
        // Récupérer les statistiques du flux
        auto stats = streamManager_->getStreamStats(streamId);
        
        if (!stats) {
            handleError(res, 404, "Statistiques non disponibles pour ce flux");
            return;
        }
        
        // Construire la réponse JSON
        json response = {
            {"segmentsProcessed", stats->segmentsProcessed},
            {"discontinuitiesDetected", stats->discontinuitiesDetected},
            {"bufferSize", stats->bufferSize},
            {"bufferCapacity", stats->bufferCapacity},
            {"packetsTransmitted", stats->packetsTransmitted},
            {"currentBitrate", stats->currentBitrate},
            {"width", stats->width},
            {"height", stats->height},
            {"bandwidth", stats->bandwidth},
            {"codecs", stats->codecs},
            {"running", streamManager_->isStreamRunning(streamId)}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la récupération des statistiques: " + std::string(e.what()));
    }
}

void WebServer::handleGetAlerts(const httplib::Request& req, httplib::Response& res) {
    try {
        // Récupérer les alertes
        auto alerts = AlertManager::getInstance().getAllAlerts();
        
        // Construire la réponse JSON
        json response = json::array();
        
        for (const auto& alert : alerts) {
            // Convertir le niveau d'alerte en chaîne
            std::string levelStr;
            switch (alert.level) {
                case AlertLevel::INFO:
                    levelStr = "INFO";
                    break;
                case AlertLevel::WARNING:
                    levelStr = "WARNING";
                    break;
                case AlertLevel::ERROR:
                    levelStr = "ERROR";
                    break;
                default:
                    levelStr = "UNKNOWN";
            }
            
            // Format de l'horodatage : convertir timestamp en chaîne lisible
            auto timestamp = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(alert.timestamp)
            );
            auto timeT = std::chrono::system_clock::to_time_t(timestamp);
            char timeStr[100];
            std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", std::localtime(&timeT));
            
            response.push_back({
                {"level", levelStr},
                {"source", alert.source},
                {"message", alert.message},
                {"persistent", alert.persistent},
                {"timestamp", std::string(timeStr)}
            });
        }
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la récupération des alertes: " + std::string(e.what()));
    }
}

void WebServer::handleDeleteAlert(int alertId, const httplib::Request& req, httplib::Response& res) {
    try {
        // Supprimer l'alerte
        bool success = AlertManager::getInstance().removeAlert(alertId);
        
        if (!success) {
            handleError(res, 404, "Alerte non trouvée");
            return;
        }
        
        // Construire la réponse
        json response = {
            {"success", true},
            {"message", "Alerte supprimée avec succès"}
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la suppression de l'alerte: " + std::string(e.what()));
    }
}

void WebServer::handleGetSystemStatus(const httplib::Request& req, httplib::Response& res) {
    try {
        // Récupérer les informations système
        auto streams = config_->getStreams();
        size_t totalStreams = streams.size();
        size_t runningStreams = 0;
        
        for (const auto& stream : streams) {
            if (streamManager_->isStreamRunning(stream.id)) {
                runningStreams++;
            }
        }
        
        // Obtenir l'utilisation CPU et mémoire (exemple simplifié)
        double cpuUsage = 0.0;  // À implémenter en fonction du système
        double memoryUsage = 0.0;  // À implémenter en fonction du système
        
        // Obtenir l'uptime du système (exemple simplifié)
        std::string uptime = "0d 0h 0m";  // À implémenter en fonction du système
        
        // Construire la réponse JSON
        json response = {
            {"totalStreams", totalStreams},
            {"runningStreams", runningStreams},
            {"cpuUsage", cpuUsage},
            {"memoryUsage", memoryUsage},
            {"uptime", uptime},
            {"version", "1.0.0"}  // Version de l'application
        };
        
        // Envoyer la réponse
        res.set_content(response.dump(), "application/json");
    }
    catch (const std::exception& e) {
        handleError(res, 500, "Erreur lors de la récupération des informations système: " + std::string(e.what()));
    }
}

void WebServer::handleError(httplib::Response& res, int statusCode, const std::string& message) {
    // Journaliser l'erreur
    spdlog::error("Erreur API ({}) : {}", statusCode, message);
    
    // Construire la réponse d'erreur
    json response = {
        {"success", false},
        {"error", message}
    };
    
    // Définir le code d'état HTTP
    res.status = statusCode;
    
    // Envoyer la réponse
    res.set_content(response.dump(), "application/json");
}

WebServer::~WebServer() {
    if (running_) {
        stop();
    }
}