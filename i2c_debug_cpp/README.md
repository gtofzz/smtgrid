# Ferramenta de Debug I2C (C++)

Utilitário em C++ para simular/depurar o lado STM no barramento I2C sem alterar o código em `rasp`.

## Opções de debug
- `--scan` varre todos os endereços válidos.
- `--read --reg=0x00 --bytes=4` lê registradores com logs opcionais verbosos.
- `--write --reg=0x10 --data=AA,BB,CC` envia bytes customizados.
- `--dump` realiza leitura contínua para acompanhar variações.
- `--verbose` habilita prefixos de dispositivo/endereço e ajuda a rastrear falhas.
- `--force` usa `I2C_SLAVE_FORCE` caso o dispositivo esteja em uso.

## Compilação
```bash
g++ -std=c++17 -Wall -Wextra -o i2c_debug_tool i2c_debug_tool.cpp
```

## Exemplos
Escanear o bus principal:
```bash
./i2c_debug_tool --device=/dev/i2c-1 --scan
```

Ler 4 bytes do endereço 0x20:
```bash
./i2c_debug_tool --addr=0x20 --read --reg=0x00 --bytes=4 --verbose
```

Loop de dump contínuo de um registrador:
```bash
./i2c_debug_tool --addr=0x20 --dump --reg=0x05 --bytes=2 --verbose
```
