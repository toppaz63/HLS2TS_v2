#include "mpegts/DVBProcessor.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <cstring>
#include <iostream>
// Utilisation de TSDuck pour manipuler les tables DVB
#include <tsduck/tsduck.h>

using namespace hls_to_dvb;

DVBProcessor::DVBProcessor() 
    : versionPAT_(0), versionSDT_(0), versionEIT_(0), versionNIT_(0) {
}

void DVBProcessor::initialize() {
    spdlog::info("**** DVBProcessor::initialize() **** Début");
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Créer les tables PSI de base avec des valeurs par défaut
        pat_ = std::make_unique<ts::PAT>();
        pat_->ts_id = 1;
        pat_->version = versionPAT_;
        pat_->is_current = true;
        
        spdlog::info("**** DVBProcessor::initialize() **** PAT créée");
        
        sdt_ = std::make_unique<ts::SDT>();
        sdt_->ts_id = 1;
        sdt_->onetw_id = 1; // Nom de propriété correct dans TSDuck 3.40
        sdt_->version = versionSDT_;
        sdt_->is_current = true;
        
        spdlog::info("**** DVBProcessor::initialize() **** SDT créée");
        
        nit_ = std::make_unique<ts::NIT>();
        nit_->network_id = 1;
        nit_->version = versionNIT_;
        nit_->is_current = true;
        
        spdlog::info("**** DVBProcessor::initialize() **** NIT créée");
        
        eit_ = std::make_unique<ts::EIT>();
        eit_->service_id = 0;
        eit_->ts_id = 1;
        eit_->onetw_id = 1; // Nom de propriété correct dans TSDuck 3.40
        eit_->version = versionEIT_;
        eit_->is_current = true;
        
        spdlog::info("**** DVBProcessor::initialize() **** EIT créée");
        
        // Créer un service par défaut
        DVBService defaultService;
        defaultService.serviceId = 1;
        defaultService.pmtPid = 0x1000;
        defaultService.name = "Service HLS";
        defaultService.provider = "HLS to DVB Converter";
        defaultService.serviceType = 0x01; // Digital TV
        defaultService.components[0x1001] = 0x1B; // H.264 Video
        defaultService.components[0x1002] = 0x03; // MPEG Audio
 
        
        spdlog::info("**** DVBProcessor::initialize() **** Création du service par défaut");
        
        try {
            // Ajouter le service par défaut
            setServiceInternal(defaultService);
        }
        catch (const ts::Exception& e) {
            spdlog::error("**** DVBProcessor::initialize() **** Erreur TSDuck lors de la définition du service par défaut: {}", e.what());
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "DVBProcessor",
                std::string("Erreur TSDuck lors de la définition du service par défaut: ") + e.what(),
                true
            );
            
            throw;
        }
        catch (const std::exception& e) {
            spdlog::error("**** DVBProcessor::initialize() **** Erreur lors de la définition du service par défaut: {}", e.what());
            
            AlertManager::getInstance().addAlert(
                AlertLevel::ERROR,
                "DVBProcessor",
                std::string("Erreur lors de la définition du service par défaut: ") + e.what(),
                true
            );
            
            throw;
        }
        
        spdlog::info("DVBProcessor initialisé avec succès");
    }
    catch (const ts::Exception& e) {
        spdlog::error("Erreur TSDuck lors de l'initialisation du DVBProcessor: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur TSDuck lors de l'initialisation: ") + e.what(),
            true
        );
        
        throw;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de l'initialisation du DVBProcessor: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur lors de l'initialisation: ") + e.what(),
            true
        );
        
        throw;
    }
}

void DVBProcessor::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Libérer les ressources
    pat_.reset();
    sdt_.reset();
    eit_.reset();
    nit_.reset();
    pmts_.clear();
    services_.clear();
}

std::vector<uint8_t> DVBProcessor::updatePSITables(const std::vector<uint8_t>& data, bool discontinuity) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Si les données sont vides, retourner directement
        if (data.empty() || !pat_ || !sdt_) {
            return data;
        }
        
        // Si c'est une discontinuité, mettre à jour les versions des tables
        if (discontinuity) {
            versionPAT_ = (versionPAT_ + 1) % 32;
            versionSDT_ = (versionSDT_ + 1) % 32;
            versionNIT_ = (versionNIT_ + 1) % 32;
            
            // Mettre à jour les numéros de version dans les tables
            pat_->version = versionPAT_;
            sdt_->version = versionSDT_;
            if (nit_) nit_->version = versionNIT_;
            
            // Mettre à jour les versions PMT
            for (auto& [serviceId, pmt] : pmts_) {
                versionPMT_[serviceId] = (versionPMT_[serviceId] + 1) % 32;
                pmt->version = versionPMT_[serviceId];
            }
            
            spdlog::info("Versions des tables PSI/SI incrémentées en raison d'une discontinuité");
        }
        
        // Vérifier que la taille est un multiple de TS_PACKET_SIZE
        if (data.size() % ts::PKT_SIZE != 0) {
            spdlog::warn("Taille de données non multiple de 188 octets: {}", data.size());
            return data;
        }
        
        // Analyser les PID dans le flux
        auto pids = analyzePIDs(data);
        
        // Générer les tables PSI/SI
        std::map<uint16_t, std::vector<uint8_t>> tables;
        
        // Générer la PAT (PID 0x0000)
        auto patData = generatePAT();
        if (!patData.empty()) {
            tables[0x0000] = patData;
        }
        
        // Générer la SDT (PID 0x0011)
        auto sdtData = generateSDT();
        if (!sdtData.empty()) {
            tables[0x0011] = sdtData;
        }
        
        // Générer la NIT (PID 0x0010)
        auto nitData = generateNIT();
        if (!nitData.empty()) {
            tables[0x0010] = nitData;
        }
        
        // Générer les PMT pour chaque service
        for (const auto& [serviceId, service] : services_) {
            auto pmtData = generatePMT(serviceId);
            if (!pmtData.empty()) {
                tables[service.pmtPid] = pmtData;
            }
        }
        
        // Insérer les tables dans le flux
        return insertTables(data, tables);
    }
    catch (const ts::Exception& e) {
        spdlog::error("Erreur TSDuck lors de la mise à jour des tables PSI/SI: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur TSDuck lors de la mise à jour des tables PSI/SI: ") + e.what(),
            true
        );
        
        return data;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de la mise à jour des tables PSI/SI: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur lors de la mise à jour des tables PSI/SI: ") + e.what(),
            true
        );
        
        return data;
    }
}

void DVBProcessor::setService(const DVBService& service) {
    spdlog::info("**** DVBProcessor::setService() **** (Publique) Début");
    std::lock_guard<std::mutex> lock(mutex_); // Verrouillage ici
    setServiceInternal(service); // Appel à la version interne
    spdlog::info("**** DVBProcessor::setService() **** (Publique) Fin");
}

void DVBProcessor::setServiceInternal(const DVBService& service) {
    spdlog::info("**** DVBProcessor::setServiceInternal() **** Début de la définition du service"); // Notez le changement de nom du log
    try {
        spdlog::info("**** DVBProcessor::setServiceInternal() **** Avant accès services_."); // LOG D1
        services_[service.serviceId] = service;
        spdlog::info("**** DVBProcessor::setServiceInternal() **** Service stocké. Service ID: {}", service.serviceId); // LOG D2

        spdlog::info("**** DVBProcessor::setServiceInternal() **** Avant accès pmts_.find."); // LOG E1
        auto it = pmts_.find(service.serviceId);
        spdlog::info("**** DVBProcessor::setServiceInternal() **** Recherche de la PMT pour le service ID: {} terminée.", service.serviceId); // LOG E2

        if (it == pmts_.end()) {
            spdlog::info("**** DVBProcessor::setServiceInternal() **** Avant création unique_ptr PMT."); // LOG F1
            pmts_[service.serviceId] = std::make_unique<ts::PMT>();
            spdlog::info("**** DVBProcessor::setServiceInternal() **** Après création unique_ptr PMT."); // LOG F2
            versionPMT_[service.serviceId] = 0;
            spdlog::info("**** DVBProcessor::setServiceInternal() **** Nouvelle PMT créée pour le service ID: {}", service.serviceId); // LOG F3
        }
        
        // Configurer la PMT
        ts::PMT* pmt = pmts_[service.serviceId].get();
        pmt->service_id = service.serviceId;
        pmt->version = versionPMT_[service.serviceId];
        pmt->is_current = true;
        
        spdlog::info("**** DVBProcessor::setServiceInternal() **** Nettoyage des streams existants");
        // Nettoyer les streams existants
        pmt->streams.clear();
        
        spdlog::info("**** DVBProcessor::setServiceInternal() **** Ajout des composants, nombre total: {}", service.components.size());
        
        // Vérifier s'il y a des composants vidéo pour le PCR
        bool hasPCR = false;
        uint16_t pcrPid = 0;
        
        // Créer un DuckContext temporaire
        ts::DuckContext duck;
        duck.report().setMaxSeverity(ts::Severity::Debug);
        
        // Ajouter les composants en utilisant l'API correcte de TSDuck 3.40
        for (const auto& [pid, streamType] : service.components) {
            spdlog::debug("**** DVBProcessor::setService() **** Ajout du composant PID: 0x{:X}, Type: 0x{:X}", pid, streamType);
            
            // Créer et configurer le stream
            ts::PMT::Stream& stream = pmt->streams[pid];
            stream.stream_type = streamType;
            
            // Si c'est une vidéo, utiliser son PID comme PCR PID
            if (streamType == 0x1B || streamType == 0x02 || streamType == 0x24) {
                if (!hasPCR) {
                    pcrPid = pid;
                    hasPCR = true;
                }
            }
            
            // Ajouter les descripteurs appropriés pour ce type de stream
            switch (streamType) {
                case 0x02: // MPEG-2 Video
                    try {
                        ts::VideoStreamDescriptor videoDesc;
                        videoDesc.frame_rate_code = 4; // 25 Hz pour PAL
                        videoDesc.chroma_format = 1; // 4:2:0
                        videoDesc.profile_and_level_indication = 0x85; // Main Profile @ Main Level
                        
                        spdlog::info("**** DVBProcessor::setService() **** Tentative d'ajout du descripteur MPEG-2 video pour PID 0x{:X}", pid);
                        stream.descs.add(duck, videoDesc);
                        spdlog::info("**** DVBProcessor::setService() **** Descripteur MPEG-2 video ajouté avec succès pour PID 0x{:X}", pid);
                    }
                    catch (const ts::Exception& e) {
                        spdlog::error("**** DVBProcessor::setService() ****  Erreur lors de l'ajout du descripteur MPEG-2 video: {}", e.what());
                    }
                    break;

                case 0x1B: // H.264 Video
                    try {
                        ts::AVCVideoDescriptor avcDesc;
                        avcDesc.profile_idc = 100; // High Profile
                        avcDesc.level_idc = 40; // Level 4.0
                        
                        spdlog::info("**** DVBProcessor::setService() **** Tentative d'ajout du descripteur H.264 video pour PID 0x{:X}", pid);
                        stream.descs.add(duck, avcDesc);
                        spdlog::info("**** DVBProcessor::setService() **** Descripteur H.264 video ajouté avec succès pour PID 0x{:X}", pid);
                    }
                    catch (const ts::Exception& e) {
                        spdlog::error("**** DVBProcessor::setService() ****  Erreur lors de l'ajout du descripteur H.264 video: {}", e.what());
                    }
                    break;

                case 0x24: // H.265/HEVC Video
                    try {
                        ts::HEVCVideoDescriptor hevcDesc;
                        // Configuration de base pour HEVC
                        
                        spdlog::info("**** DVBProcessor::setService() **** Tentative d'ajout du descripteur HEVC video pour PID 0x{:X}", pid);
                        stream.descs.add(duck, hevcDesc);
                        spdlog::info("**** DVBProcessor::setService() **** Descripteur HEVC video ajouté avec succès pour PID 0x{:X}", pid);
                    }
                    catch (const ts::Exception& e) {
                        spdlog::error("**** DVBProcessor::setService() ****  Erreur lors de l'ajout du descripteur HEVC video: {}", e.what());
                    }
                    break;
                case 0x03: // MPEG-1 Audio
                case 0x04: // MPEG-2 Audio
                case 0x0F: // AAC Audio
                case 0x11: // AAC with ADTS
                    // Pour les flux audio, ajouter un descripteur audio
                    try {
                        ts::AudioStreamDescriptor audioDesc;
                        audioDesc.free_format = false;
                        audioDesc.ID = true;
                        audioDesc.layer = 2;
                        spdlog::info("**** DVBProcessor::setService() **** Tentative d'ajout du descripteur audio pour PID 0x{:X}", pid);
                        stream.descs.add(duck, audioDesc);
                        spdlog::info("**** DVBProcessor::setService() **** Descripteur audio ajouté avec succès pour PID 0x{:X}", pid);
                    }
                    catch (const ts::Exception& e) {
                        spdlog::error("**** DVBProcessor::setService() ****  Erreur lors de l'ajout du descripteur audio: {}", e.what());
                    }
                    break;
                case 0x06: // Private data (souvent utilisé pour les sous-titres DVB)
                    // Descripteur d'application spécifique si nécessaire
                    break;
                default:
                    // Aucun descripteur spécial pour les autres types
                    break;
            }
            
            spdlog::debug("**** DVBProcessor::setService() **** Composant ajouté: PID: 0x{:X}, Type: 0x{:X}", pid, streamType);
        }
        
        // Définir le PCR PID si on en a trouvé un
        if (hasPCR) {
            pmt->pcr_pid = pcrPid;
        } else {
            // Utiliser le premier PID disponible comme fallback
            if (!service.components.empty()) {
                pmt->pcr_pid = service.components.begin()->first;
            } else {
                pmt->pcr_pid = 0x1FFF; // Valeur invalide
            }
        }
        // Vérifier si la PMT est valide
        if (!pmt->isValid()) {
            spdlog::error("PMT configurée non valide selon les standards DVB");
            // Log des détails de la PMT pour déboguer
            spdlog::info("PMT service_id: {}, PCR PID: 0x{:X}, Nombre de streams: {}",
                        pmt->service_id, pmt->pcr_pid, pmt->streams.size());
        }
        
        spdlog::info("Service configuré: ID={}, PMT PID=0x{:X}, {} composants", 
                   service.serviceId, service.pmtPid, service.components.size());
    }
    catch (const ts::Exception& e) {
        spdlog::error("**** DVBProcessor::setService() **** Exception TSDuck lors de la configuration du service: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur TSDuck lors de la configuration du service: ") + e.what(),
            true
        );
        
        throw;
    }
    catch (const std::exception& e) {
        spdlog::error("**** DVBProcessor::setService() **** Exception lors de la configuration du service: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur lors de la configuration du service: ") + e.what(),
            true
        );
        
        throw;
    }
    catch (...) {
        spdlog::error("**** DVBProcessor::setService() **** Exception inconnue lors de la configuration du service");
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            "Exception inconnue lors de la configuration du service",
            true
        );
        
        throw;
    }
    spdlog::info("**** DVBProcessor::setService() **** Fin de la définition du service");
}

bool DVBProcessor::removeService(uint16_t serviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Vérifier si le service existe
    auto it = services_.find(serviceId);
    if (it == services_.end()) {
        return false;
    }
    
    // Supprimer le service
    services_.erase(it);
    
    // Supprimer la PMT associée
    pmts_.erase(serviceId);
    versionPMT_.erase(serviceId);
    
    spdlog::info("Service supprimé: ID={}", serviceId);
    
    return true;
}

std::vector<DVBService> DVBProcessor::getServices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<DVBService> result;
    result.reserve(services_.size());
    
    for (const auto& [id, service] : services_) {
        result.push_back(service);
    }
    
    return result;
}

std::vector<uint8_t> DVBProcessor::generatePAT() {
    if (!pat_) return {};
    
    try {
        // Mettre à jour la PAT
        pat_->version = versionPAT_;
        pat_->pmts.clear();
        
        // Ajouter tous les services à la PAT
        for (const auto& [serviceId, service] : services_) {
            pat_->pmts[serviceId] = service.pmtPid;
        }
        
        // Créer un DuckContext temporaire
        ts::DuckContext duck;

        
        // Sérialiser la PAT en BinaryTable
        ts::BinaryTable binTable;
        if (!pat_->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la PAT");
            return {};
        }
        
        // Utiliser TSPacketizer pour une conversion propre en paquets
        // Correction: Utiliser ts::PID(0x00) au lieu de 0x00 directement
        ts::CyclingPacketizer pzer(duck, ts::PID(0x00)); // PID 0x00 pour PAT
        pzer.addTable(binTable);
        
        std::vector<uint8_t> result;
        ts::TSPacket packet;
        while (pzer.getNextPacket(packet)) {
            result.insert(result.end(), packet.b, packet.b + ts::PKT_SIZE);
        }
        
        return result;
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors de la génération de la PAT: {}", e.what());
        return {};
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la PAT: {}", e.what());
        return {};
    }
}


std::vector<uint8_t> DVBProcessor::generatePMT(uint16_t serviceId) {
    // Vérifier si le service existe
    auto serviceIt = services_.find(serviceId);
    if (serviceIt == services_.end()) {
        return {};
    }
    
    // Vérifier si la PMT existe
    auto pmtIt = pmts_.find(serviceId);
    if (pmtIt == pmts_.end()) {
        return {};
    }
    
    const auto& service = serviceIt->second;
    ts::PMT* pmt = pmtIt->second.get();
    
    try {
        // Mettre à jour la PMT
        pmt->service_id = serviceId;
        pmt->version = versionPMT_[serviceId];
        pmt->is_current = true;
        
        // Vérifier s'il y a des composants vidéo ou audio
        bool hasPCR = false;
        uint16_t pcrPid = 0;
        
        // Vérifier les streams existants pour trouver un PID pour le PCR
        for (const auto& [pid, stream] : pmt->streams) {
            if (stream.stream_type == 0x1B || stream.stream_type == 0x02 || stream.stream_type == 0x24) {
                if (!hasPCR) {
                    pcrPid = pid;
                    hasPCR = true;
                }
            }
        }
        
        // Définir le PCR PID si on en a trouvé un
        if (hasPCR) {
            pmt->pcr_pid = pcrPid;
        } else {
            // Utiliser le premier PID disponible comme fallback
            if (!pmt->streams.empty()) {
                pmt->pcr_pid = pmt->streams.begin()->first;
            } else {
                pmt->pcr_pid = 0x1FFF; // Valeur invalide
            }
        }
        
        // Créer un DuckContext temporaire
        ts::DuckContext duck;


        // Sérialiser la PMT en BinaryTable
        ts::BinaryTable binTable;
        if (!pmt->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la PMT");
            return {};
        }
        
        // Utiliser TSPacketizer pour une conversion propre en paquets
        ts::CyclingPacketizer pzer(duck, ts::PID(service.pmtPid));
        pzer.addTable(binTable);
        
        std::vector<uint8_t> result;
        ts::TSPacket packet;
        while (pzer.getNextPacket(packet)) {
            result.insert(result.end(), packet.b, packet.b + ts::PKT_SIZE);
        }
        
        return result;
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors de la génération de la PMT: {}", e.what());
        return {};
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la PMT: {}", e.what());
        return {};
    }
}

std::vector<uint8_t> DVBProcessor::generateSDT() {
    if (!sdt_) {
        return {};
    }
    
    try {
        // Mettre à jour la SDT
        sdt_->version = versionSDT_;
        sdt_->services.clear();
        
        // Créer un DuckContext temporaire
        ts::DuckContext duck;

        
        // Ajouter tous les services à la SDT en utilisant l'API correcte de TSDuck 3.40
        for (const auto& [serviceId, service] : services_) {
            // Dans TSDuck 3.40, on accède directement à la map services
            auto& sdtServiceEntry = sdt_->services[serviceId];
            
            // Configuration des flags standard
            sdtServiceEntry.running_status = 4; // running
            sdtServiceEntry.CA_controlled = false;
            
            // Utiliser ServiceDescriptor
            ts::ServiceDescriptor desc;
            desc.service_type = service.serviceType;
            
            // Convertir les chaînes std::string en UString
            ts::UString providerName;
            ts::UString serviceName;
            providerName.assignFromUTF8(service.provider);
            serviceName.assignFromUTF8(service.name);
            
            // Utiliser les noms de propriétés corrects pour TSDuck 3.40
            desc.provider_name = providerName;
            desc.service_name = serviceName;
            
            // Ajouter le descripteur à la SDT
            sdtServiceEntry.descs.add(duck, desc);
        }
        
        // Sérialiser la SDT en BinaryTable
        ts::BinaryTable binTable;
        if (!sdt_->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la SDT");
            return {};
        }
        
        // Utiliser TSPacketizer pour une conversion propre en paquets
        // Correction: Utiliser ts::PID(0x11) au lieu de relire service.pmtPid
        ts::CyclingPacketizer pzer(duck, ts::PID(0x11)); // PID 0x11 pour SDT
        pzer.addTable(binTable);
        
        std::vector<uint8_t> result;
        ts::TSPacket packet;
        while (pzer.getNextPacket(packet)) {
            result.insert(result.end(), packet.b, packet.b + ts::PKT_SIZE);
        }
        
        return result;
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors de la génération de la SDT: {}", e.what());
        return {};
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la SDT: {}", e.what());
        return {};
    }
}


std::vector<uint8_t> DVBProcessor::generateEIT() {
    // Pour simplifier, nous n'allons pas générer d'EIT
    return {};
}

std::vector<uint8_t> DVBProcessor::generateNIT() {
    if (!nit_) {
        return {};
    }
    
    try {
        // Mettre à jour la NIT
        nit_->version = versionNIT_;
        nit_->transports.clear();
        
        // Créer un DuckContext temporaire
        ts::DuckContext duck;

        
        // Pour TSDuck 3.40, ajouter un transport de façon compatible
        uint16_t tsid = 1;
        uint16_t onid = 1;
        // Utiliser TransportStreamId comme clé dans la map des transports
        ts::TransportStreamId tsKey(tsid, onid);
        // Obtenir une référence à la nouvelle entrée (qui est automatiquement créée)
        auto& transport = nit_->transports[tsKey];
        
        // Ajouter un descripteur de service list
        ts::ServiceListDescriptor sld;
        
        for (const auto& [serviceId, service] : services_) {
            sld.entries.push_back(ts::ServiceListDescriptor::Entry(serviceId, service.serviceType));
        }
        
        // Ajouter le descripteur
        transport.descs.add(duck, sld);
        
        // Ajouter un descripteur de réseau
        ts::NetworkNameDescriptor netName;
        // Correction: Utiliser la propriété correcte pour TSDuck 3.40
        netName.name.assignFromUTF8("HLS to DVB Network");
        nit_->descs.add(duck, netName);
        
        // Sérialiser la NIT en BinaryTable
        ts::BinaryTable binTable;
        if (!nit_->serialize(duck, binTable)) {
            spdlog::error("Erreur lors de la sérialisation de la NIT");
            return {};
        }
        
        // Utiliser TSPacketizer pour une conversion propre en paquets
        // Correction: Utiliser ts::PID(0x10) au lieu de relire service.pmtPid
        ts::CyclingPacketizer pzer(duck, ts::PID(0x10)); // PID 0x10 pour NIT
        pzer.addTable(binTable);
        
        std::vector<uint8_t> result;
        ts::TSPacket packet;
        while (pzer.getNextPacket(packet)) {
            result.insert(result.end(), packet.b, packet.b + ts::PKT_SIZE);
        }
        
        return result;
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors de la génération de la NIT: {}", e.what());
        return {};
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de la génération de la NIT: {}", e.what());
        return {};
    }
}


std::map<uint16_t, uint8_t> DVBProcessor::analyzePIDs(const std::vector<uint8_t>& data) {
    std::map<uint16_t, uint8_t> pidTypes;
    spdlog::error("**** DVBProcessor::analyzePIDs() **** Début de l'analyse des PIDs");

    // Si nous avons déjà des services configurés, mettre à jour les composants
    if (!services_.empty()) {
        spdlog::debug("Services déjà configurés, utilisation des composants existants");
        return pidTypes;
    }
    
    // Utiliser TSAnalyzer de TSDuck pour une analyse plus robuste
    try {
        // Convertir les données en paquets TS
        std::set<uint16_t> videoPids;
        std::set<uint16_t> audioPids;
        std::set<uint16_t> otherPids;
        std::set<uint16_t> pcrPids;
        
        // Collection de statistiques pour chaque PID
        std::map<uint16_t, size_t> pidCount;
        std::map<uint16_t, uint8_t> pidStreamType;
        
        // Analyser chaque paquet pour détecter les PID intéressants
        size_t packetCount = data.size() / ts::PKT_SIZE;
        for (size_t i = 0; i < packetCount; ++i) {
            const uint8_t* packetData = &data[i * ts::PKT_SIZE];
            ts::TSPacket packet;
            std::memcpy(packet.b, packetData, ts::PKT_SIZE);
            
            // Extraire le PID
            uint16_t pid = packet.getPID();
            
            // Ignorer les PID réservés et PSI standard
            if (pid <= 0x1F || 
                pid == 0x0000 || pid == 0x0001 || 
                pid == 0x0010 || pid == 0x0011 || 
                pid == 0x0012 || pid == 0x0014 ||
                pid >= 0x1FFF) {
                continue;
            }
            
            // Compter les occurrences de ce PID
            pidCount[pid]++;
            
            // Vérifier si c'est un paquet avec PCR
            if (packet.hasPCR()) {
                pcrPids.insert(pid);
            }
        }
        
        // Identifier les types de streams basés sur la fréquence et les PCR
        for (const auto& [pid, count] : pidCount) {
            // Si le PID a un PCR, c'est probablement de la vidéo
            if (pcrPids.find(pid) != pcrPids.end()) {
                videoPids.insert(pid);
                pidStreamType[pid] = 0x1B; // H.264 par défaut
            }
            // Si le PID est fréquent mais n'a pas de PCR, c'est probablement de l'audio
            else if (count > packetCount / 20) { // Si le PID représente plus de 5% des paquets
                audioPids.insert(pid);
                pidStreamType[pid] = 0x03; // MPEG Audio par défaut
            }
            // Sinon, c'est un autre type
            else {
                otherPids.insert(pid);
                pidStreamType[pid] = 0x06; // Données privées
            }
        }
        
        // S'il n'y a pas de service configuré et qu'on trouve des PID, créer un service par défaut
        if (services_.empty() && (!videoPids.empty() || !audioPids.empty() || !otherPids.empty())) {
            DVBService service;
            service.serviceId = 1;
            service.pmtPid = 0x1000;  // Valeur arbitraire pour la PMT
            service.name = "Service HLS";
            service.provider = "HLS to DVB Converter";
            service.serviceType = 0x01;  // TV numérique
            
            // Ajouter les PID vidéo comme composants
            for (uint16_t pid : videoPids) {
                service.components[pid] = 0x1B;  // H.264 video
                pidTypes[pid] = 0x1B;
                spdlog::info("PID vidéo détecté: 0x{:04X}", pid);
            }
            
            // Ajouter les PID audio comme composants
            for (uint16_t pid : audioPids) {
                service.components[pid] = 0x03;  // MPEG audio
                pidTypes[pid] = 0x03;
                spdlog::info("PID audio détecté: 0x{:04X}", pid);
            }
            
            // Ajouter les autres PID comme composants (données privées)
            for (uint16_t pid : otherPids) {
                service.components[pid] = 0x06;  // Données privées
                pidTypes[pid] = 0x06;
                spdlog::info("Autre PID détecté: 0x{:04X}", pid);
            }
            
            // Si aucun PID n'a été détecté, ajouter un composant générique
            if (service.components.empty()) {
                uint16_t genericPid = 0x1001;
                service.components[genericPid] = 0x1B;  // Vidéo générique
                pidTypes[genericPid] = 0x1B;
                spdlog::warn("Aucun PID détecté, ajout d'un composant générique: 0x{:04X}", genericPid);
            }
            //try {
                // Tentative d'ajouter service par défaut
                //setService(service);
            //}
            //catch (const std::exception& e) {
            //    spdlog::error("Exception capturée lors de l'initialisation du service par défaut: {}", e.what());
            //    // Continuez sans service par défaut
            //}
            spdlog::info("Analyse des PIDs terminée avec {} composants", service.components.size());
        }
        
        return pidTypes;
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors de l'analyse des PID: {}", e.what());
        return pidTypes;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de l'analyse des PID: {}", e.what());
        return pidTypes;
    }
}

std::vector<uint8_t> DVBProcessor::insertTables(const std::vector<uint8_t>& data, 
                                              const std::map<uint16_t, std::vector<uint8_t>>& tables) {
    // Si aucune table à insérer, retourner les données originales
    if (tables.empty()) {
        return data;
    }
    
    try {
        // Utiliser les fonctionnalités de TSDuck pour un traitement plus robuste
        ts::TSPacketVector packets;
        
        // Convertir les données en paquets TS
        size_t packetCount = data.size() / ts::PKT_SIZE;
        packets.reserve(packetCount);
        
        // Identifier les PID des tables à remplacer
        std::set<uint16_t> tablePids;
        for (const auto& [pid, _] : tables) {
            tablePids.insert(pid);
        }
        
        // Charger les paquets tout en filtrant les tables PSI existantes
        for (size_t i = 0; i < packetCount; ++i) {
            ts::TSPacket packet;
            std::memcpy(packet.b, &data[i * ts::PKT_SIZE], ts::PKT_SIZE);
            
            uint16_t pid = packet.getPID();
            
            // Ne pas ajouter les paquets avec les PID des tables qu'on va insérer
            if (tablePids.find(pid) == tablePids.end()) {
                packets.push_back(packet);
            }
        }
        
        // Ordre d'insertion des tables PSI standard
        const std::vector<uint16_t> psiOrder = {0x0000, 0x0010, 0x0011, 0x0012};
        
        // Préparer les tables PSI dans le bon ordre
        ts::TSPacketVector psiPackets;
        
        // D'abord ajouter les tables PSI standard
        for (uint16_t pid : psiOrder) {
            auto it = tables.find(pid);
            if (it != tables.end() && !it->second.empty()) {
                // Convertir ces données en paquets
                size_t tablePacketCount = it->second.size() / ts::PKT_SIZE;
                for (size_t i = 0; i < tablePacketCount; ++i) {
                    ts::TSPacket packet;
                    std::memcpy(packet.b, &it->second[i * ts::PKT_SIZE], ts::PKT_SIZE);
                    psiPackets.push_back(packet);
                }
            }
        }
        
        // Puis ajouter les autres tables (PMT, etc.)
        for (const auto& [pid, tableData] : tables) {
            // Ignorer les PID déjà traités
            if (pid == 0x0000 || pid == 0x0010 || pid == 0x0011 || pid == 0x0012) {
                continue;
            }
            
            if (!tableData.empty()) {
                size_t tablePacketCount = tableData.size() / ts::PKT_SIZE;
                for (size_t i = 0; i < tablePacketCount; ++i) {
                    ts::TSPacket packet;
                    std::memcpy(packet.b, &tableData[i * ts::PKT_SIZE], ts::PKT_SIZE);
                    psiPackets.push_back(packet);
                }
            }
        }
        
        // Utiliser TSMerger pour intégrer les tables PSI de manière optimale
        ts::TSPacketVector resultPackets;
        resultPackets.reserve(packets.size() + psiPackets.size());
        
        // Insérer les tables PSI au début du flux
        resultPackets.insert(resultPackets.end(), psiPackets.begin(), psiPackets.end());
        
        // Calculer le rapport d'insertion pour répéter les tables
        size_t insertionRatio = packets.size() / (psiPackets.size() * 2);
        if (insertionRatio < 50) insertionRatio = 50; // Au moins tous les 50 paquets
        
        // Ajouter les paquets originaux et répéter les tables PSI selon le ratio
        for (size_t i = 0; i < packets.size(); ++i) {
            resultPackets.push_back(packets[i]);
            
            // Tous les 'insertionRatio' paquets, ajouter à nouveau les tables importantes
            if (i > 0 && i % insertionRatio == 0) {
                // Ajouter seulement PAT et PMT pour ne pas trop surcharger
                for (uint16_t pid : {(uint16_t)0x0000}) {
                    auto it = tables.find(pid);
                    if (it != tables.end() && !it->second.empty()) {
                        // Ajouter seulement le premier paquet de chaque table
                        ts::TSPacket packet;
                        std::memcpy(packet.b, &it->second[0], ts::PKT_SIZE);
                        resultPackets.push_back(packet);
                    }
                }
                
                // Ajouter une PMT pour chaque service
                for (const auto& [serviceId, service] : services_) {
                    auto it = tables.find(service.pmtPid);
                    if (it != tables.end() && !it->second.empty()) {
                        // Ajouter seulement le premier paquet de la PMT
                        ts::TSPacket packet;
                        std::memcpy(packet.b, &it->second[0], ts::PKT_SIZE);
                        resultPackets.push_back(packet);
                    }
                }
            }
        }
        
        // Convertir les paquets en vecteur d'octets
        std::vector<uint8_t> result;
        result.reserve(resultPackets.size() * ts::PKT_SIZE);
        
        for (const auto& packet : resultPackets) {
            result.insert(result.end(), packet.b, packet.b + ts::PKT_SIZE);
        }
        
        return result;
    }
    catch (const ts::Exception& e) {
        spdlog::error("Exception TSDuck lors de l'insertion des tables: {}", e.what());
        return data;
    }
    catch (const std::exception& e) {
        spdlog::error("Exception lors de l'insertion des tables: {}", e.what());
        return data;
    }
}

DVBProcessor::~DVBProcessor() {
    cleanup();
}
        