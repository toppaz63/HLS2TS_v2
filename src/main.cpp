#include <iostream>
#include <string>
#include <signal.h>
#include <thread>
#include <chrono>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "core/config.h"
#include "core/StreamManager.h"
#include "alerting/AlertManager.h"
#include "web/WebServer.h"

using namespace hls_to_dvb;

// Pour la gestion des signaux
volatile sig_atomic_t g_running = 1;

void signalHandler(int signum) {
    spdlog::info("Signal reçu: {}", signum);
    g_running = 0;
}

// Point d'entrée principal
int main(int argc, char* argv[]) {
    // Vérifier les arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }
    
    std::string configFile = argv[1];
    
    // Configurer le gestionnaire de signaux
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        // S'assurer que le répertoire logs existe
        std::filesystem::create_directories("logs");
        
        // Initialiser le logger
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::info);
        
        // Créer un logger avec plusieurs sinks
        std::vector<spdlog::sink_ptr> sinks {console_sink};
        
        // Ajouter un sink de fichier si le dossier logs existe
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                "logs/hls-to-dvb.log", 10 * 1024 * 1024, 5);
            file_sink->set_level(spdlog::level::debug);
            sinks.push_back(file_sink);
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Erreur lors de la création du fichier de log: " << ex.what() << std::endl;
            std::cerr << "Les logs seront uniquement affichés sur la console" << std::endl;
        }
        
        auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
        
        // Afficher le message de démarrage
        spdlog::info("Démarrage du convertisseur HLS vers MPEG-TS DVB");
        spdlog::info("configFile: {}", configFile);
        // Charger la configuration
        Config config(configFile);
        if (!config.load()) {
            spdlog::error("Erreur lors du chargement de la configuration: {}", configFile);
            return 1;
        }
        spdlog::info("Post config call");

        const auto& loggingConfig = config.getLoggingConfig();
        if (loggingConfig.level == "debug") {
            spdlog::set_level(spdlog::level::debug);
        } else if (loggingConfig.level == "info") {
            spdlog::set_level(spdlog::level::info);
        } else if (loggingConfig.level == "warning") {
            spdlog::set_level(spdlog::level::warn);
        } else if (loggingConfig.level == "error") {
            spdlog::set_level(spdlog::level::err);
        }
        
        spdlog::info("config loaded");

        // Initialiser le gestionnaire d'alertes
        const auto& alertsConfig = config.getAlertRetention();
        
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
        
        // Ajouter une alerte de démarrage
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "System",
            "Application started",
            false
        );
        
        spdlog::info("AlertManager loaded");
        // Créer le gestionnaire de flux
        StreamManager streamManager(&config);
        streamManager.start();
        
        // Démarrer les flux configurés
        for (const auto& streamConfig : config.getStreamConfigs()) {
              spdlog::info("Configuration trouvée - ID: {}, URL: {}, Enabled: {}", 
                streamConfig.id, streamConfig.hlsInput, streamConfig.enabled);
 
            spdlog::info("Before call - Starting stream: {}", streamConfig.id);
            if (streamConfig.enabled) {
                if (!streamManager.startStream(streamConfig.id)) {
                    spdlog::warn("Impossible de démarrer le flux: {}", streamConfig.id);
                }
                spdlog::info("Stream {} started", streamConfig.id);
            }
        }
        spdlog::info("streamManager loaded");
        
        // Créer et démarrer le serveur web
        spdlog::info("Initialisation du serveur web avec le répertoire: web");
        WebServer webServer(config, streamManager, "web");

        spdlog::info("Tentative de démarrage du serveur web...");
        bool webServerStarted = webServer.start();
        spdlog::info("Résultat du démarrage du serveur web: {}", webServerStarted ? "Succès" : "Échec");

        if (!webServerStarted) {
            spdlog::error("Erreur lors du démarrage du serveur web");
            // Ne pas quitter l'application, mais continuer sans serveur web
            // return 1;
        }
        
        // Boucle principale
        spdlog::info("Application démarrée et en cours d'exécution");
        
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // Arrêt propre
        spdlog::info("Arrêt de l'application...");
        
        // Arrêter le serveur web
        webServer.stop();
        
        // Arrêter tous les flux
        streamManager.stop();
        
        // Ajouter une alerte de fin
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "System",
            "Application stopped",
            false
        );
        
        spdlog::info("Application arrêtée");
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception non gérée: " << e.what() << std::endl;
        return 1;
    }
}
