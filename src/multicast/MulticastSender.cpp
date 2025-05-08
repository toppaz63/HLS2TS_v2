#include "multicast/MulticastSender.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>

#include <cstring>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <net/if.h>  // Pour if_nametoindex et IFNAMSIZ
  #include <sys/ioctl.h>  // Pour ioctl et SIOCGIFADDR
  #include <ctype.h>  // Pour isalpha
  #define SOCKET int
  #define INVALID_SOCKET -1
  #define SOCKET_ERROR -1
  #define closesocket close
#endif
#ifdef __APPLE__
#include <ifaddrs.h>
#include <netdb.h>
#endif

namespace hls_to_dvb {

MulticastSender::MulticastSender(const std::string& groupAddress, int port, 
                              const std::string& interface, int ttl)
    : groupAddress_(groupAddress), port_(port), ttl_(ttl),
      running_(false), bitrateKbps_(0), socket_(INVALID_SOCKET) {
    
    // Déterminer l'interface à utiliser
    if (interface.empty()) {
#ifdef __APPLE__
        // Sur macOS, essayer de détecter l'interface active
        interface_ = detectActiveInterface();
        spdlog::info("No interface specified, auto-detected interface: {}", interface_);
#else
        interface_ = "";
#endif
    } else {
        interface_ = interface;
    }
    
    // Initialiser l'horodatage de dernier envoi
    stats_.lastSendTime = std::chrono::system_clock::now();
}

MulticastSender::~MulticastSender() {
    stop();
    closeSocket();
}

bool MulticastSender::initialize() {
#ifdef _WIN32
    // Initialiser Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        spdlog::error("WSAStartup failed");
        return false;
    }
#endif

    // Vérifier les permissions réseau (surtout pour macOS 15.4+)
    if (!checkNetworkPermissions()) {
        spdlog::error("Network permission check failed");
        AlertManager::getInstance().addAlert(
            AlertLevel::WARNING,
            "MulticastSender",
            "Network permission issues detected. On macOS 15.4, try disabling 'Limit IP Address Tracking' in Network settings",
            false
        );
        // Continuer malgré l'avertissement
    }
    
    // Créer le socket
    if (!createSocket()) {
        return false;
    }
    
    spdlog::info("MulticastSender initialized for group {}:{}", groupAddress_, port_);
    AlertManager::getInstance().addAlert(
        AlertLevel::INFO,
        "MulticastSender",
        "Initialized for group " + groupAddress_ + ":" + std::to_string(port_),
        false
    );
    
    return true;
}

bool MulticastSender::start() {
        spdlog::error("*** [M1] DÉBUT DE MulticastSender::start() ***");

    if (running_) {
        spdlog::warn("MulticastSender already running");
        return false;
    }
    spdlog::error("*** [M2] APRÈS VÉRIFICATION running_ ***");

    // Vérifier si le socket est initialisé
    if (socket_ == INVALID_SOCKET) {
        // Tentative d'initialisation automatique
        spdlog::warn("Socket not initialized, attempting automatic initialization");
        if (!initialize()) {
            spdlog::error("Automatic initialization failed: Cannot start MulticastSender");
            return false;
        }
        spdlog::error("*** [M5] APRÈS initialize() ***");
    }
    spdlog::error("*** [M6] AVANT reset() ***");
    // Réinitialiser les statistiques
    stats_.reset();
    spdlog::error("*** [M7] APRÈS reset() ***");
    // IMPORTANT: Trace de débogage supplémentaire
    spdlog::info("MulticastSender starting thread and setting running=true");
    running_ = true;
    spdlog::error("*** [M8] AVANT sendTestPacket() ***");
    // Envoyer un paquet de test pour vérifier la configuration
    spdlog::info("Sending test packet before starting sender thread");
    if (sendTestPacket()) {
        spdlog::info("Test packet sent successfully, socket configuration is working");
    } else {
        spdlog::error("Failed to send test packet, socket configuration may be incorrect");
        // Ne pas retourner false, continuer malgré l'échec
    }
    spdlog::error("*** [M9] APRÈS sendTestPacket() ***");
    // IMPORTANT: Utiliser try/catch pour détecter les problèmes de démarrage de thread
    try {
        // Démarrer le thread d'envoi
        senderThread_ = std::thread(&MulticastSender::senderLoop, this);
        spdlog::error("*** [M11] THREAD CRÉÉ AVEC SUCCÈS ***");
        spdlog::info("MulticastSender thread started successfully");
    } catch (const std::exception& e) {
        spdlog::error("Failed to start MulticastSender thread: {}", e.what());
        running_ = false;
        return false;
    }
    spdlog::error("*** [M13] FIN DE MulticastSender::start() ***");
    spdlog::info("MulticastSender started for group {}:{}", groupAddress_, port_);
    return true;
}


void MulticastSender::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Notifier tous les threads en attente
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueCond_.notify_all();
    }
    
    // Attendre que le thread se termine
    if (senderThread_.joinable()) {
        senderThread_.join();
    }
    
    spdlog::info("MulticastSender stopped for group {}:{}", groupAddress_, port_);
}

bool MulticastSender::isRunning() const {
    return running_;
}

bool MulticastSender::sendTestPacket() {
    if (socket_ == INVALID_SOCKET) {
        spdlog::error("Cannot send test packet - socket is invalid");
        return false;
    }
    
    // Créer un paquet de test (188 octets, taille d'un paquet MPEG-TS)
    std::vector<uint8_t> testData(188, 0xFF);
    
    // Ajouter un en-tête pour identifier qu'il s'agit d'un paquet de test
    const char* testHeader = "MPEGTS_TEST_PACKET";
    std::memcpy(testData.data(), testHeader, strlen(testHeader));
    
    // Préparer l'adresse de destination
    struct sockaddr_in destAddr;
    std::memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    
    if (inet_pton(AF_INET, groupAddress_.c_str(), &destAddr.sin_addr) != 1) {
        spdlog::error("Invalid destination address for test packet: {}", groupAddress_);
        return false;
    }
    
    destAddr.sin_port = htons(port_);
    
    char addrStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(destAddr.sin_addr), addrStr, INET_ADDRSTRLEN);
    spdlog::info("Sending multicast test packet to {}:{} (size: {} bytes, socket: {})", 
               addrStr, port_, testData.size(), socket_);
    
    // Envoyer le paquet de test directement (bypass de la file d'attente)
    int sendResult = sendto(socket_, 
                         reinterpret_cast<const char*>(testData.data()), 
                         static_cast<int>(testData.size()), 
                         0, 
                         reinterpret_cast<struct sockaddr*>(&destAddr), 
                         sizeof(destAddr));
    
    if (sendResult == SOCKET_ERROR) {
#ifdef _WIN32
        int errorCode = WSAGetLastError();
        spdlog::error("Error sending test packet: {} (WSA error={})", errorCode, errorCode);
#else
        int errorCode = errno;
        spdlog::error("Error sending test packet: {} (errno={})", strerror(errorCode), errorCode);
#endif
        return false;
    }
    
    spdlog::info("Multicast test packet sent successfully ({} bytes)", sendResult);
    return true;
}


bool MulticastSender::send(const std::vector<uint8_t>& data, bool discontinuity) {
    if (!running_) {
        spdlog::warn("MulticastSender not running");
        return false;
    }
    
    // Ajouter les données à la file d'attente avec l'indicateur de discontinuité
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        
        // Si c'est une discontinuité, ajouter un marqueur spécial dans la file
        if (discontinuity) {
            spdlog::info("Discontinuité détectée, ajout d'un marqueur dans la file d'attente multicast");
            
            // On peut ajouter un traitement spécial si nécessaire
            // Par exemple, vider partiellement la file pour éviter des délais importants
            if (dataQueue_.size() > 10) {
                spdlog::info("File d'attente volumineuse lors d'une discontinuité, conservation des 5 derniers segments uniquement");
                
                // Créer une file temporaire avec les éléments à conserver dans le bon ordre
                std::vector<std::pair<std::vector<uint8_t>, bool>> lastItems;
                
                // Récupérer tous les éléments de la file
                while (!dataQueue_.empty()) {
                    lastItems.push_back(dataQueue_.front());
                    dataQueue_.pop();
                }
                
                // Ne garder que les 5 derniers éléments
                if (lastItems.size() > 5) {
                    // Supprimer tous les éléments sauf les 5 derniers
                    lastItems.erase(lastItems.begin(), lastItems.end() - 5);
                }
                
                // Remettre les éléments conservés dans la file
                for (const auto& item : lastItems) {
                    dataQueue_.push(item);
                }
                
                spdlog::info("File d'attente redimensionnée à {} éléments pour la nouvelle discontinuité", dataQueue_.size());
            }
        }
        
        // Ajouter les données avec l'indicateur de discontinuité
        dataQueue_.push(std::make_pair(data, discontinuity));
        // Log pour suivre les données ajoutées à la file multicast
        spdlog::info("Segment ajouté à la file multicast, taille: {} octets, discontinuité: {}", data.size(), discontinuity ? "oui" : "non");
    }
    
    // Notifier le thread d'envoi
    queueCond_.notify_one();
    
    return true;
}

void MulticastSender::setBitrate(uint32_t bitrateKbps) {
    bitrateKbps_ = bitrateKbps;
    spdlog::info("MulticastSender bitrate set to {} kbps", bitrateKbps);
}

const MulticastStats& MulticastSender::getStats() const {
    return stats_;
}

std::string MulticastSender::getGroupAddress() const {
    return groupAddress_;
}

int MulticastSender::getPort() const {
    return port_;
}

std::string MulticastSender::detectActiveInterface() {
#ifdef __APPLE__
    // Sur macOS, essayer de détecter une interface active
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        spdlog::error("Unable to create socket for interface detection: {}", strerror(errno));
        return "";
    }
    
    // Liste des interfaces courantes sur macOS
    const std::vector<std::string> commonInterfaces = {"en0", "en1", "en2", "en3", "en4", "en5", "en6", "en7", "en8"};
    
    for (const auto& ifName : commonInterfaces) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ - 1);
        
        if (ioctl(socket_fd, SIOCGIFADDR, &ifr) >= 0) {
            struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, addr_str, INET_ADDRSTRLEN);
            
            // Vérifier que c'est une interface active (non loopback)
            if (std::string(addr_str) != "127.0.0.1") {
                close(socket_fd);
                spdlog::info("Detected active interface: {} with IP {}", ifName, addr_str);
                return ifName;
            }
        }
    }
    
    close(socket_fd);
    spdlog::warn("No active interface detected, falling back to default");
    return "en0";  // Retourner en0 par défaut
#else
    return "";
#endif
}

void MulticastSender::senderLoop() {
    spdlog::info("MulticastSender::senderLoop STARTED for group {}:{}", groupAddress_, port_);
    
    // Vérification immédiate du socket
    if (socket_ == INVALID_SOCKET) {
        spdlog::error("Socket invalid at start of senderLoop for {}:{}", groupAddress_, port_);
    } else {
        spdlog::info("Socket valid at start of senderLoop: {}", socket_);
    }    
    // Structure pour l'adresse de destination
    struct sockaddr_in destAddr;
    std::memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    
    // Utiliser inet_pton au lieu de inet_addr
    if (inet_pton(AF_INET, groupAddress_.c_str(), &destAddr.sin_addr) != 1) {
        spdlog::error("Invalid destination address: {}", groupAddress_);
        running_ = false;
        return;
    }
    
    destAddr.sin_port = htons(port_);
    
    // Imprimer l'adresse formatée pour le débogage
    char addrStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(destAddr.sin_addr), addrStr, INET_ADDRSTRLEN);
    spdlog::info("Adresse de destination configurée: {}:{}", addrStr, ntohs(destAddr.sin_port));
    
    // Taille maximum d'un datagramme UDP (pour éviter la fragmentation)
    const size_t MAX_PACKET_SIZE = 1316; // 7 paquets MPEG-TS (7*188=1316)
    
    // Timers pour le contrôle de débit
    auto lastSendTime = std::chrono::steady_clock::now();
    size_t bytesSent = 0;
    
    // Vérifier périodiquement si le socket est toujours valide
    int retryCount = 0;
    int packetsSentTotal = 0;
    
    while (running_) {
        static int loopCount = 0;
        if (++loopCount % 1000 == 0) {
            spdlog::info("MulticastSender::senderLoop est actif, itération {}, socket={}, queue_size={}", loopCount, socket_, dataQueue_.size());
        }
        // Vérifier si le socket est valide
        if (socket_ == INVALID_SOCKET) {
            retryCount++;
            if (retryCount <= 3) {
                spdlog::error("Socket invalid, attempting to recreate (attempt {}/3)", retryCount);
                if (!createSocket()) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
            } else {
                spdlog::error("Failed to recreate socket after multiple attempts, exiting thread");
                running_ = false;
                break;
            }
        }
        
        std::vector<uint8_t> data;
        bool isDiscontinuity = false;
        
        // Attendre des données à envoyer
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (dataQueue_.empty()) {
                // Attendre qu'il y ait des données ou que le thread doive s'arrêter
                queueCond_.wait_for(lock, std::chrono::milliseconds(100),
                                   [this] { return !dataQueue_.empty() || !running_; });
                
                if (!running_) {
                    break;
                }
                
                if (dataQueue_.empty()) {
                    continue;
                }
            }
            
            // Récupérer les données et l'indicateur de discontinuité
            auto dataPair = std::move(dataQueue_.front());
            dataQueue_.pop();
            
            data = std::move(dataPair.first);
            isDiscontinuity = dataPair.second;
            
            spdlog::info("Données extraites de la file d'attente, taille: {} octets, discontinuité: {}", 
                      data.size(), isDiscontinuity ? "oui" : "non");
        }
        
        if (data.empty()) {
            spdlog::warn("Données extraites de la file d'attente vides, ignorées");
            continue;
        }
        
        // Si limitation de débit active, contrôler le débit
        if (bitrateKbps_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSendTime).count();
            
            // Calculer le temps nécessaire pour envoyer les données au débit configuré
            double byteRate = bitrateKbps_ * 1000.0 / 8.0; // Octets par seconde
            double expectedTimeMs = (bytesSent * 1000.0) / byteRate;
            
            // Si on est en avance, attendre
            if (elapsedMs < expectedTimeMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<long>(expectedTimeMs - elapsedMs)));
            }
            
            // Réinitialiser si on dépasse 1 seconde
            if (elapsedMs > 1000) {
                lastSendTime = now;
                bytesSent = 0;
            }
        }
        
        // Envoyer les données par paquets pour éviter la fragmentation
        int successPackets = 0;
        int failedPackets = 0;
        
        for (size_t offset = 0; offset < data.size(); offset += MAX_PACKET_SIZE) {
            // Calculer la taille du paquet actuel
            size_t packetSize = std::min(MAX_PACKET_SIZE, data.size() - offset);
            
            spdlog::debug("Tentative d'envoi multicast: {} octets vers {}:{} (offset={})",
                       packetSize, groupAddress_, port_, offset);
            
            // Envoyer le paquet
            int sendResult = sendto(socket_, 
                                  reinterpret_cast<const char*>(data.data() + offset), 
                                  static_cast<int>(packetSize), 
                                  0, 
                                  reinterpret_cast<struct sockaddr*>(&destAddr), 
                                  sizeof(destAddr));
            if (sendResult != SOCKET_ERROR) {
                // Log détaillé toutes les 100 paquets pour éviter de surcharger les logs
                static int packetLogCount = 0;
                if (++packetLogCount % 100 == 0) {
                    spdlog::debug("Paquet multicast #{} envoyé avec succès: {} octets vers {}:{} (offset={})",
                                packetLogCount, packetSize, groupAddress_, port_, offset);
                }
            }
            if (sendResult == SOCKET_ERROR) {
                failedPackets++;
                stats_.errors++;
                
                #ifdef _WIN32
                spdlog::error("Error sending multicast packet: {} (WSA error={})", WSAGetLastError(), WSAGetLastError());
                #else
                spdlog::error("Error sending multicast packet: {} (errno={})", strerror(errno), errno);
                #endif
                
                AlertManager::getInstance().addAlert(
                    AlertLevel::ERROR,
                    "MulticastSender",
                    "Error sending multicast packet",
                    false
                );
                
                // Attendre un peu avant de réessayer
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            // Succès
            successPackets++;
            packetsSentTotal++;
            
            // Mettre à jour les statistiques
            stats_.packetsSent++;
            stats_.bytesSent += packetSize;
            bytesSent += packetSize;
            
            if (packetsSentTotal % 100 == 0) {
                spdlog::info("Multicast: {} paquets envoyés avec succès jusqu'à présent", packetsSentTotal);
                spdlog::info("Multicast: paquets envoyés {} ({}KB), état socket: {}, destination: {}:{}",
                   packetsSentTotal, stats_.bytesSent/1024, socket_ != INVALID_SOCKET ? "valide" : "invalide",
                   groupAddress_, port_);
            }
            
            if (bitrateKbps_ > 0) {
                // Ajouter un léger délai entre les paquets pour lisser le débit
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        
        spdlog::info("Segment multicast envoyé: {} paquets réussis, {} paquets échoués", 
                   successPackets, failedPackets);
        
        // Mettre à jour le débit instantané
        auto now = std::chrono::system_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - stats_.lastSendTime).count();
        
        if (elapsedMs > 0) {
            stats_.instantBitrate = (data.size() * 8.0) / (elapsedMs / 1000.0);
            
            // Mise à jour du débit moyen (moyenne mobile)
            if (stats_.bitrate == 0.0) {
                stats_.bitrate = stats_.instantBitrate;
            } else {
                stats_.bitrate = stats_.bitrate * 0.9 + stats_.instantBitrate * 0.1;
            }
        }
        
        stats_.lastSendTime = now;
    }
    spdlog::info("Sortie de la boucle principale du MulticastSender, stats: packets={}, bytes={}, errors={}", stats_.packetsSent, stats_.bytesSent, stats_.errors);
    spdlog::info("Thread d'envoi multicast terminé pour {}:{}", groupAddress_, port_);
}


bool MulticastSender::createSocket() {
    // Fermer le socket existant si nécessaire
    closeSocket();
    spdlog::info("createSocket() appelé");
    
#ifdef __APPLE__
    // Vérifier l'état actuel du firewall
    int result = system("defaults read /Library/Preferences/com.apple.alf globalstate");
    spdlog::info("État du pare-feu macOS : {}", result);
    
    // Identifier les interfaces et leurs adresses
    struct ifaddrs *ifaddr, *ifa;
    int family, s;
    char host[NI_MAXHOST];
    
    if (getifaddrs(&ifaddr) == -1) {
        spdlog::error("getifaddrs failed: {}", strerror(errno));
    } else {
        spdlog::info("Interfaces réseau disponibles :");
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL)
                continue;
                
            family = ifa->ifa_addr->sa_family;
            
            if (family == AF_INET) {
                s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                              host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
                if (s != 0) {
                    spdlog::error("getnameinfo() failed: {}", gai_strerror(s));
                    continue;
                }
                
                spdlog::info("  Interface: {}\tAdresse: {}\tFlags: 0x{:x}",
                          ifa->ifa_name, host, ifa->ifa_flags);
            }
        }
        
        freeifaddrs(ifaddr);
    }
#endif

    // Créer un nouveau socket UDP
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ == INVALID_SOCKET) {
    #ifdef _WIN32
        spdlog::error("Failed to create socket: {}", WSAGetLastError());
    #else
        spdlog::error("Failed to create socket: {}", strerror(errno));
    #endif
        return false;
    }
    
    // NOUVEAU CODE - APPLICABLE À TOUTES LES PLATEFORMES
    // Configurer la réutilisation d'adresse
    int reuseAddr = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, 
                  reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr)) < 0) {
    #ifdef _WIN32
        spdlog::error("Failed to set SO_REUSEADDR: {}", WSAGetLastError());
    #else
        spdlog::error("Failed to set SO_REUSEADDR: {}", strerror(errno));
    #endif
        closeSocket();
        return false;
    }
    
    // Lier le socket à toutes les interfaces
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);  // Bind à toutes les interfaces
    bindAddr.sin_port = htons(port_);
        
    if (bind(socket_, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
    #ifdef _WIN32
        spdlog::error("Failed to bind socket: {}", WSAGetLastError());
    #else
        spdlog::error("Failed to bind socket: {} (errno={})", strerror(errno), errno);
    #endif
        closeSocket();
        return false;
    }
    spdlog::info("Socket multicast lié avec succès à INADDR_ANY:{} sur toutes les plateformes", port_);
    // FIN DU NOUVEAU CODE
    
    // SUPPRIMER OU COMMENTER LE BLOC SUIVANT POUR ÉVITER LA REDÉFINITION
    /*
    #ifdef __APPLE__
    // Activer la réutilisation d'adresse ET de port (crucial pour multicast sur macOS)
    int reuseAddr = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)) < 0) {
        spdlog::error("Failed to set SO_REUSEADDR: {}", strerror(errno));
        closeSocket();
        return false;
    }
    
    int reusePort = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT, &reusePort, sizeof(reusePort)) < 0) {
        spdlog::warn("Failed to set SO_REUSEPORT: {}", strerror(errno));
        // Continue même si SO_REUSEPORT échoue
    }
    
    // Sur macOS, il est OBLIGATOIRE de lier le socket avant d'envoyer des données multicast
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);  // Bind à toutes les interfaces
    bindAddr.sin_port = htons(port_);
    
    if (bind(socket_, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        spdlog::error("Failed to bind socket: {} (errno={})", strerror(errno), errno);
        closeSocket();
        return false;
    }
    spdlog::info("Socket multicast lié avec succès à INADDR_ANY:{}", port_);
    #endif
    */
    
    // AJOUTER SPÉCIFIQUEMENT POUR MACOS (SANS REDÉFINIR LES VARIABLES)
    #ifdef __APPLE__
    // Activer la réutilisation de port sur macOS (en plus de la réutilisation d'adresse déjà configurée)
    int reusePort = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT, &reusePort, sizeof(reusePort)) < 0) {
        spdlog::warn("Failed to set SO_REUSEPORT on macOS: {}", strerror(errno));
        // Continue même si SO_REUSEPORT échoue
    } else {
        spdlog::info("SO_REUSEPORT configuré avec succès sur macOS");
    }
    #endif

    // Validation de l'adresse de groupe multicast
    struct in_addr mcastAddr;
    if (inet_pton(AF_INET, groupAddress_.c_str(), &mcastAddr) != 1) {
        spdlog::error("Invalid multicast group address: {}", groupAddress_);
        closeSocket();
        return false;
    }
 
    // Vérifier que c'est bien une adresse multicast (224.0.0.0 à 239.255.255.255)
    uint32_t addr = ntohl(mcastAddr.s_addr);
    if ((addr & 0xF0000000) != 0xE0000000) {
        spdlog::error("Not a multicast address: {}", groupAddress_);
        closeSocket();
        return false;
    }
    
    // Configurer les options multicast
    
    // Définir le TTL
    if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, 
                  reinterpret_cast<const char*>(&ttl_), sizeof(ttl_)) < 0) {
    #ifdef _WIN32
        spdlog::error("Failed to set TTL: {}", WSAGetLastError());
    #else
        spdlog::error("Failed to set TTL: {}", strerror(errno));
    #endif
        closeSocket();
        return false;
    }
    
    unsigned char loop = 1;
    if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        spdlog::warn("Failed to set IP_MULTICAST_LOOP: {}", 
                #ifdef _WIN32
                WSAGetLastError()
                #else
                strerror(errno)
                #endif
                );
        // Ne pas retourner false, ce n'est qu'un avertissement
    }

    // Activer la réutilisation d'adresse
    int reuse = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, 
                  reinterpret_cast<const char*>(&reuse), sizeof(reuse)) < 0) {
    #ifdef _WIN32
        spdlog::error("Failed to set SO_REUSEADDR: {}", WSAGetLastError());
    #else
        spdlog::error("Failed to set SO_REUSEADDR: {}", strerror(errno));
    #endif
        closeSocket();
        return false;
    }
    
    #ifdef SO_REUSEPORT
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT,
              reinterpret_cast<const char*>(&reuse), sizeof(reuse)) < 0) {
    #ifdef _WIN32
        spdlog::warn("Failed to set SO_REUSEPORT: {}", WSAGetLastError());
    #else
        spdlog::warn("Failed to set SO_REUSEPORT: {}", strerror(errno));
    #endif
        // Continuer même si SO_REUSEPORT échoue
    }
    #endif
    
    // Définir l'interface de sortie si spécifiée
    if (!interface_.empty()) {
        spdlog::info("Setting outgoing interface to: {}", interface_);
        
#ifndef _WIN32
        // Vérifier si c'est un nom d'interface (comme "en0") plutôt qu'une adresse IP
        if (isalpha(interface_[0])) {  // Si ça commence par une lettre, c'est probablement un nom d'interface
            // Obtenir l'adresse IP de l'interface
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, interface_.c_str(), IFNAMSIZ - 1);
            
            // Obtenir l'adresse IPv4 de l'interface
            if (ioctl(socket_, SIOCGIFADDR, &ifr) < 0) {
                spdlog::error("Failed to get IPv4 address for interface {}: {}", interface_, strerror(errno));
                
                // Essayer avec une autre approche: utiliser une adresse IP locale
                struct in_addr localInterface;
                localInterface.s_addr = htonl(INADDR_ANY);  // Utiliser INADDR_ANY comme fallback
                
                if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, 
                              &localInterface, sizeof(localInterface)) < 0) {
                    spdlog::error("Failed to set default interface: {}", strerror(errno));
                    closeSocket();
                    return false;
                }
                
                spdlog::warn("Using default interface instead of {}", interface_);
            } else {
                // Extraire l'adresse IP
                struct sockaddr_in* addr_in = (struct sockaddr_in*)&ifr.ifr_addr;
                struct in_addr localInterface = addr_in->sin_addr;

                // Afficher l'adresse IP obtenue pour le débogage
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &localInterface, ip_str, sizeof(ip_str));
                spdlog::info("Using IP address {} for interface {}", ip_str, interface_.c_str());
                
                if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, 
                              &localInterface, sizeof(localInterface)) < 0) {
                    spdlog::error("Failed to set outgoing interface by address: {}", strerror(errno));
                    closeSocket();
                    return false;
                }
            }
        } else {
#endif
            // Essayer de traiter l'interface comme une adresse IP
            struct in_addr localInterface;
            if (inet_pton(AF_INET, interface_.c_str(), &localInterface) != 1) {
#ifndef _WIN32
                spdlog::error("Invalid interface name or address: {}", interface_);
                
                // Essayer avec une adresse par défaut
                localInterface.s_addr = htonl(INADDR_ANY);
                spdlog::warn("Using default interface (INADDR_ANY)");
#else
                spdlog::error("Invalid interface address: {}", interface_);
                closeSocket();
                return false;
#endif
            }
            
            if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, 
                          reinterpret_cast<const char*>(&localInterface), sizeof(localInterface)) < 0) {
#ifdef _WIN32
                spdlog::error("Failed to set outgoing interface: {}", WSAGetLastError());
#else
                spdlog::error("Failed to set outgoing interface: {}", strerror(errno));
#endif
                closeSocket();
                return false;
            }
#ifndef _WIN32
        }
#endif
    } else {
        // Si aucune interface n'est spécifiée, utiliser INADDR_ANY
        spdlog::info("No interface specified, using any available interface");
        struct in_addr localInterface;
        localInterface.s_addr = htonl(INADDR_ANY);
        
        if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, 
                      reinterpret_cast<const char*>(&localInterface), sizeof(localInterface)) < 0) {
#ifdef _WIN32
            spdlog::error("Failed to set default interface: {}", WSAGetLastError());
#else
            spdlog::error("Failed to set default interface: {}", strerror(errno));
#endif
            closeSocket();
            return false;
        }
    }
    
    // Augmenter la taille du buffer d'envoi
    int sendBufSize = 1024 * 1024; // 1 MB
    if (setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, 
                  reinterpret_cast<const char*>(&sendBufSize), sizeof(sendBufSize)) < 0) {
    #ifdef _WIN32
        spdlog::warn("Failed to set send buffer size: {}", WSAGetLastError());
    #else
        spdlog::warn("Failed to set send buffer size: {}", strerror(errno));
    #endif
        // On continue quand même, ce n'est pas critique
    }
    
    spdlog::info("Socket created successfully for multicast group {}:{}", groupAddress_, port_);
    return true;
}

bool MulticastSender::checkNetworkPermissions() {
    // Vérification spécifique à macOS
    #ifdef __APPLE__
        // Désactiver temporairement "Limit IP Address Tracking" si possible
        spdlog::info("Running on macOS, checking for network permissions");
        
        // Tester une connexion locale pour voir si elle fonctionne
        struct sockaddr_in testAddr;
        std::memset(&testAddr, 0, sizeof(testAddr));
        testAddr.sin_family = AF_INET;
        testAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
        testAddr.sin_port = htons(port_);
        
        // Créer un socket de test temporaire
        int testSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (testSocket == INVALID_SOCKET) {
            spdlog::error("Failed to create test socket, possible permission issue");
            return false;
        }
        
        // Essayer de se connecter (pour UDP, cela définit simplement l'adresse par défaut)
        if (connect(testSocket, reinterpret_cast<struct sockaddr*>(&testAddr), sizeof(testAddr)) < 0) {
            spdlog::error("Failed to connect test socket: {}", strerror(errno));
            close(testSocket);
            return false;
        }
        
        close(testSocket);
    #endif
    
    return true;
}

void MulticastSender::closeSocket() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    
#ifdef _WIN32
    // Nettoyer Winsock
    WSACleanup();
#endif
}

} // namespace hls_to_dvb