#include "web/WebServer.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <fstream>  // Ajout pour std::ifstream
#include <algorithm> // Pour std::transform, std::replace, etc.
#include "alerting/AlertManager.h" // Ajout de l'include pour AlertManager
#include <cerrno> // Pour strerror

// Inclure httplib avec l'implémentation
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace hls_to_dvb {

WebServer::WebServer(Config& config, StreamManager& streamManager, const std::string& webRoot)
    : config_(config), streamManager_(streamManager), webRoot_(webRoot), running_(false) {
    server_ = std::make_unique<httplib::Server>();
}

WebServer::~WebServer() {
    stop();
}

bool WebServer::start() {
    spdlog::critical("************ WebServer::start() CALLED! ************");
    if (isRunning()) {
        spdlog::warn("Le serveur web est déjà en cours d'exécution");
        return true;
    }
    
    // Configurer les routes
    setupRoutes();
    
    // Obtenir la configuration du serveur
    const auto& serverConfig = config_.getServerConfig();
    
    // Afficher plus d'informations de débogage
    spdlog::info("Tentative de démarrage du serveur web sur {}:{}", serverConfig.address, serverConfig.port);
    spdlog::info("Répertoire web racine: {}", webRoot_);
    
    // Démarrer le serveur dans un thread séparé
    running_ = true;
    serverThread_ = std::thread([this, serverConfig]() {
        try {
            spdlog::info("Thread du serveur démarré");
            
            // Configurer le répertoire statique
            server_->set_mount_point("/", webRoot_);
            spdlog::info("Point de montage configuré: / -> {}", webRoot_);
            
            // Ajouter une route de base pour tester
            server_->Get("/test", [](const httplib::Request&, httplib::Response& res) {
                res.set_content("Test server is running", "text/plain");
            });
            spdlog::info("Route de test ajoutée: /test");
            
            // Démarrer le serveur (bloquant)
            spdlog::info("Démarrage de l'écoute sur {}:{}", serverConfig.address, serverConfig.port);
            if (!server_->listen(serverConfig.address.c_str(), serverConfig.port)) {
                spdlog::error("Erreur lors du démarrage du serveur web: {}", strerror(errno));
                running_ = false;
            }
        } catch (const std::exception& e) {
            spdlog::error("Exception dans le thread du serveur: {}", e.what());
            running_ = false;
        } catch (...) {
            spdlog::error("Exception inconnue dans le thread du serveur");
            running_ = false;
        }
    });
    
    // Attendre un peu plus longtemps pour vérifier si le serveur a démarré correctement
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    if (running_) {
        spdlog::info("Serveur web démarré avec succès");
    } else {
        spdlog::error("Échec du démarrage du serveur web");
    }
    
    return running_;
}

void WebServer::stop() {
    if (!isRunning()) {
        return;
    }
    
    spdlog::info("Arrêt du serveur web");
    
    // Arrêter le serveur
    server_->stop();
    
    // Attendre la fin du thread
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    
    running_ = false;
}

bool WebServer::isRunning() const {
    return running_;
}

void WebServer::setupRoutes() {
    // Gérer les routes API
    server_->Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStatus(req, res);
    });
    
    server_->Get("/api/streams", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStreams(req, res);
    });
    
    server_->Post("/api/streams", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleCreateStream(req, res);
    });
    
    server_->Get(R"(/api/streams/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStream(req, res);
    });
    
    server_->Put(R"(/api/streams/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleUpdateStream(req, res);
    });
    
    server_->Delete(R"(/api/streams/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleDeleteStream(req, res);
    });
    
    server_->Post(R"(/api/streams/([^/]+)/start)", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleStartStream(req, res);
    });
    
    server_->Post(R"(/api/streams/([^/]+)/stop)", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleStopStream(req, res);
    });
    
    server_->Get("/api/stats", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStats(req, res);
    });
    
    server_->Get("/api/alerts", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetAlerts(req, res);
    });
    
    server_->Post(R"(/api/alerts/([^/]+)/resolve)", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleResolveAlert(req, res);
    });
    
    server_->Get("/api/alerts/export", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleExportAlerts(req, res);
    });
    
    // Route par défaut pour le SPA
    server_->Get(".*", [this]([[maybe_unused]] const httplib::Request& req, httplib::Response& res) {
        // Définir le type de contenu avec set_header au lieu de set_content_type
        res.set_header("Content-Type", "text/html");
        
        // Rediriger toutes les routes inconnues vers index.html pour le SPA
        auto indexPath = webRoot_ + "/index.html";
        
        // Lecture du fichier complète et mise dans la réponse
        std::ifstream file(indexPath, std::ios::binary);
        if (!file) {
            res.status = 404;
            res.set_content("<html><body><h1>404 Not Found</h1></body></html>", "text/html");
            return;
        }
        
        // Lire tout le fichier en une fois
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        res.set_content(content, "text/html");
    });
}

// Implémentation des gestionnaires de route

void WebServer::handleGetStatus([[maybe_unused]] const httplib::Request& req, httplib::Response& res) {
    nlohmann::json response = {
        {"status", "ok"},
        {"uptime", 0},  // À implémenter
        {"version", "1.0.0"}
    };
    
    res.set_content(response.dump(), "application/json");
}

void WebServer::handleGetStreams([[maybe_unused]] const httplib::Request& req, httplib::Response& res) {
    nlohmann::json streamsJson = nlohmann::json::array();
    
    for (const auto& streamConfig : config_.getStreamConfigs()) {
        bool isRunning = streamManager_.isStreamRunning(streamConfig.id);
        
        nlohmann::json streamJson = {
            {"id", streamConfig.id},
            {"name", streamConfig.name},
            {"hlsInput", streamConfig.hlsInput},
            {"multicastOutput", streamConfig.mcastOutput},
            {"multicastPort", streamConfig.mcastPort},
            {"bufferSize", streamConfig.bufferSize},
            {"enabled", streamConfig.enabled},
            {"running", isRunning}
        };
        
        // Ajouter les statistiques si le flux est en cours d'exécution
        if (isRunning) {
            auto stats = streamManager_.getStreamStats(streamConfig.id);
            if (stats) {
                streamJson["stats"] = {
                    {"segmentsProcessed", stats->segmentsProcessed},
                    {"discontinuitiesDetected", stats->discontinuitiesDetected},
                    {"bufferSize", stats->bufferSize},
                    {"bufferCapacity", stats->bufferCapacity},
                    {"packetsTransmitted", stats->packetsTransmitted},
                    {"currentBitrate", stats->currentBitrate},
                    {"width", stats->width},
                    {"height", stats->height},
                    {"bandwidth", stats->bandwidth},
                    {"codecs", stats->codecs}
                };
            }
        }
        
        streamsJson.push_back(streamJson);
    }
    
    res.set_content(streamsJson.dump(), "application/json");
}

// Implémentation des autres méthodes de gestionnaire

void WebServer::handleCreateStream(const httplib::Request& req, httplib::Response& res) {
    try {
        auto json = nlohmann::json::parse(req.body);
        
        StreamConfig config;
        config.name = json.value("name", "");
        config.hlsInput = json.value("hlsInput", "");
        config.mcastOutput = json.value("multicastOutput", "");
        config.mcastPort = json.value("multicastPort", 1234);
        config.bufferSize = json.value("bufferSize", 3);
        config.enabled = json.value("enabled", true);
        
        // Générer un ID si non fourni
        config.id = json.value("id", generateStreamId(config.name));
        
        // Vérifier si l'ID existe déjà
        if (config_.getStreamConfig(config.id) != nullptr) {
            res.status = 409; // Conflict
            nlohmann::json response = {
                {"error", "Un flux avec cet ID existe déjà"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }
        
        // Mettre à jour la configuration
        if (!config_.updateStreamConfig(config)) {
            res.status = 500;
            nlohmann::json response = {
                {"error", "Erreur lors de la création du flux"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }
        
        // Retourner la configuration créée
        nlohmann::json response = {
            {"id", config.id},
            {"name", config.name},
            {"hlsInput", config.hlsInput},
            {"multicastOutput", config.mcastOutput},
            {"multicastPort", config.mcastPort},
            {"bufferSize", config.bufferSize},
            {"enabled", config.enabled},
            {"running", false}
        };
        
        res.status = 201; // Created
        res.set_content(response.dump(), "application/json");
    } 
    catch (const std::exception& e) {
        res.status = 400;
        nlohmann::json response = {
            {"error", std::string("Erreur lors du parsing de la requête: ") + e.what()}
        };
        res.set_content(response.dump(), "application/json");
    }
}

void WebServer::handleGetStream(const httplib::Request& req, httplib::Response& res) {
    // Extraire l'ID du flux depuis les captures regex
    std::string streamId = req.matches[1];
    
    // Rechercher le flux dans la configuration
    const auto* streamConfig = config_.getStreamConfig(streamId);
    if (!streamConfig) {
        res.status = 404;
        nlohmann::json response = {
            {"error", "Flux non trouvé"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Vérifier si le flux est en cours d'exécution
    bool isRunning = streamManager_.isStreamRunning(streamId);
    
    // Construire la réponse JSON
    nlohmann::json response = {
        {"id", streamConfig->id},
        {"name", streamConfig->name},
        {"hlsInput", streamConfig->hlsInput},
        {"multicastOutput", streamConfig->mcastOutput},
        {"multicastPort", streamConfig->mcastPort},
        {"bufferSize", streamConfig->bufferSize},
        {"enabled", streamConfig->enabled},
        {"running", isRunning}
    };
    
    // Ajouter les statistiques si le flux est en cours d'exécution
    if (isRunning) {
        auto stats = streamManager_.getStreamStats(streamId);
        if (stats) {
            response["stats"] = {
                {"segmentsProcessed", stats->segmentsProcessed},
                {"discontinuitiesDetected", stats->discontinuitiesDetected},
                {"bufferSize", stats->bufferSize},
                {"bufferCapacity", stats->bufferCapacity},
                {"packetsTransmitted", stats->packetsTransmitted},
                {"currentBitrate", stats->currentBitrate},
                {"width", stats->width},
                {"height", stats->height},
                {"bandwidth", stats->bandwidth},
                {"codecs", stats->codecs}
            };
        }
    }
    
    res.set_content(response.dump(), "application/json");
}

void WebServer::handleUpdateStream(const httplib::Request& req, httplib::Response& res) {
    // Extraire l'ID du flux depuis les captures regex
    std::string streamId = req.matches[1];
    
    // Vérifier si le flux existe
    const auto* existingConfig = config_.getStreamConfig(streamId);
    if (!existingConfig) {
        res.status = 404;
        nlohmann::json response = {
            {"error", "Flux non trouvé"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    try {
        auto json = nlohmann::json::parse(req.body);
        
        // Partir de la configuration existante
        StreamConfig config = *existingConfig;
        
        // Mettre à jour les champs fournis
        if (json.contains("name")) config.name = json["name"];
        if (json.contains("hlsInput")) config.hlsInput = json["hlsInput"];
        if (json.contains("multicastOutput")) config.mcastOutput = json["multicastOutput"];
        if (json.contains("multicastPort")) config.mcastPort = json["multicastPort"];
        if (json.contains("bufferSize")) config.bufferSize = json["bufferSize"];
        if (json.contains("enabled")) config.enabled = json["enabled"];
        
        // Mettre à jour la configuration
        if (!config_.updateStreamConfig(config)) {
            res.status = 500;
            nlohmann::json response = {
                {"error", "Erreur lors de la mise à jour du flux"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }
        
        // Vérifier si le flux est en cours d'exécution
        bool isRunning = streamManager_.isStreamRunning(streamId);
        
        // Construire la réponse JSON
        nlohmann::json response = {
            {"id", config.id},
            {"name", config.name},
            {"hlsInput", config.hlsInput},
            {"multicastOutput", config.mcastOutput},
            {"multicastPort", config.mcastPort},
            {"bufferSize", config.bufferSize},
            {"enabled", config.enabled},
            {"running", isRunning}
        };
        
        res.set_content(response.dump(), "application/json");
    } 
    catch (const std::exception& e) {
        res.status = 400;
        nlohmann::json response = {
            {"error", std::string("Erreur lors du parsing de la requête: ") + e.what()}
        };
        res.set_content(response.dump(), "application/json");
    }
}

void WebServer::handleDeleteStream(const httplib::Request& req, httplib::Response& res) {
    // Extraire l'ID du flux depuis les captures regex
    std::string streamId = req.matches[1];
    
    // Vérifier si le flux existe
    if (config_.getStreamConfig(streamId) == nullptr) {
        res.status = 404;
        nlohmann::json response = {
            {"error", "Flux non trouvé"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Arrêter le flux s'il est en cours d'exécution
    if (streamManager_.isStreamRunning(streamId)) {
        streamManager_.stopStream(streamId);
    }
    
    // Supprimer le flux de la configuration
    if (!config_.removeStreamConfig(streamId)) {
        res.status = 500;
        nlohmann::json response = {
            {"error", "Erreur lors de la suppression du flux"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Réponse 204 No Content
    res.status = 204;
}

void WebServer::handleStartStream(const httplib::Request& req, httplib::Response& res) {
    // Extraire l'ID du flux depuis les captures regex
    std::string streamId = req.matches[1];
    
    // Vérifier si le flux existe
    const auto* streamConfig = config_.getStreamConfig(streamId);
    if (!streamConfig) {
        res.status = 404;
        nlohmann::json response = {
            {"error", "Flux non trouvé"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Vérifier si le flux est déjà en cours d'exécution
    if (streamManager_.isStreamRunning(streamId)) {
        res.status = 409; // Conflict
        nlohmann::json response = {
            {"error", "Le flux est déjà en cours d'exécution"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Démarrer le flux (passer l'ID, pas le StreamConfig)
    if (!streamManager_.startStream(streamId)) {
        res.status = 500;
        nlohmann::json response = {
            {"error", "Erreur lors du démarrage du flux"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Réponse 204 No Content
    res.status = 204;
}

void WebServer::handleStopStream(const httplib::Request& req, httplib::Response& res) {
    // Extraire l'ID du flux depuis les captures regex
    std::string streamId = req.matches[1];
    
    // Vérifier si le flux existe
    if (config_.getStreamConfig(streamId) == nullptr) {
        res.status = 404;
        nlohmann::json response = {
            {"error", "Flux non trouvé"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Vérifier si le flux est en cours d'exécution
    if (!streamManager_.isStreamRunning(streamId)) {
        res.status = 409; // Conflict
        nlohmann::json response = {
            {"error", "Le flux n'est pas en cours d'exécution"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Arrêter le flux
    if (!streamManager_.stopStream(streamId)) {
        res.status = 500;
        nlohmann::json response = {
            {"error", "Erreur lors de l'arrêt du flux"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Réponse 204 No Content
    res.status = 204;
}

void WebServer::handleGetStats([[maybe_unused]] const httplib::Request& req, httplib::Response& res) {
    // Compter les flux en cours d'exécution
    int runningStreams = 0;
    for (const auto& config : config_.getStreamConfigs()) {
        if (streamManager_.isStreamRunning(config.id)) {
            runningStreams++;
        }
    }

    nlohmann::json stats = {
        {"streams", {
            {"total", config_.getStreamConfigs().size()},
            {"running", runningStreams}
        }},
        {"system", {
            {"cpuUsage", 0.0},  // À implémenter
            {"memoryUsage", 0.0}  // À implémenter
        }}
    };
    
    res.set_content(stats.dump(), "application/json");
}

void WebServer::handleGetAlerts([[maybe_unused]] const httplib::Request& req, httplib::Response& res) {
    nlohmann::json alertsJson = nlohmann::json::array();
    
    // Récupérer les alertes depuis AlertManager
    auto alerts = hls_to_dvb::AlertManager::getInstance().getActiveAlerts();
    
    for (const auto& alert : alerts) {
        nlohmann::json alertJson = {
            {"id", alert.id},
            {"level", static_cast<int>(alert.level)},
            {"message", alert.message},
            {"component", alert.component},
            {"timestamp", std::chrono::system_clock::to_time_t(alert.timestamp)},
            {"persistent", alert.persistent}
        };
        
        alertsJson.push_back(alertJson);
    }
    
    res.set_content(alertsJson.dump(), "application/json");
}

void WebServer::handleResolveAlert(const httplib::Request& req, httplib::Response& res) {
    // Extraire l'ID de l'alerte depuis les captures regex
    std::string alertId = req.matches[1];
    
    // Résoudre l'alerte
    if (!hls_to_dvb::AlertManager::getInstance().resolveAlert(alertId)) {
        res.status = 404;
        nlohmann::json response = {
            {"error", "Alerte non trouvée"}
        };
        res.set_content(response.dump(), "application/json");
        return;
    }
    
    // Réponse 204 No Content
    res.status = 204;
}

void WebServer::handleExportAlerts([[maybe_unused]] const httplib::Request& req, httplib::Response& res) {
    nlohmann::json alertsJson = nlohmann::json::array();
    
    // Récupérer les alertes depuis AlertManager
    auto alerts = hls_to_dvb::AlertManager::getInstance().getActiveAlerts();
    
    for (const auto& alert : alerts) {
        nlohmann::json alertJson = {
            {"id", alert.id},
            {"level", static_cast<int>(alert.level)},
            {"message", alert.message},
            {"component", alert.component},
            {"timestamp", std::chrono::system_clock::to_time_t(alert.timestamp)},
            {"persistent", alert.persistent}
        };
        
        alertsJson.push_back(alertJson);
    }
    
    // Configurer l'en-tête pour le téléchargement
    res.set_header("Content-Disposition", "attachment; filename=alerts.json");
    res.set_content(alertsJson.dump(4), "application/json");
}

std::string WebServer::generateStreamId(const std::string& name) {
    // Générer un ID à partir du nom (slugify)
    std::string id = name;
    
    // Remplacer les espaces par des tirets
    std::replace(id.begin(), id.end(), ' ', '-');
    
    // Supprimer les caractères non alphanumériques
    id.erase(std::remove_if(id.begin(), id.end(), [](char c) {
        return !std::isalnum(c) && c != '-' && c != '_';
    }), id.end());
    
    // Convertir en minuscules
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);
    
    return id;
}

} // namespace hls_to_dvb