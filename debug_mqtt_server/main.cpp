#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
struct ClientSession {
    int fd{-1};
    std::string clientId{"?"};
    std::set<std::string> subscriptions{};
    std::vector<uint8_t> inbox{}; // buffer with partial frames
};

struct Config {
    int port{1883};
    int maxClients{8};
    bool logPackets{false};
    bool traceSubscriptions{false};
    bool traceMessages{true};
    bool quiet{false};
    int artificialDelayMs{0};
};

volatile std::sig_atomic_t stopFlag = 0;

void handleSignal(int) { stopFlag = 1; }

void log(const std::string &msg, bool quiet) {
    if (!quiet) {
        std::cout << msg << std::endl;
    }
}

std::string toHex(const std::vector<uint8_t> &data) {
    std::ostringstream oss;
    for (size_t i = 0; i < data.size(); ++i) {
        if (i) oss << ' ';
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

bool decodeRemainingLength(const std::vector<uint8_t> &buf, size_t &offset, size_t &length) {
    int multiplier = 1;
    length = 0;
    size_t consumed = 0;
    while (offset + consumed < buf.size()) {
        uint8_t encoded = buf[offset + consumed];
        length += (encoded & 127) * multiplier;
        multiplier *= 128;
        ++consumed;
        if (!(encoded & 128)) {
            offset += consumed;
            return true;
        }
        if (consumed > 4) return false;
    }
    return false;
}

std::vector<uint8_t> encodeRemainingLength(size_t length) {
    std::vector<uint8_t> out;
    do {
        uint8_t encoded = length % 128;
        length /= 128;
        if (length > 0) encoded |= 128;
        out.push_back(encoded);
    } while (length > 0);
    return out;
}

bool sendAll(int fd, const std::vector<uint8_t> &data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t sent = ::send(fd, data.data() + total, data.size() - total, 0);
        if (sent <= 0) return false;
        total += static_cast<size_t>(sent);
    }
    return true;
}

void sendConnack(int fd) {
    std::vector<uint8_t> pkt{0x20, 0x02, 0x00, 0x00};
    sendAll(fd, pkt);
}

void sendPuback(int fd, uint16_t packetId) {
    std::vector<uint8_t> pkt{0x40, 0x02, static_cast<uint8_t>(packetId >> 8), static_cast<uint8_t>(packetId & 0xFF)};
    sendAll(fd, pkt);
}

void sendSuback(int fd, uint16_t packetId, uint8_t qos = 0x00) {
    std::vector<uint8_t> pkt{0x90, 0x03, static_cast<uint8_t>(packetId >> 8), static_cast<uint8_t>(packetId & 0xFF), qos};
    sendAll(fd, pkt);
}

void sendPingResp(int fd) {
    std::vector<uint8_t> pkt{0xD0, 0x00};
    sendAll(fd, pkt);
}

std::vector<uint8_t> buildPublish(const std::string &topic, const std::string &payload) {
    std::vector<uint8_t> data;
    data.push_back(0x30); // QoS0 publish
    std::vector<uint8_t> variable;
    variable.push_back(static_cast<uint8_t>(topic.size() >> 8));
    variable.push_back(static_cast<uint8_t>(topic.size() & 0xFF));
    variable.insert(variable.end(), topic.begin(), topic.end());
    variable.insert(variable.end(), payload.begin(), payload.end());

    auto encoded = encodeRemainingLength(variable.size());
    data.insert(data.end(), encoded.begin(), encoded.end());
    data.insert(data.end(), variable.begin(), variable.end());
    return data;
}

void broadcast(const std::string &topic, const std::string &payload, std::map<int, ClientSession> &clients, int senderFd, bool verbose) {
    for (auto &pair : clients) {
        if (pair.first == senderFd) continue;
        if (pair.second.subscriptions.count(topic)) {
            auto pkt = buildPublish(topic, payload);
            if (sendAll(pair.first, pkt)) {
                if (verbose) {
                    std::cout << "Forwarded to " << pair.second.clientId << " on topic '" << topic << "'" << std::endl;
                }
            }
        }
    }
}

void processPacket(ClientSession &client, const Config &cfg, std::map<int, ClientSession> &clients) {
    while (client.inbox.size() >= 2) {
        uint8_t header = client.inbox[0];
        size_t offset = 1;
        size_t remainingLength = 0;
        if (!decodeRemainingLength(client.inbox, offset, remainingLength)) return; // wait for more data
        size_t totalPacketLength = offset + remainingLength;
        if (client.inbox.size() < totalPacketLength) return; // incomplete

        std::vector<uint8_t> packet(client.inbox.begin(), client.inbox.begin() + totalPacketLength);
        client.inbox.erase(client.inbox.begin(), client.inbox.begin() + totalPacketLength);

        if (cfg.logPackets) {
            std::cout << "[raw] op=" << ((header >> 4) & 0x0F) << " bytes=" << toHex(packet) << std::endl;
        }

        uint8_t type = header >> 4;
        switch (type) {
            case 1: { // CONNECT
                size_t pos = offset;
                if (pos + 2 > packet.size()) break;
                uint16_t protoLen = (packet[pos] << 8) | packet[pos + 1];
                pos += 2 + protoLen; // skip protocol name
                if (pos + 4 > packet.size()) break;
                pos += 4; // protocol level + flags + keepalive
                if (pos + 2 > packet.size()) break;
                uint16_t clientIdLen = (packet[pos] << 8) | packet[pos + 1];
                pos += 2;
                std::string cid;
                if (pos + clientIdLen <= packet.size()) {
                    cid.assign(reinterpret_cast<const char*>(&packet[pos]), clientIdLen);
                }
                client.clientId = cid.empty() ? client.clientId : cid;
                log("[connect] clientId=" + client.clientId, cfg.quiet);
                if (cfg.artificialDelayMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.artificialDelayMs));
                }
                sendConnack(client.fd);
                break;
            }
            case 3: { // PUBLISH
                size_t pos = offset;
                if (pos + 2 > packet.size()) break;
                uint16_t topicLen = (packet[pos] << 8) | packet[pos + 1];
                pos += 2;
                if (pos + topicLen > packet.size()) break;
                std::string topic(reinterpret_cast<const char*>(&packet[pos]), topicLen);
                pos += topicLen;
                std::string payload;
                int qos = (header >> 1) & 0x03;
                if (qos > 0) {
                    if (pos + 2 > packet.size()) break;
                    uint16_t packetId = (packet[pos] << 8) | packet[pos + 1];
                    pos += 2;
                    if (pos < packet.size()) {
                        payload.assign(reinterpret_cast<const char*>(&packet[pos]), packet.size() - pos);
                    }
                    sendPuback(client.fd, packetId);
                } else {
                    if (pos < packet.size()) {
                        payload.assign(reinterpret_cast<const char*>(&packet[pos]), packet.size() - pos);
                    }
                }
                if (cfg.traceMessages) {
                    log("[publish] from=" + client.clientId + " topic='" + topic + "' payload='" + payload + "'", cfg.quiet);
                }
                broadcast(topic, payload, clients, client.fd, cfg.traceMessages);
                break;
            }
            case 8: { // SUBSCRIBE
                size_t pos = offset;
                if (pos + 2 > packet.size()) break;
                uint16_t packetId = (packet[pos] << 8) | packet[pos + 1];
                pos += 2;
                while (pos + 2 <= packet.size()) {
                    uint16_t topicLen = (packet[pos] << 8) | packet[pos + 1];
                    pos += 2;
                    if (pos + topicLen + 1 > packet.size()) break;
                    std::string topic(reinterpret_cast<const char*>(&packet[pos]), topicLen);
                    pos += topicLen;
                    ++pos; // qos byte
                    client.subscriptions.insert(topic);
                    if (cfg.traceSubscriptions) {
                        log("[subscribe] " + client.clientId + " -> '" + topic + "'", cfg.quiet);
                    }
                }
                sendSuback(client.fd, packetId);
                break;
            }
            case 12: { // PINGREQ
                if (!cfg.quiet) std::cout << "[ping] from " << client.clientId << std::endl;
                sendPingResp(client.fd);
                break;
            }
            case 14: { // DISCONNECT
                log("[disconnect] " + client.clientId, cfg.quiet);
                close(client.fd);
                client.fd = -1;
                return;
            }
            default:
                log("[warn] Unhandled packet type " + std::to_string(type), cfg.quiet);
        }
    }
}

Config parseArgs(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            cfg.port = std::stoi(argv[++i]);
        } else if (arg == "--max" && i + 1 < argc) {
            cfg.maxClients = std::stoi(argv[++i]);
        } else if (arg == "--quiet") {
            cfg.quiet = true;
        } else if (arg == "--raw") {
            cfg.logPackets = true;
        } else if (arg == "--trace-sub") {
            cfg.traceSubscriptions = true;
        } else if (arg == "--no-trace-msg") {
            cfg.traceMessages = false;
        } else if (arg == "--delay" && i + 1 < argc) {
            cfg.artificialDelayMs = std::stoi(argv[++i]);
        } else {
            std::cout << "Usage: " << argv[0] << " [--port <p>] [--max <n>] [--quiet] [--raw] [--trace-sub] [--no-trace-msg] [--delay <ms>]" << std::endl;
            std::exit(1);
        }
    }
    return cfg;
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handleSignal);
    Config cfg = parseArgs(argc, argv);

    int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cfg.port);

    if (bind(serverFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(serverFd, cfg.maxClients) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "MQTT debug server listening on port " << cfg.port << std::endl;

    std::map<int, ClientSession> clients;

    while (!stopFlag) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverFd, &readfds);
        int maxfd = serverFd;
        for (const auto &pair : clients) {
            if (pair.first >= 0) {
                FD_SET(pair.first, &readfds);
                if (pair.first > maxfd) maxfd = pair.first;
            }
        }
        timeval tv{1, 0};
        int activity = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
        if (activity < 0 && errno != EINTR) {
            perror("select");
            break;
        }
        if (stopFlag) break;

        if (FD_ISSET(serverFd, &readfds)) {
            int newFd = accept(serverFd, nullptr, nullptr);
            if (newFd >= 0) {
                if (static_cast<int>(clients.size()) >= cfg.maxClients) {
                    log("[drop] too many clients", cfg.quiet);
                    close(newFd);
                } else {
                    ClientSession session;
                    session.fd = newFd;
                    clients[newFd] = session;
                    log("[accept] fd=" + std::to_string(newFd), cfg.quiet);
                }
            }
        }

        std::vector<int> toRemove;
        for (auto &pair : clients) {
            int fd = pair.first;
            if (FD_ISSET(fd, &readfds)) {
                uint8_t buffer[2048];
                ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
                if (received <= 0) {
                    log("[close] fd=" + std::to_string(fd), cfg.quiet);
                    close(fd);
                    toRemove.push_back(fd);
                } else {
                    pair.second.inbox.insert(pair.second.inbox.end(), buffer, buffer + received);
                    processPacket(pair.second, cfg, clients);
                    if (pair.second.fd < 0) {
                        toRemove.push_back(fd);
                    }
                }
            }
        }
        for (int fd : toRemove) {
            clients.erase(fd);
        }
    }

    for (auto &pair : clients) {
        close(pair.first);
    }
    close(serverFd);
    std::cout << "Server stopped" << std::endl;
    return 0;
}

