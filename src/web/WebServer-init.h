#include "core/Config.h"
#include "core/StreamManager.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

// Inclure la bibliothèque cpp-httplib
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <fstream>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <random>

namespace hls_to_dvb {

WebServer::WebServer(Config& config, StreamManager& streamManager, const std::string& webRoot)
    : config_(config), streamManager_(streamManager), webRoot_(webRoot), running_(false) {
}

WebServer::~WebServer() {
    stop();
}

bool WebServer::start() {
    if (running_) {
        spdlog::warn("WebServer already running");
        return false;
    }
    
    const auto& serverConfig = config_.getServerConfig();
    
    // Créer le serveur HTTP
    server_ = std::make_unique<httplib::Server>();
    
    // Configurer les routes
    setupRoutes();
    
    // Démarrer le serveur dans un thread séparé
    running_ = true;
    serverThread_ = std::thread([this, &serverConfig]() {
        spdlog::info("Starting WebServer on {}:{}", serverConfig.address, serverConfig.port);
        
        // Démarrer le serveur
        if (!server_->listen(serverConfig.address.c_str(), serverConfig.port)) {
            spdlog::error("Failed to start WebServer");
            running_ = false;
        }
    });
    
    // Attendre un peu pour s'assurer que le serveur démarre correctement
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    if (!running_) {
        return false;
    }
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "WebServer",
        "Started on " + serverConfig.address + ":" + std::to_string(serverConfig.port),
        false
    );
    
    return true;
}

void WebServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Arrêter le serveur
    if (server_) {
        server_->stop();
    }
    
    // Attendre que le thread se termine
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    
    spdlog::info("WebServer stopped");
    
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "WebServer",
        "Stopped",
        false
    );
}

bool WebServer::isRunning() const {
    return running_;
}

void WebServer::setupRoutes() {
    if (!server_) {
        return;
    }
    
    // Servir les fichiers statiques
    server_->set_mount_point("/", webRoot_);
    
    // Route pour l'état du serveur
    server_->Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStatus(req, res);
    });
    
    // Routes pour la gestion des flux
    server_->Get("/api/streams", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStreams(req, res);
    });
    
    server_->Post("/api/streams", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleCreateStream(req, res);
    });
    
    server_->Get("/api/streams/:id", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStream(req, res);
    });
    
    server_->Put("/api/streams/:id", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleUpdateStream(req, res);
    });
    
    server_->Delete("/api/streams/:id", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleDeleteStream(req, res);
    });
    
    server_->Post("/api/streams/:id/start", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleStartStream(req, res);
    });
    
    server_->Post("/api/streams/:id/stop", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleStopStream(req, res);
    });
    
    // Routes pour les statistiques
    server_->Get("/api/stats", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetStats(req, res);
    });
    
    // Routes pour les alertes
    server_->Get("/api/alerts", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleGetAlerts(req, res);
    });
    
    server_->Post("/api/alerts/:id/resolve", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleResolveAlert(req, res);
    });
    
    server_->Get("/api/alerts/export", [this](const httplib::Request& req, httplib::Response& res) {
        this->handleExportAlerts(req, res);
    });
    
    // Route par défaut pour l'application web (SPA)
    server_->Get(".*", [this](const httplib::Request& req, httplib::Response& res) {
        // Rediriger vers index.html pour les routes non trouvées (pour les applications SPA)
        std::ifstream file(webRoot_ + "/index.html");
        if (file) {
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            res.set_content(content, "text/html");
        } else {
            res.status = 404;
            res.set_content("Not Found", "text/plain");
        }
    });
}

void WebServer::handleGetStatus(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json response = {
        {"status", "running"},
        {"uptime", 0}, // À implémenter : calculer le temps d'exécution
        {"totalStreams", streamManager_.getStreamCount()},
        {"runningStreams", streamManager_.getRunningStreamCount()}
    };
    
    res.set_content(response.dump(), "application/json");
}

void WebServer::handleGetStreams(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json response = nlohmann::json::array();
    
    for (const auto& config : streamManager_.getStreamConfigs()) {
        nlohmann::json streamJson = {
            {"id", config.id},
            {"name", config.name},
            {"hlsInput", config.hlsInput},
            {"mcastOutput", config.mcastOutput},
            {"mcastPort", config.mcastPort},
            {"bufferSize", config.bufferSize},
            {"enabled", config.enabled},
            {"status", "stopped"}
        };
        
        // Vérifier si le flux est en cours d'exécution
        const StreamStats* stats = streamManager_.getStreamStats(config.id);
        if (stats) {
            streamJson["status"] = "running";
            streamJson["stats"] = {
                {"segmentsReceived", stats->segmentsReceived},
                {"segmentsSent", stats->segmentsSent},
                {"discontinuities", stats->discontinuities},
                {"bytesReceived", stats->bytesReceived},
                {"bytesSent", stats->bytesSent},
                {"averageBitrate", stats->averageBitrate},
                {"bufferLevel", stats->bufferLevel}
            };
        }
        
        response.push_back(streamJson);
    }
    
    res.set_content(response.dump(), "application/json");
}

void WebServer::handleCreateStream(const httplib::Request& req, httplib::Response& res) {
    try {
        // Analyser le corps de la requête
        nlohmann::json requestBody = nlohmann::json::parse(req.body);
        
        // Créer la configuration du flux
        StreamConfig config;
        
        // Vérifier les champs requis
        if (!requestBody.contains("name") || !requestBody.contains("hlsInput") ||
            !requestBody.contains("mcastOutput") || !requestBody.contains("mcastPort")) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing required fields\"}", "application/json");
            return;
        }
        
        // Récupérer les valeurs
        config.name = requestBody["name"];
        config.hlsInput = requestBody["hlsInput"];
        config.mcastOutput = requestBody["mcastOutput"];
        config.mcastPort = requestBody["mcastPort"];
        
        // Champs optionnels
        if (requestBody.contains("bufferSize")) {
            config.bufferSize = requestBody["bufferSize"];
        }
        
        if (requestBody.contains("enabled")) {
            config.enabled = requestBody["enabled"];
        }
        
        // Générer un ID si non fourni
        if (!requestBody.contains("id") || requestBody["id"].get<std::string>().empty()) {
            config.id = generateStreamId(config.name);
        } else {
            config.id = requestBody["id"];
        }
        
        // Ajouter le flux
        if (!streamManager_.addStream(config)) {
            res.status = 500;
            res.set_content("{\"error\":\"Failed to create stream\"}", "application/json");
            return;
        }
        
        // Réponse
        nlohmann::json response = {
            {"id", config.id},
            {"name", config.name},
            {"hlsInput", config.hlsInput},
            {"mcastOutput", config.mcastOutput},
            {"mcastPort", config.mcastPort},
            {"bufferSize", config.bufferSize},
            {"enabled", config.enabled},
            {"status", config.enabled ? "running" : "stopped"}
        };
        
        res.status = 201;
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content("{\"error\":\"" + std::string(e.what()) + "\"}", "application/json");
    }
}

void WebServer::handleGetStream(const httplib::Request& req, httplib::Response& res) {
    std::string streamId = req.path_params.at("id");
    
    // Vérifier si le flux existe
    const auto& configs = streamManager_.getStreamConfigs();
    auto it = std::find_if(configs.begin(), configs.end(),
                         [&streamId](const StreamConfig& config) {
                             return config.id == streamId;
                         });
    
    if (it == configs.end()) {
        res.status = 404;
        res.set_content("{\"error\":\"Stream not found\"}", "application/json");
        return;
    }
    
    // Construire la réponse
    nlohmann::json response = {
        {"id", it->id},
        {"name", it->name},
        {"hlsInput", it->hlsInput},
        {"mcastOutput", it->mcastOutput},
        {"mcastPort", it->mcastPort},
        {"bufferSize", it->bufferSize},
        {"enabled", it->enabled},
        {"status", "stopped"}
    };
    
    // Ajouter les statistiques si le flux est en cours d'exécution
    const StreamStats* stats = streamManager_.getStreamStats(streamId);
    if (stats) {
        response["status"] = "running";
        response["stats"] = {
            {"segmentsReceived", stats->segmentsReceived},
            {"segmentsSent", stats->segmentsSent},
            {"discontinuities", stats->discontinuities},
            {"bytesReceived", stats->bytesReceived},
            {"bytesSent", stats->bytesSent},
            {"averageBitrate", stats->averageBitrate},
            {"bufferLevel", stats->bufferLevel}
        };
    }
    
    res.set_content(response.dump(), "application/json");
}

void WebServer::handleUpdateStream(const httplib::Request& req, httplib::Response& res) {
    std::string streamId = req.path_params.at("id");
    
    try {
        // Analyser le corps de la requête
        nlohmann::json requestBody = nlohmann::json::parse(req.body);
        
        // Créer la configuration du flux
        StreamConfig config;
        config.id = streamId;
        
        // Vérifier les champs requis
        if (!requestBody.contains("name") || !requestBody.contains("hlsInput") ||
            !requestBody.contains("mcastOutput") || !requestBody.contains("mcastPort")) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing required fields\"}", "application/json");
            return;
        }
        
        // Récupérer les valeurs
        config.name = requestBody["name"];
        config.hlsInput = requestBody["hlsInput"];
        config.mcastOutput = requestBody["mcastOutput"];
        config.mcastPort = requestBody["mcastPort"];
        
        // Champs optionnels
        if (requestBody.contains("bufferSize")) {
            config.bufferSize = requestBody["bufferSize"];
        }
        
        if (requestBody.contains("enabled")) {
            config.enabled = requestBody["enabled"];
        }
        
        // Mettre à jour le flux
        if (!streamManager_.updateStream(config)) {
            res.status = 500;
            res.set_content("{\"error\":\"Failed to update stream\"}", "application/json");
            return;
        }
        
        // Construire la réponse
        nlohmann::json response = {
            {"id", config.id},
            {"name", config.name},
            {"hlsInput", config.hlsInput},
            {"mcastOutput", config.mcastOutput},
            {"mcastPort", config.mcastPort},
            {"bufferSize", config.bufferSize},
            {"enabled", config.enabled},
            {"status", "stopped"}
        };
        
        // Ajouter les statistiques si le flux est en cours d'exécution
        const StreamStats* stats = streamManager_.getStreamStats(streamId);
        if (stats) {
            response["status"] = "running";
            response["stats"] = {
                {"segmentsReceived", stats->segmentsReceived},
                {"segmentsSent", stats->segmentsSent},
                {"discontinuities", stats->discontinuities},
                {"bytesReceived", stats->bytesReceived},
                {"bytesSent", stats->bytesSent},
                {"averageBitrate", stats->averageBitrate},
                {"bufferLevel", stats->bufferLevel}
            };
        }
        
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content("{\"error\":\"" + std::string(e.what()) + "\"}", "application/json");
    }
}

void WebServer::handleDeleteStream(const httplib::Request& req, httplib::Response& res) {
    std::string streamId = req.path_params.at("id");
    
    // Supprimer le flux
    if (!streamManager_.removeStream(streamId)) {
        res.status = 404;
        res.set_content("{\"error\":\"Stream not found\"}", "application/json");
        return;
    }
    
    res.set_content("{\"status\":\"success\"}", "application/json");
}

void WebServer::handleStartStream(const httplib::Request& req, httplib::Response& res) {
    std::string streamId = req.path_params.at("id");
    
    // Démarrer le flux
    if (!streamManager_.startStream(streamId)) {
        res.status = 500;
        res.set_content("{\"error\":\"Failed to start stream\"}", "application/json");
        return;
    }
    
    res.set_content("{\"status\":\"started\"}", "application/json");
}

void WebServer::handleStopStream(const httplib::Request& req, httplib::Response& res) {
    std::string streamId = req.path_params.at("id");
    
    // Arrêter le flux
    if (!streamManager_.stopStream(streamId)) {
        res.status = 500;
        res.set_content("{\"error\":\"Failed to stop stream\"}", "application/json");
        return;
    }
    
    res.set_content("{\"status\":\"stopped\"}", "application/json");
}

void WebServer::handleGetStats(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json response = {
        {"global", {
            {"totalStreams", streamManager_.getStreamCount()},
            {"runningStreams", streamManager_.getRunningStreamCount()}
        }},
        {"streams", nlohmann::json::object()}
    };
    
    // Récupérer les statistiques pour chaque flux
    for (const auto& id : streamManager_.getStreamIds()) {
        const StreamStats* stats = streamManager_.getStreamStats(id);
        if (stats) {
            response["streams"][id] = {
                {"segmentsReceived", stats->segmentsReceived},
                {"segmentsSent", stats->segmentsSent},
                {"discontinuities", stats->discontinuities},
                {"bytesReceived", stats->bytesReceived},
                {"bytesSent", stats->bytesSent},
                {"averageBitrate", stats->averageBitrate},
                {"bufferLevel", stats->bufferLevel}
            };
        }
    }
    
    res.set_content(response.dump(), "application/json");
}

void WebServer::handleGetAlerts(const httplib::Request& req, httplib::Response& res) {
    // Récupérer les paramètres de requête
    std::vector<AlertLevel> levels;
    size_t limit = 100;
    std::string componentFilter;
    std::string textFilter;
    
    // Niveaux d'alerte
    if (req.has_param("levels")) {
        std::string levelsParam = req.get_param_value("levels");
        std::istringstream iss(levelsParam);
        std::string level;
        while (std::getline(iss, level, ',')) {
            levels.push_back(stringToAlertLevel(level));
        }
    } else {
        levels = {AlertLevel::INFO, AlertLevel::WARNING, AlertLevel::ERROR};
    }
    
    // Limite
    if (req.has_param("limit")) {
        try {
            limit = std::stoul(req.get_param_value("limit"));
        } catch (...) {
            limit = 100;
        }
    }
    
    // Filtre par composant
    if (req.has_param("component")) {
        componentFilter = req.get_param_value("component");
    }
    
    // Filtre par texte
    if (req.has_param("text")) {
        textFilter = req.get_param_value("text");
    }
    
    // Récupérer les alertes
    auto alerts = AlertManager::getInstance().getAlerts(levels, limit, componentFilter, textFilter);
    
    // Construire la réponse
    nlohmann::json response = nlohmann::json::array();
    
    for (const auto& alert : alerts) {
        // Convertir l'horodatage en chaîne ISO
        auto timeT = std::chrono::system_clock::to_time_t(alert.timestamp);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&timeT), "%FT%T%z");
        
        response.push_back({
            {"level", alertLevelToString(alert.level)},
            {"component", alert.component},
            {"message", alert.message},
            {"timestamp", ss.str()},
            {"persistent", alert.persistent},
            {"id", alert.id}
        });
    }
    
    res.set_content(response.dump(), "application/json");
}

void WebServer::handleResolveAlert(const httplib::Request& req, httplib::Response& res) {
    std::string alertId = req.path_params.at("id");
    
    // Résoudre l'alerte
    if (!AlertManager::getInstance().resolveAlert(alertId)) {
        res.status = 404;
        res.set_content("{\"error\":\"Alert not found\"}", "application/json");
        return;
    }
    
    res.set_content("{\"status\":\"success\"}", "application/json");
}

void WebServer::handleExportAlerts(const httplib::Request& req, httplib::Response& res) {
    // Récupérer les paramètres de requête
    std::vector<AlertLevel> levels;
    std::string componentFilter;
    std::string textFilter;
    std::string format = "json";
    
    // Niveaux d'alerte
    if (req.has_param("levels")) {
        std::string levelsParam = req.get_param_value("levels");
        std::istringstream iss(levelsParam);
        std::string level;
        while (std::getline(iss, level, ',')) {
            levels.push_back(stringToAlertLevel(level));
        }
    } else {
        levels = {AlertLevel::INFO, AlertLevel::WARNING, AlertLevel::ERROR};
    }
    
    // Filtre par composant
    if (req.has_param("component")) {
        componentFilter = req.get_param_value("component");
    }
    
    // Filtre par texte
    if (req.has_param("text")) {
        textFilter = req.get_param_value("text");
    }
    
    // Format
    if (req.has_param("format")) {
        format = req.get_param_value("format");
    }
    
    // Exporter les alertes dans le format demandé
    std::string content;
    if (format == "csv") {
        content = AlertManager::getInstance().exportAlertsToCsv(levels, componentFilter, textFilter);
        res.set_header("Content-Type", "text/csv");
        res.set_header("Content-Disposition", "attachment; filename=alerts.csv");
    } else {
        content = AlertManager::getInstance().exportAlertsToJson(levels, componentFilter, textFilter);
        res.set_header("Content-Type", "application/json");
        res.set_header("Content-Disposition", "attachment; filename=alerts.json");
    }
    
    res.set_content(content, res.get_header_value("Content-Type"));
}

std::string WebServer::generateStreamId(const std::string& name) {
    // Générer un ID à partir du nom
    std::string id = name;
    
    // Remplacer les espaces par des tirets
    std::replace(id.begin(), id.end(), ' ', '-');
    
    // Supprimer les caractères spéciaux
    id.erase(std::remove_if(id.begin(), id.end(), 
                           [](char c) { return !std::isalnum(c) && c != '-'; }), 
            id.end());
    
    // Convertir en minuscules
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);
    
    // Ajouter un suffixe aléatoire pour éviter les collisions
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    id += "-" + std::to_string(dis(gen));
    
    return id;
}

} // namespace hls_to_dvb
