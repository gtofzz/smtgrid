# MQTT Debug Server

Servidor MQTT mínimo para depuração, útil para validar a comunicação do lado do STM sem modificar nada em `rasp`.

## Recursos de depuração
- Logs opcionais de pacotes brutos em hexadecimal (`--log-raw`).
- Logs de payload decodificados como UTF-8 (`--log-payload`).
- Reflexão de mensagens: publica novamente para todos inscritos no mesmo tópico (`--reflect`).
- Encerramento imediato em pacotes malformados (`--disconnect-on-error`).
- Timestamps opcionais para cada linha de log (`--timestamp`).

## Uso rápido
```bash
python3 mqtt_debug_server.py \
  --host 0.0.0.0 --port 1883 \
  --log-raw --log-payload --reflect --timestamp
```

Conecte um cliente MQTT normal (p. ex. `mosquitto_pub`/`mosquitto_sub`). O broker aceita CONNECT, SUBSCRIBE, PUBLISH (QoS 0), PINGREQ e DISCONNECT e registra todos os eventos relevantes para debug.
