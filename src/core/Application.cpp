#include "core/Application.h"
#include "core/config.h"
#include "core/StreamManager.h"
#include "web/WebServer.h"
#include "alerting/AlertManager.h"
#include <iostream>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <thread>
#include <filesystem>

namespace hls_to_dvb {

Application::Application(const std::string& configPath)
    : configPath_(configPath), running_(false) {
}

Application::~Application() {
    stop();
}

bool Application::initialize() {
    // Initialiser la configuration
    config_ = std::make_unique<Config>(configPath_);
    if (!config_->load()) {
        std::cerr << "Failed to load configuration from: " << configPath_ << std::endl;
        return false;
    }
    
    // Initialiser la journalisation
    if (!initializeLogging()) {
        std::cerr << "Failed to initialize logging" << std::endl;
        return false;
    }
    
    // Initialiser le gestionnaire d'alertes
    if (!initializeAlertManager()) {
        spdlog::error("Failed to initialize alert manager");
        return false;
    }
    
    // Initialiser le gestionnaire de flux
    streamManager_ = std::make_unique<StreamManager>(config_.get());
    //if (!streamManager_->start()) {
    //    spdlog::error("Failed to initialize stream manager");
    //    return false;
    //}
    
    // Initialiser le serveur web
    webServer_ = std::make_unique<WebServer>(*config_, *streamManager_, "web");
    
    // Ajouter une alerte d'initialisation
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "Application",
        "Application initialized successfully",
        false
    );
    
    spdlog::info("Application initialized successfully");
    return true;
}

bool Application::start() {
    if (running_) {
        spdlog::warn("Application already running");
        return false;
    }
    
    // Démarrer le gestionnaire de flux
    streamManager_->start();
    
    // Démarrer le serveur web
    if (!webServer_->start()) {
        spdlog::error("Failed to start web server");
        return false;
    }
    
    // Marquer l'application comme démarrée
    running_ = true;
    startTime_ = std::chrono::system_clock::now();
    
    // Ajouter une alerte de démarrage
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "Application",
        "Application started",
        false
    );
    
    spdlog::info("Application started");
    return true;
}

void Application::stop() {
    if (!running_) {
        return;
    }
    
    spdlog::info("Stopping application...");
    
    // Arrêter le serveur web
    if (webServer_) {
        webServer_->stop();
    }
    
    // Arrêter le gestionnaire de flux
    if (streamManager_) {
        streamManager_->stop();
    }
    
    // Arrêter le gestionnaire d'alertes
    //AlertManager::getInstance().stopCleanupThread();
    
    // Ajouter une alerte d'arrêt
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "Application",
        "Application stopped",
        false
    );
    
    running_ = false;
    spdlog::info("Application stopped");
}

void Application::run() {
    if (!running_) {
        if (!start()) {
            spdlog::error("Failed to start application");
            return;
        }
    }
    
    spdlog::info("Application running, press Ctrl+C to stop");
    
    // Boucle principale
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool Application::isRunning() const {
    return running_;
}

uint64_t Application::getUptime() const {
    if (!running_) {
        return 0;
    }
    
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
}

Config& Application::getConfig() {
    return *config_;
}

StreamManager& Application::getStreamManager() {
    return *streamManager_;
}

bool Application::initializeLogging() {
    try {
        const auto& loggingConfig = config_->getLoggingConfig();
        
        // Créer les sinks
        std::vector<spdlog::sink_ptr> sinks;
        
        // Console sink
        if (loggingConfig.console) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinks.push_back(console_sink);
        }
        
        // File sink
        if (loggingConfig.file.enabled) {
            // Créer le dossier des logs s'il n'existe pas
            std::filesystem::path logPath(loggingConfig.file.path);
            std::filesystem::create_directories(logPath.parent_path());
            
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                loggingConfig.file.path,
                loggingConfig.file.rotationSize,
                loggingConfig.file.maxFiles
            );
            sinks.push_back(file_sink);
        }
        
        // Créer le logger
        auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
        
        // Configurer le niveau de log
        spdlog::level::level_enum level = spdlog::level::info;
        if (loggingConfig.level == "debug") {
            level = spdlog::level::debug;
        } else if (loggingConfig.level == "info") {
            level = spdlog::level::info;
        } else if (loggingConfig.level == "warning") {
            level = spdlog::level::warn;
        } else if (loggingConfig.level == "error") {
            level = spdlog::level::err;
        }
        
        logger->set_level(level);
        spdlog::set_default_logger(logger);
        
        spdlog::info("Logging initialized: level={}, console={}, file={}",
                    loggingConfig.level, loggingConfig.console, loggingConfig.file.enabled);
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error initializing logging: " << e.what() << std::endl;
        return false;
    }
}

bool Application::initializeAlertManager() {
    try {
        // Utiliser la méthode getAlertRetention au lieu de getAlertsConfig
        const auto& alertsConfig = config_->getAlertRetention();
        
        // Démarrer le thread de nettoyage
        AlertManager::getInstance();
        
        // Utiliser directement les membres de la structure AlertRetentionConfig
        AlertManager::getInstance().setRetention(
            AlertLevel::INFO,
            alertsConfig.info
        );
        AlertManager::getInstance().setRetention(
            AlertLevel::WARNING,
            alertsConfig.warning
        );
        AlertManager::getInstance().setRetention(
            AlertLevel::ERROR,
            alertsConfig.error
        );
        
        spdlog::info("AlertManager initialized");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Error initializing AlertManager: {}", e.what());
        return false;
    }
}


} // namespace hls_to_dvb
