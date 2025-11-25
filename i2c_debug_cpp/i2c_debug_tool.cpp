#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

struct Options {
    std::string device = "/dev/i2c-1";
    bool scan = false;
    bool read = false;
    bool write = false;
    bool dump = false;
    bool verbose = false;
    bool force = false;
    int address = -1;
    int register_addr = 0;
    int bytes = 1;
    std::vector<uint8_t> write_buffer;
};

void PrintUsage(const char* prog) {
    std::cout << "I2C debug utility (simula STM)" << std::endl;
    std::cout << "Uso: " << prog << " [opções]\n";
    std::cout << "  --device=/dev/i2c-X   Seleciona o bus (default /dev/i2c-1)\n";
    std::cout << "  --addr=0x20           Endereço do dispositivo I2C\n";
    std::cout << "  --scan                Escaneia todos endereços válidos\n";
    std::cout << "  --read                Lê bytes a partir de um registrador\n";
    std::cout << "  --write               Escreve bytes no registrador\n";
    std::cout << "  --dump                Dump contínuo (loop) do registrador\n";
    std::cout << "  --reg=0x00            Registrador base para leitura/escrita\n";
    std::cout << "  --bytes=N             Quantidade de bytes (default 1)\n";
    std::cout << "  --data=AA,BB,...      Lista de bytes para escrita\n";
    std::cout << "  --verbose             Mais logs (endereços, erros, tempo)\n";
    std::cout << "  --force               Força ioctl(I2C_SLAVE_FORCE)\n";
}

int ParseHex(const std::string& hex) {
    return std::stoi(hex, nullptr, 0);
}

Options ParseArgs(int argc, char** argv) {
    Options opt;
    static struct option long_opts[] = {
        {"device", required_argument, 0, 'd'},
        {"addr", required_argument, 0, 'a'},
        {"scan", no_argument, 0, 's'},
        {"read", no_argument, 0, 'r'},
        {"write", no_argument, 0, 'w'},
        {"dump", no_argument, 0, 'D'},
        {"reg", required_argument, 0, 'g'},
        {"bytes", required_argument, 0, 'b'},
        {"data", required_argument, 0, 't'},
        {"verbose", no_argument, 0, 'v'},
        {"force", no_argument, 0, 'f'},
        {0, 0, 0, 0}};

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
            case 'd':
                opt.device = optarg;
                break;
            case 'a':
                opt.address = ParseHex(optarg);
                break;
            case 's':
                opt.scan = true;
                break;
            case 'r':
                opt.read = true;
                break;
            case 'w':
                opt.write = true;
                break;
            case 'D':
                opt.dump = true;
                break;
            case 'g':
                opt.register_addr = ParseHex(optarg);
                break;
            case 'b':
                opt.bytes = ParseHex(optarg);
                break;
            case 't': {
                std::stringstream ss(optarg);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    opt.write_buffer.push_back(static_cast<uint8_t>(ParseHex(token)));
                }
                break;
            }
            case 'v':
                opt.verbose = true;
                break;
            case 'f':
                opt.force = true;
                break;
            default:
                PrintUsage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    return opt;
}

int OpenBus(const Options& opt) {
    int fd = open(opt.device.c_str(), O_RDWR);
    if (fd < 0) {
        std::perror("Falha ao abrir bus");
        exit(EXIT_FAILURE);
    }
    return fd;
}

void SelectAddress(int fd, const Options& opt) {
    int request = opt.force ? I2C_SLAVE_FORCE : I2C_SLAVE;
    if (ioctl(fd, request, opt.address) < 0) {
        std::perror("Falha ao selecionar endereço I2C");
        exit(EXIT_FAILURE);
    }
}

void LogPrefix(const Options& opt, const std::string& action) {
    if (opt.verbose) {
        std::cout << "[" << opt.device << " addr=0x" << std::hex << opt.address << "] " << action << std::dec << std::endl;
    }
}

void ScanBus(const Options& opt) {
    int fd = OpenBus(opt);
    std::cout << "Scanning " << opt.device << "..." << std::endl;
    for (int addr = 0x03; addr <= 0x77; ++addr) {
        if (ioctl(fd, I2C_SLAVE, addr) < 0) continue;
        uint8_t buf = 0;
        if (write(fd, &buf, 1) == 1 || errno == EREMOTEIO) {
            std::cout << "  Found device at 0x" << std::hex << addr << std::dec << std::endl;
        }
    }
    close(fd);
}

std::vector<uint8_t> ReadRegister(int fd, const Options& opt) {
    uint8_t reg = static_cast<uint8_t>(opt.register_addr);
    if (write(fd, &reg, 1) != 1) {
        std::perror("Falha ao selecionar registrador");
        exit(EXIT_FAILURE);
    }
    std::vector<uint8_t> buf(opt.bytes, 0);
    if (read(fd, buf.data(), opt.bytes) != opt.bytes) {
        std::perror("Falha ao ler dados");
        exit(EXIT_FAILURE);
    }
    return buf;
}

void WriteRegister(int fd, const Options& opt) {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(opt.register_addr));
    buf.insert(buf.end(), opt.write_buffer.begin(), opt.write_buffer.end());
    if (write(fd, buf.data(), buf.size()) != static_cast<int>(buf.size())) {
        std::perror("Falha ao escrever dados");
        exit(EXIT_FAILURE);
    }
    LogPrefix(opt, "Escreveu " + std::to_string(opt.write_buffer.size()) + " bytes");
}

void DumpLoop(int fd, const Options& opt) {
    std::cout << "Dump contínuo (Ctrl+C para sair)" << std::endl;
    while (true) {
        auto data = ReadRegister(fd, opt);
        std::cout << "Reg 0x" << std::hex << opt.register_addr << ": ";
        for (uint8_t b : data) {
            std::cout << "0x" << std::setw(2) << std::setfill('0') << static_cast<int>(b) << " ";
        }
        std::cout << std::dec << std::endl;
        usleep(500000);
    }
}

int main(int argc, char** argv) {
    Options opt = ParseArgs(argc, argv);
    if (!opt.scan && !opt.read && !opt.write && !opt.dump) {
        PrintUsage(argv[0]);
        return 0;
    }

    if ((opt.read || opt.write || opt.dump) && opt.address < 0) {
        std::cerr << "Erro: informe --addr ao usar read/write/dump" << std::endl;
        return EXIT_FAILURE;
    }

    if (opt.write && opt.write_buffer.empty()) {
        std::cerr << "Erro: --write requer --data" << std::endl;
        return EXIT_FAILURE;
    }

    if (opt.scan) {
        ScanBus(opt);
    }

    if (opt.read || opt.write || opt.dump) {
        int fd = OpenBus(opt);
        SelectAddress(fd, opt);
        if (opt.write) {
            LogPrefix(opt, "Preparando escrita");
            WriteRegister(fd, opt);
        }
        if (opt.read) {
            LogPrefix(opt, "Lendo registrador");
            auto data = ReadRegister(fd, opt);
            std::cout << "Dados: ";
            for (uint8_t b : data) {
                std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ' ';
            }
            std::cout << std::dec << std::endl;
        }
        if (opt.dump) {
            DumpLoop(fd, opt);
        }
        close(fd);
    }
    return 0;
}
