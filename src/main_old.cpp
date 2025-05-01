#include <iostream>
#include <string>
#include <signal.h>
#include <thread>
#include <chrono>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "core/Config.h"
#include "core/StreamManager.h"
#include "alerting/AlertManager.h"
#include "web/WebServer.h"
#include "core/Config.h"

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
        
        // Charger la configuration
        Config config(configFile);
        if (!config.load()) {
            spdlog::error("Erreur lors du chargement de la configuration: {}", configFile);
            return 1;
        }
        
        // Mettre à jour la configuration de journalisation
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
        
        // Initialiser le gestionnaire d'alertes
        const auto& alertsConfig = config.getAlertsConfig();
        AlertManager::getInstance().startCleanupThread(
            alertsConfig.retention.info,
            alertsConfig.retention.warning,
            alertsConfig.retention.error
        );
        
        // Ajouter une alerte de démarrage
        AlertManager::getInstance().addAlert(
            AlertLevel::INFO,
            "System",
            "Application started",
            false
        );
        
        // Créer le gestionnaire de flux
        StreamManager streamManager(config);
        if (!streamManager.initialize()) {
            spdlog::error("Erreur lors de l'initialisation du gestionnaire de flux");
            return 1;
        }
        
        // Démarrer les flux configurés
        if (!streamManager.startAll()) {
            spdlog::warn("Certains flux n'ont pas pu être démarrés");
        }
        
        // Créer et démarrer le serveur web
        WebServer webServer(config, streamManager, "web");
        if (!webServer.start()) {
            spdlog::error("Erreur lors du démarrage du serveur web");
            return 1;
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
        streamManager.stopAll();
        
        // Arrêter le gestionnaire d'alertes
        AlertManager::getInstance().stopCleanupThread();
        
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
