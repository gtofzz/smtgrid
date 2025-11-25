#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct Config {
    std::string pattern{"incremental"};
    double errorRate{0.0};
    int latencyMs{0};
    bool verbose{true};
    bool traceRaw{false};
    std::string preloadFile{};
};

struct TransactionResult {
    bool ack{true};
    std::vector<uint8_t> payload{};
};

class I2CSim {
public:
    explicit I2CSim(const Config &cfg) : cfg_(cfg), rng_(std::random_device{}()) {
        if (cfg_.preloadFile.size()) {
            loadRegisters(cfg_.preloadFile);
        } else {
            // seed with a recognizable pattern
            for (int i = 0; i < 256; ++i) {
                registers_[i] = static_cast<uint8_t>(i & 0xFF);
            }
        }
    }

    void updateConfig(const Config &cfg) { cfg_ = cfg; }

    TransactionResult read(uint8_t addr, uint8_t reg, size_t length) {
        maybeDelay();
        TransactionResult result;
        result.ack = maybeError();
        if (!result.ack) return result;
        for (size_t i = 0; i < length; ++i) {
            uint8_t val = valueFor(reg + static_cast<uint8_t>(i));
            result.payload.push_back(val);
        }
        if (cfg_.verbose) {
            std::cout << "[read] addr=0x" << std::hex << static_cast<int>(addr)
                      << " reg=0x" << static_cast<int>(reg)
                      << " len=" << std::dec << length
                      << " -> " << bytesToString(result.payload) << std::endl;
        }
        return result;
    }

    TransactionResult write(uint8_t addr, uint8_t reg, const std::vector<uint8_t> &payload) {
        maybeDelay();
        TransactionResult result;
        result.ack = maybeError();
        if (!result.ack) return result;
        for (size_t i = 0; i < payload.size(); ++i) {
            uint8_t key = reg + static_cast<uint8_t>(i);
            registers_[key] = payload[i];
        }
        if (cfg_.verbose) {
            std::cout << "[write] addr=0x" << std::hex << static_cast<int>(addr)
                      << " reg=0x" << static_cast<int>(reg)
                      << " data=" << bytesToString(payload) << std::endl;
        }
        return result;
    }

    void scanBus() {
        std::cout << "[scan] searching devices 0x03..0x77" << std::endl;
        for (int addr = 0x03; addr <= 0x77; ++addr) {
            bool ack = maybeError();
            if (ack) {
                std::cout << "  - simulated device responded at 0x" << std::hex << addr << std::dec << std::endl;
            }
        }
    }

    void dumpRegisters(size_t count = 64) {
        std::cout << "[dump] first " << count << " registers:" << std::endl;
        for (size_t i = 0; i < count; i += 16) {
            std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0') << i << ": ";
            for (size_t j = 0; j < 16 && i + j < count; ++j) {
                uint8_t val = valueFor(static_cast<uint8_t>(i + j));
                std::cout << std::setw(2) << static_cast<int>(val) << ' ';
            }
            std::cout << std::dec << std::endl;
        }
    }

private:
    uint8_t valueFor(uint8_t reg) {
        auto it = registers_.find(reg);
        if (it != registers_.end()) {
            return it->second;
        }
        if (cfg_.pattern == "random") {
            return static_cast<uint8_t>(dist_(rng_) & 0xFF);
        }
        if (cfg_.pattern == "ramp") {
            return static_cast<uint8_t>(reg);
        }
        // default incremental pattern with wrap
        return static_cast<uint8_t>((base_ + reg) & 0xFF);
    }

    bool maybeError() {
        if (cfg_.errorRate <= 0.0) return true;
        std::uniform_real_distribution<double> prob(0.0, 1.0);
        bool ack = prob(rng_) > cfg_.errorRate;
        if (cfg_.traceRaw && !ack) {
            std::cout << "[debug] injected NACK" << std::endl;
        }
        return ack;
    }

    void maybeDelay() {
        if (cfg_.latencyMs > 0) {
            if (cfg_.traceRaw) {
                std::cout << "[debug] inserting latency " << cfg_.latencyMs << "ms" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.latencyMs));
        }
    }

    void loadRegisters(const std::string &path) {
        std::ifstream in(path);
        if (!in) {
            std::cerr << "[warn] could not open preload file, continuing with defaults" << std::endl;
            return;
        }
        std::string line;
        uint8_t reg = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            registers_[reg++] = static_cast<uint8_t>(std::stoi(line, nullptr, 0));
        }
    }

    std::string bytesToString(const std::vector<uint8_t> &bytes) {
        std::ostringstream oss;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i) oss << ' ';
            oss << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }

    Config cfg_;
    std::map<uint8_t, uint8_t> registers_;
    std::mt19937 rng_;
    std::uniform_int_distribution<int> dist_{0, 255};
    uint8_t base_{0x10};
};

Config parseArgs(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pattern" && i + 1 < argc) {
            cfg.pattern = argv[++i];
        } else if (arg == "--error" && i + 1 < argc) {
            cfg.errorRate = std::stod(argv[++i]);
        } else if (arg == "--latency" && i + 1 < argc) {
            cfg.latencyMs = std::stoi(argv[++i]);
        } else if (arg == "--quiet") {
            cfg.verbose = false;
        } else if (arg == "--trace-raw") {
            cfg.traceRaw = true;
        } else if (arg == "--preload" && i + 1 < argc) {
            cfg.preloadFile = argv[++i];
        } else {
            std::cout << "Usage: " << argv[0]
                      << " [--pattern incremental|ramp|random]"
                      << " [--error <0..1>]"
                      << " [--latency <ms>]"
                      << " [--quiet]"
                      << " [--trace-raw]"
                      << " [--preload <file>]" << std::endl;
            std::exit(1);
        }
    }
    return cfg;
}

std::vector<uint8_t> parseBytes(const std::string &input) {
    std::vector<uint8_t> data;
    std::istringstream iss(input);
    std::string token;
    while (iss >> token) {
        int base = (token.rfind("0x", 0) == 0) ? 16 : 10;
        data.push_back(static_cast<uint8_t>(std::stoi(token, nullptr, base)));
    }
    return data;
}

int main(int argc, char **argv) {
    Config cfg = parseArgs(argc, argv);
    I2CSim sim(cfg);

    std::cout << "Interactive I2C debug shim (type 'help' for commands)" << std::endl;
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "quit" || cmd == "exit") break;
        else if (cmd == "help") {
            std::cout << "Commands:\n"
                      << "  read <addr> <reg> <len>\n"
                      << "  write <addr> <reg> <bytes...> (space separated decimal or 0xHEX)\n"
                      << "  scan\n"
                      << "  dump [count]\n"
                      << "  pattern <incremental|ramp|random>\n"
                      << "  error <rate> (0..1)\n"
                      << "  latency <ms>\n"
                      << "  help\n"
                      << "  exit" << std::endl;
        } else if (cmd == "read") {
            int addr, reg, len;
            if (!(iss >> std::hex >> addr >> reg >> std::dec >> len)) {
                std::cout << "usage: read <addr> <reg> <len>" << std::endl;
                continue;
            }
            TransactionResult res = sim.read(static_cast<uint8_t>(addr), static_cast<uint8_t>(reg), static_cast<size_t>(len));
            if (!res.ack) {
                std::cout << "  -> NACK" << std::endl;
            } else if (!cfg.verbose) {
                std::cout << "  -> " << res.payload.size() << " bytes" << std::endl;
            }
        } else if (cmd == "write") {
            int addr, reg;
            if (!(iss >> std::hex >> addr >> reg)) {
                std::cout << "usage: write <addr> <reg> <bytes...>" << std::endl;
                continue;
            }
            std::string rest;
            std::getline(iss, rest);
            auto data = parseBytes(rest);
            TransactionResult res = sim.write(static_cast<uint8_t>(addr), static_cast<uint8_t>(reg), data);
            if (!res.ack) std::cout << "  -> NACK" << std::endl;
        } else if (cmd == "scan") {
            sim.scanBus();
        } else if (cmd == "dump") {
            int count = 64;
            iss >> count;
            sim.dumpRegisters(static_cast<size_t>(count));
        } else if (cmd == "pattern") {
            std::string mode;
            if (!(iss >> mode)) {
                std::cout << "usage: pattern <incremental|ramp|random>" << std::endl;
                continue;
            }
            cfg.pattern = mode;
            std::cout << "[cfg] pattern set to " << mode << std::endl;
            sim.updateConfig(cfg);
        } else if (cmd == "error") {
            double rate;
            if (!(iss >> rate)) {
                std::cout << "usage: error <0..1>" << std::endl;
                continue;
            }
            cfg.errorRate = rate;
            std::cout << "[cfg] error rate set to " << rate << std::endl;
            sim.updateConfig(cfg);
        } else if (cmd == "latency") {
            int latency;
            if (!(iss >> latency)) {
                std::cout << "usage: latency <ms>" << std::endl;
                continue;
            }
            cfg.latencyMs = latency;
            std::cout << "[cfg] latency set to " << latency << "ms" << std::endl;
            sim.updateConfig(cfg);
        } else {
            std::cout << "unknown command: " << cmd << std::endl;
        }
    }

    std::cout << "bye" << std::endl;
    return 0;
}

