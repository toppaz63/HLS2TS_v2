#include "DVBProcessor.h"
#include "../alerting/AlertManager.h"
#include "spdlog/spdlog.h"

// Utilisation de TSDuck pour manipuler les tables DVB
#include <tsduck/tsduck.h>

// Constantes DVB
constexpr uint16_t PID_PAT = 0x0000;      // Program Association Table
constexpr uint16_t PID_CAT = 0x0001;      // Conditional Access Table
constexpr uint16_t PID_TSDT = 0x0002;     // Transport Stream Description Table
constexpr uint16_t PID_NIT = 0x0010;      // Network Information Table
constexpr uint16_t PID_SDT = 0x0011;      // Service Description Table
constexpr uint16_t PID_EIT = 0x0012;      // Event Information Table
constexpr uint16_t PID_TDT = 0x0014;      // Time & Date Table

// Types de flux
constexpr uint8_t STREAM_TYPE_VIDEO_MPEG2 = 0x02;  // Vidéo MPEG-2
constexpr uint8_t STREAM_TYPE_AUDIO_MPEG1 = 0x03;  // Audio MPEG-1
constexpr uint8_t STREAM_TYPE_AUDIO_MPEG2 = 0x04;  // Audio MPEG-2
constexpr uint8_t STREAM_TYPE_PRIVATE = 0x06;      // Données privées
constexpr uint8_t STREAM_TYPE_AUDIO_AAC = 0x0F;    // Audio AAC
constexpr uint8_t STREAM_TYPE_VIDEO_H264 = 0x1B;   // Vidéo H.264
constexpr uint8_t STREAM_TYPE_VIDEO_HEVC = 0x24;   // Vidéo H.265

// Types de service
constexpr uint8_t SERVICE_TYPE_TV = 0x01;          // TV numérique
constexpr uint8_t SERVICE_TYPE_RADIO = 0x02;       // Radio numérique

DVBProcessor::DVBProcessor()
    : versionPAT_(0), versionSDT_(0), versionEIT_(0), versionNIT_(0) {
}

void DVBProcessor::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Initialiser les tables PSI/SI
        pat_ = std::make_unique<ts::PAT>(PID_PAT);
        sdt_ = std::make_unique<ts::SDT>(PID_SDT);
        eit_ = std::make_unique<ts::EIT>(PID_EIT);
        nit_ = std::make_unique<ts::NIT>(PID_NIT);
        
        // Configurer les identifiants de réseau/transport
        if (nit_) {
            nit_->setNetworkId(1);  // ID de réseau
        }
        
        // Initialiser les versions des tables
        versionPAT_ = 0;
        versionSDT_ = 0;
        versionEIT_ = 0;
        versionNIT_ = 0;
        
        spdlog::info("Processeur DVB initialisé avec succès");
    }
    catch (const ts::Exception& e) {
        spdlog::error("Erreur lors de l'initialisation du processeur DVB (TSDuck): {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur lors de l'initialisation du processeur DVB (TSDuck): ") + e.what(),
            true
        );
        
        throw std::runtime_error("Erreur lors de l'initialisation du processeur DVB");
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de l'initialisation du processeur DVB: {}", e.what());
        
        AlertManager::getInstance().addAlert(
            AlertLevel::ERROR,
            "DVBProcessor",
            std::string("Erreur lors de l'initialisation du processeur DVB: ") + e.what(),
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
    versionPMT_.clear();
    
    spdlog::info("Ressources du processeur DVB libérées");
}

std::vector<uint8_t> DVBProcessor::updatePSITables(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Analyser les PID pour détecter les flux audio/vidéo
        auto pidTypes = analyzePIDs(data);
        
        // Si aucun service n'est configuré, créer un service par défaut
        if (services_.empty()) {
            DVBService defaultService;
            defaultService.serviceId = 1;
            defaultService.pmtPid = 0x1000;  // PID arbitraire pour la PMT
            defaultService.name = "HLS2TS Service";
            defaultService.provider = "HLS2TS";
            defaultService.serviceType = SERVICE_TYPE_TV;
            
            // Ajouter les composants détectés
            defaultService.components = pidTypes;
            
            // Configurer le service
            setService(defaultService);
        }
        
        // Générer les tables PSI/SI
        std::map<uint16_t, std::vector<uint8_t>> tables;
        
        // PAT
        tables[PID_PAT] = generatePAT();
        
        // PMT pour chaque service
        for (const auto& [serviceId, service] : services_) {
            tables[service.pmtPid] = generatePMT(serviceId);
        }
        
        // SDT
        tables[PID_SDT] = generateSDT();
        
        // EIT
        tables[PID_EIT] = generateEIT();
        
        // NIT
        tables[PID_NIT] = generateNIT();
        
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
        
        // En cas d'erreur, retourner les données d'origine
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
        
        // En cas d'erreur, retourner les données d'origine
        return data;
    }
}

void DVBProcessor::setService(const DVBService& service) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Ajouter ou mettre à jour le service
    services_[service.serviceId] = service;
    
    // Initialiser la version de la PMT si nécessaire
    if (versionPMT_.find(service.serviceId) == versionPMT_.end()) {
        versionPMT_[service.serviceId] = 0;
    }
    
    // Incrémenter les versions des tables
    versionPAT_ = (versionPAT_ + 1) % 32;
    versionSDT_ = (versionSDT_ + 1) % 32;
    
    spdlog::info("Service DVB configuré: id={}, nom=\"{}\"", service.serviceId, service.name);
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
    
    // Incrémenter les versions des tables
    versionPAT_ = (versionPAT_ + 1) % 32;
    versionSDT_ = (versionSDT_ + 1) % 32;
    
    spdlog::info("Service DVB supprimé: id={}", serviceId);
    
    return true;
}

std::vector<DVBService> DVBProcessor::getServices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<DVBService> result;
    result.reserve(services_.size());
    
    for (const auto& [serviceId, service] : services_) {
        result.push_back(service);
    }
    
    return result;
}

std::vector<uint8_t> DVBProcessor::generatePAT() {
    if (!pat_) {
        return {};
    }
    
    // Réinitialiser la PAT
    pat_->clear();
    
    // Configurer les services dans la PAT
    for (const auto& [serviceId, service] : services_) {
        pat_->addProgram(serviceId, service.pmtPid);
    }
    
    // Définir le transport stream ID
    pat_->setTransportStreamId(1);
    
    // Définir la version
    pat_->setVersion(versionPAT_);
    
    // Sérialiser la PAT
    ts::ByteBlock block;
    pat_->serialize(block);
    
    return std::vector<uint8_t>(block.begin(), block.end());
}

std::vector<uint8_t> DVBProcessor::generatePMT(uint16_t serviceId) {
    // Vérifier si le service existe
    auto serviceIt = services_.find(serviceId);
    if (serviceIt == services_.end()) {
        return {};
    }
    
    const DVBService& service = serviceIt->second;
    
    // Créer ou récupérer la PMT
    auto& pmt = pmts_[serviceId];
    if (!pmt) {
        pmt = std::make_unique<ts::PMT>(service.pmtPid, serviceId);
    }
    else {
        pmt->clear();
        pmt->setPID(service.pmtPid);
        pmt->setServiceId(serviceId);
    }
    
    // Configurer la version
    pmt->setVersion(versionPMT_[serviceId]);
    
    // Ajouter les composants
    for (const auto& [pid, streamType] : service.components) {
        pmt->addComponent(pid, streamType);
    }
    
    // Sérialiser la PMT
    ts::ByteBlock block;
    pmt->serialize(block);
    
    return std::vector<uint8_t>(block.begin(), block.end());
}

std::vector<uint8_t> DVBProcessor::generateSDT() {
    if (!sdt_) {
        return {};
    }
    
    // Réinitialiser la SDT
    sdt_->clear();
    
    // Définir le transport stream ID
    sdt_->setTransportStreamId(1);
    
    // Définir l'original network ID
    sdt_->setOriginalNetworkId(1);
    
    // Définir la version
    sdt_->setVersion(versionSDT_);
    
    // Ajouter les services
    for (const auto& [serviceId, service] : services_) {
        ts::SDT::Service sdtService;
        
        sdtService.setServiceId(serviceId);
        sdtService.setRunningStatus(ts::RunningStatus::RUNNING);
        sdtService.setCAControlled(false);
        sdtService.setEITScheduleFlag(false);
        sdtService.setEITPresentFollowingFlag(true);
        
        ts::DescriptorList& descriptors = sdtService.descs;
        
        // Ajouter le descripteur de service
        ts::ServiceDescriptor serviceDesc;
        serviceDesc.setServiceType(service.serviceType);
        serviceDesc.setProviderName(service.provider);
        serviceDesc.setServiceName(service.name);
        
        descriptors.add(serviceDesc);
        
        sdt_->addService(sdtService);
    }
    
    // Sérialiser la SDT
    ts::ByteBlock block;
    sdt_->serialize(block);
    
    return std::vector<uint8_t>(block.begin(), block.end());
}

std::vector<uint8_t> DVBProcessor::generateEIT() {
    if (!eit_) {
        return {};
    }
    
    // Réinitialiser la EIT
    eit_->clear();
    
    // Définir les paramètres de la EIT
    eit_->setServiceId(1);  // ID du premier service
    eit_->setTransportStreamId(1);
    eit_->setOriginalNetworkId(1);
    eit_->setTableId(ts::TID_EIT_PF_ACT);  // EIT present/following actual
    eit_->setVersion(versionEIT_);
    
    // Ajouter un événement "en cours" (présent)
    ts::EIT::Event event;
    event.setEventId(1);
    
    // Horodatage actuel pour le début de l'événement
    ts::Time startTime = ts::Time::CurrentUTC();
    event.setStartTime(startTime);
    
    // Durée de l'événement: 2 heures
    event.setDuration(ts::Time::Hours(2));
    
    // Définir les descripteurs de l'événement
    ts::DescriptorList& descriptors = event.descs;
    
    // Descripteur de court événement (nom et description)
    ts::ShortEventDescriptor shortEvent;
    shortEvent.setLanguageCode("fre");  // Français
    shortEvent.setEventName("HLS2TS Stream");
    shortEvent.setText("Flux converti par HLS2TS");
    descriptors.add(shortEvent);
    
    // Ajouter l'événement à la EIT
    eit_->addEvent(event);
    
    // Sérialiser la EIT
    ts::ByteBlock block;
    eit_->serialize(block);
    
    return std::vector<uint8_t>(block.begin(), block.end());
}

std::vector<uint8_t> DVBProcessor::generateNIT() {
    if (!nit_) {
        return {};
    }
    
    // Réinitialiser la NIT
    nit_->clear();
    
    // Définir les paramètres de la NIT
    nit_->setNetworkId(1);
    nit_->setVersion(versionNIT_);
    
    // Ajouter un descripteur de réseau (nom)
    ts::DescriptorList& networkDescriptors = nit_->networkDesc;
    
    ts::NetworkNameDescriptor networkName;
    networkName.setName("HLS2TS Network");
    networkDescriptors.add(networkName);
    
    // Ajouter un transport stream
    ts::NIT::TransportStream ts;
    ts.setTransportStreamId(1);
    ts.setOriginalNetworkId(1);
    
    // Ajouter des descripteurs au transport stream
    ts::DescriptorList& tsDescriptors = ts.descs;
    
    // Descripteur de livraison par satellite (exemple)
    ts::SatelliteDeliverySystemDescriptor satellite;
    satellite.setFrequency(11000);  // 11 GHz
    satellite.setOrbitalPosition(130);  // 13.0° Est
    satellite.setWestEastFlag(true);  // Est
    satellite.setPolarization(ts::SatelliteDeliverySystemDescriptor::Polarization::HORIZONTAL);
    satellite.setRollOff(ts::SatelliteDeliverySystemDescriptor::RollOff::ROLLOFF_35);
    satellite.setModulationType(ts::SatelliteDeliverySystemDescriptor::Modulation::M8PSK);
    satellite.setSymbolRate(30000);  // 30 Msymbol/s
    satellite.setFECInner(ts::SatelliteDeliverySystemDescriptor::InnerFEC::FEC_3_4);
    
    tsDescriptors.add(satellite);
    
    // Ajouter le transport stream à la NIT
    nit_->addTransportStream(ts);
    
    // Sérialiser la NIT
    ts::ByteBlock block;
    nit_->serialize(block);
    
    return std::vector<uint8_t>(block.begin(), block.end());
}

std::map<uint16_t, uint8_t> DVBProcessor::analyzePIDs(const std::vector<uint8_t>& data) {
    std::map<uint16_t, uint8_t> result;
    
    try {
        // Convertir les données en paquets TS
        size_t packetCount = data.size() / ts::PKT_SIZE;
        ts::TSPacketVector packets;
        
        for (size_t i = 0; i < packetCount; ++i) {
            ts::TSPacket packet;
            std::memcpy(packet.b, &data[i * ts::PKT_SIZE], ts::PKT_SIZE);
            packets.push_back(packet);
        }
        
        // Analyser les paquets pour trouver les PID
        std::set<uint16_t> pidSet;
        for (const auto& packet : packets) {
            uint16_t pid = packet.getPID();
            
            // Ignorer les PID réservés ou ceux des tables PSI/SI
            if (pid <= 0x1F || pid == PID_PAT || pid == PID_CAT || pid == PID_NIT || 
                pid == PID_SDT || pid == PID_EIT || pid == PID_TDT) {
                continue;
            }
            
            pidSet.insert(pid);
        }
        
        // Tenter de déterminer le type de flux pour chaque PID
        for (uint16_t pid : pidSet) {
            // Par défaut, considérer comme données privées
            uint8_t streamType = STREAM_TYPE_PRIVATE;
            
            // Avec une analyse plus avancée, on pourrait déterminer le type de flux
            // en analysant les paquets PES ou en utilisant l'information des PMT existantes
            // Pour l'instant, nous utilisons une approche simplifiée basée sur les plages de PID
            
            if (pid >= 0x0100 && pid <= 0x01FF) {
                streamType = STREAM_TYPE_VIDEO_H264;  // Supposer vidéo H.264
            }
            else if (pid >= 0x0200 && pid <= 0x02FF) {
                streamType = STREAM_TYPE_AUDIO_AAC;   // Supposer audio AAC
            }
            
            result[pid] = streamType;
        }
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de l'analyse des PID: {}", e.what());
    }
    
    return result;
}

std::vector<uint8_t> DVBProcessor::insertTables(const std::vector<uint8_t>& data, 
                                              const std::map<uint16_t, std::vector<uint8_t>>& tables) {
    // Si aucune table à insérer ou pas de données d'entrée, retourner les données d'origine
    if (tables.empty() || data.empty()) {
        return data;
    }
    
    try {
        // Convertir les données en paquets TS
        size_t packetCount = data.size() / ts::PKT_SIZE;
        std::vector<ts::TSPacket> packets;
        packets.reserve(packetCount);
        
        for (size_t i = 0; i < packetCount; ++i) {
            ts::TSPacket packet;
            std::memcpy(packet.b, &data[i * ts::PKT_SIZE], ts::PKT_SIZE);
            packets.push_back(packet);
        }
        
        // Créer une section pour chaque table
        std::map<uint16_t, ts::SectionPtrVector> sections;
        
        for (const auto& [pid, tableData] : tables) {
            if (tableData.empty()) {
                continue;
            }
            
            // Créer une section à partir des données
            ts::SectionPtr section = ts::Section::FromBinary(tableData);
            if (section.isNull()) {
                spdlog::error("Erreur lors de la création de la section pour PID 0x{:04X}", pid);
                continue;
            }
            
            // Ajouter la section
            sections[pid].push_back(section);
        }
        
        // Insérer les sections dans les paquets TS
        ts::TSPacketVector resultPackets;
        resultPackets.reserve(packets.size() + 100);  // Réserver de l'espace supplémentaire pour les nouvelles tables
        
        // Compteurs pour la périodicité des tables
        std::map<uint16_t, size_t> tableCounters;
        
        // Périodicité d'insertion des tables (en paquets)
        constexpr size_t PAT_PERIOD = 50;    // PAT toutes les 50 paquets
        constexpr size_t PMT_PERIOD = 100;   // PMT toutes les 100 paquets
        constexpr size_t SDT_PERIOD = 200;   // SDT toutes les 200 paquets
        constexpr size_t EIT_PERIOD = 300;   // EIT toutes les 300 paquets
        constexpr size_t NIT_PERIOD = 500;   // NIT toutes les 500 paquets
        
        // Définir la périodicité pour chaque PID
        tableCounters[PID_PAT] = 0;
        for (const auto& [serviceId, service] : services_) {
            tableCounters[service.pmtPid] = 20;  // Décaler légèrement les PMT
        }
        tableCounters[PID_SDT] = 40;
        tableCounters[PID_EIT] = 60;
        tableCounters[PID_NIT] = 80;
        
        // Insérer les paquets en ajoutant les tables périodiquement
        for (const auto& packet : packets) {
            // Si c'est un paquet d'une table PSI/SI que nous remplaçons, l'ignorer
            uint16_t pid = packet.getPID();
            if (tables.find(pid) != tables.end()) {
                continue;
            }
            
            // Ajouter le paquet original
            resultPackets.push_back(packet);
            
            // Insérer les tables si nécessaire
            for (auto& [tablePid, counter] : tableCounters) {
                counter++;
                
                size_t period;
                if (tablePid == PID_PAT) {
                    period = PAT_PERIOD;
                }
                else if (services_.size() == 1 && tablePid == services_.begin()->second.pmtPid) {
                    period = PMT_PERIOD;
                }
                else if (tablePid == PID_SDT) {
                    period = SDT_PERIOD;
                }
                else if (tablePid == PID_EIT) {
                    period = EIT_PERIOD;
                }
                else if (tablePid == PID_NIT) {
                    period = NIT_PERIOD;
                }
                else {
                    // Pour les autres PMT, utiliser une période différente
                    period = PMT_PERIOD + 50;
                }
                
                if (counter >= period) {
                    counter = 0;
                    
                    // Vérifier si nous avons des sections pour ce PID
                    auto sectionsIt = sections.find(tablePid);
                    if (sectionsIt != sections.end() && !sectionsIt->second.empty()) {
                        // Insérer les sections
                        ts::TSPacketVector tablePackets;
                        for (const auto& section : sectionsIt->second) {
                            ts::TSPacketVector sectionPackets;
                            section->toTSPackets(sectionPackets, tablePid);
                            tablePackets.insert(tablePackets.end(), sectionPackets.begin(), sectionPackets.end());
                        }
                        
                        resultPackets.insert(resultPackets.end(), tablePackets.begin(), tablePackets.end());
                    }
                }
            }
        }
        
        // Convertir les paquets en vecteur d'octets
        std::vector<uint8_t> result;
        result.reserve(resultPackets.size() * ts::PKT_SIZE);
        
        for (const auto& packet : resultPackets) {
            const uint8_t* packetData = packet.b;
            result.insert(result.end(), packetData, packetData + ts::PKT_SIZE);
        }
        
        return result;
    }
    catch (const std::exception& e) {
        spdlog::error("Erreur lors de l'insertion des tables: {}", e.what());
        
        // En cas d'erreur, retourner les données d'origine
        return data;
    }
}

DVBProcessor::~DVBProcessor() {
    cleanup();
}