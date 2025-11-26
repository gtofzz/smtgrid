# Makefile raiz: compila todos os componentes usando configurações do arquivo "config"

CONFIG_FILE ?= $(abspath config)
-include $(CONFIG_FILE)

.PHONY: all debug_mqtt_server rasp clean

all: debug_mqtt_server rasp

# Compila o servidor MQTT simulado
# Permite alterar o caminho do arquivo de configuração ao invocar: make CONFIG_FILE=outro

debug_mqtt_server:
	$(MAKE) -C debug_mqtt_server all CONFIG_FILE=$(CONFIG_FILE)

# Compila os binários do nó Raspberry (simulado e real)
rasp:
	$(MAKE) -C rasp all CONFIG_FILE=$(CONFIG_FILE)

clean:
	$(MAKE) -C debug_mqtt_server clean CONFIG_FILE=$(CONFIG_FILE)
	$(MAKE) -C rasp clean CONFIG_FILE=$(CONFIG_FILE)
