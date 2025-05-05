#include "multicast/MulticastSender.h"
#include "alerting/AlertManager.h"
#include <spdlog/spdlog.h>

#include <cstring>
#include <algorithm>

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

namespace hls_to_dvb {

MulticastSender::MulticastSender(const std::string& groupAddress, int port, 
                              const std::string& interface, int ttl)
    : groupAddress_(groupAddress), port_(port), ttl_(ttl),
      running_(false), bitrateKbps_(0), socket_(INVALID_SOCKET) {
    
    // Déterminer l'interface à utiliser
    if (interface.empty()) {
#ifdef __APPLE__
        // Sur macOS, essayer d'utiliser "en0" (interface Ethernet) ou "en1" (WiFi)
        interface_ = "en0";
        spdlog::info("No interface specified, using default macOS interface: en0");
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
    if (running_) {
        spdlog::warn("MulticastSender already running");
        return false;
    }
    
    // Vérifier si le socket est initialisé
    if (socket_ == INVALID_SOCKET) {
        // Tentative d'initialisation automatique
        spdlog::warn("Socket not initialized, attempting automatic initialization");
        if (!initialize()) {
            spdlog::error("Automatic initialization failed: Cannot start MulticastSender");
            return false;
        }
    }
    
    running_ = true;
    
    // Démarrer le thread d'envoi
    senderThread_ = std::thread(&MulticastSender::senderLoop, this);
    
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

bool MulticastSender::send(const std::vector<uint8_t>& data) {
    if (!running_) {
        spdlog::warn("MulticastSender not running");
        return false;
    }
    
    // Ajouter les données à la file d'attente
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        dataQueue_.push(data);
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

void MulticastSender::senderLoop() {
    spdlog::debug("MulticastSender thread started");
    
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
    
    // Taille maximum d'un datagramme UDP (pour éviter la fragmentation)
    const size_t MAX_PACKET_SIZE = 1316; // 7 paquets MPEG-TS (7*188=1316)
    
    // Timers pour le contrôle de débit
    auto lastSendTime = std::chrono::steady_clock::now();
    size_t bytesSent = 0;
    
    // Vérifier périodiquement si le socket est toujours valide
    int retryCount = 0;
    
    while (running_) {
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
            
            data = std::move(dataQueue_.front());
            dataQueue_.pop();
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
        for (size_t offset = 0; offset < data.size(); offset += MAX_PACKET_SIZE) {
            // Calculer la taille du paquet actuel
            size_t packetSize = std::min(MAX_PACKET_SIZE, data.size() - offset);
            
            // Envoyer le paquet
            int sendResult = sendto(socket_, 
                                  reinterpret_cast<const char*>(data.data() + offset), 
                                  static_cast<int>(packetSize), 
                                  0, 
                                  reinterpret_cast<struct sockaddr*>(&destAddr), 
                                  sizeof(destAddr));
            
            if (sendResult == SOCKET_ERROR) {
                stats_.errors++;
                
#ifdef _WIN32
                spdlog::error("Error sending multicast packet: {}", WSAGetLastError());
#else
                spdlog::error("Error sending multicast packet: {}", strerror(errno));
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
            
            // Mettre à jour les statistiques
            stats_.packetsSent++;
            stats_.bytesSent += packetSize;
            bytesSent += packetSize;
            
            if (bitrateKbps_ > 0) {
                // Ajouter un léger délai entre les paquets pour lisser le débit
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        
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
    
    spdlog::debug("MulticastSender thread ended");
}

bool MulticastSender::createSocket() {
    // Fermer le socket existant si nécessaire
    closeSocket();
    spdlog::info("createSocket() appelé");
    
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


