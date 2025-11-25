#!/usr/bin/env python3
"""
Lightweight MQTT debug broker.
Supports CONNECT, SUBSCRIBE, PUBLISH (QoS0), PINGREQ, and DISCONNECT.
Provides verbose logging knobs to help verify MQTT interactions without
touching the Raspberry Pi code under `rasp`.
"""
import argparse
import asyncio
import contextlib
import datetime
import logging
import secrets
import socket
from typing import Dict, Set, Tuple

MQTT_QOS0 = 0


def decode_var_int(stream: bytes) -> Tuple[int, int]:
    multiplier = 1
    value = 0
    consumed = 0
    for b in stream:
        consumed += 1
        value += (b & 0x7F) * multiplier
        if (b & 0x80) == 0:
            return value, consumed
        multiplier *= 128
        if multiplier > 128 * 128 * 128:
            raise ValueError("Malformed Remaining Length")
    raise ValueError("Incomplete Remaining Length")


def encode_var_int(value: int) -> bytes:
    encoded = bytearray()
    while True:
        byte = value % 128
        value //= 128
        if value > 0:
            byte |= 0x80
        encoded.append(byte)
        if value == 0:
            break
    return bytes(encoded)


def parse_utf8(stream: memoryview, pos: int) -> Tuple[str, int]:
    if pos + 2 > len(stream):
        raise ValueError("Truncated UTF-8 string")
    length = (stream[pos] << 8) + stream[pos + 1]
    pos += 2
    if pos + length > len(stream):
        raise ValueError("UTF-8 string length exceeds payload")
    return stream[pos : pos + length].tobytes().decode(), pos + length


class DebugBroker:
    def __init__(self, log_raw: bool, log_payload: bool, reflect: bool, disconnect_on_error: bool):
        self.subscriptions: Dict[str, Set[asyncio.StreamWriter]] = {}
        self.log_raw = log_raw
        self.log_payload = log_payload
        self.reflect = reflect
        self.disconnect_on_error = disconnect_on_error

    def _log_packet(self, client: str, direction: str, packet_type: str, raw: bytes):
        prefix = f"[{client}] {direction} {packet_type}"
        logging.info(prefix)
        if self.log_raw:
            logging.info("%s raw: %s", prefix, raw.hex())

    async def start(self, host: str, port: int):
        server = await asyncio.start_server(self._handle_client, host=host, port=port)
        logging.info("MQTT debug broker listening on %s:%d", host, port)
        async with server:
            await server.serve_forever()

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        addr = writer.get_extra_info("peername")
        client_id = f"client-{addr[0]}:{addr[1]}"
        logging.info("Accepted connection from %s", client_id)
        try:
            while True:
                fixed_header = await reader.readexactly(2)
                packet_type = fixed_header[0] >> 4
                flags = fixed_header[0] & 0x0F
                remaining_len, consumed = decode_var_int(fixed_header[1:])
                if consumed > 1:
                    payload = fixed_header[1 + consumed - 1 :]
                    payload += await reader.readexactly(remaining_len - len(payload))
                else:
                    payload = await reader.readexactly(remaining_len)
                packet = bytes(fixed_header[:1]) + encode_var_int(remaining_len) + payload
                await self._dispatch_packet(packet_type, flags, payload, writer, client_id)
                self._log_packet(client_id, "<=", self._packet_name(packet_type), packet)
        except asyncio.IncompleteReadError:
            logging.info("%s disconnected", client_id)
        except Exception as exc:  # pylint: disable=broad-except
            logging.exception("Error handling %s: %s", client_id, exc)
            if self.disconnect_on_error:
                writer.close()
                await writer.wait_closed()

    async def _dispatch_packet(self, packet_type: int, flags: int, payload: bytes, writer: asyncio.StreamWriter, client_id: str):
        if packet_type == 1:  # CONNECT
            await self._handle_connect(payload, writer, client_id)
        elif packet_type == 3:  # PUBLISH
            await self._handle_publish(flags, payload, client_id)
        elif packet_type == 8:  # SUBSCRIBE
            await self._handle_subscribe(payload, writer, client_id)
        elif packet_type == 12:  # PINGREQ
            await self._send_simple_packet(writer, 13, b"", client_id)
        elif packet_type == 14:  # DISCONNECT
            writer.close()
            await writer.wait_closed()
        else:
            logging.warning("%s sent unsupported packet type %d", client_id, packet_type)
            if self.disconnect_on_error:
                writer.close()
                await writer.wait_closed()

    async def _send_simple_packet(self, writer: asyncio.StreamWriter, packet_type: int, payload: bytes, client_id: str):
        fixed_header = bytes([(packet_type << 4), 0])
        writer.write(fixed_header + payload)
        await writer.drain()
        self._log_packet(client_id, "=>", self._packet_name(packet_type), fixed_header + payload)

    async def _handle_connect(self, payload: bytes, writer: asyncio.StreamWriter, client_id: str):
        mv = memoryview(payload)
        proto_name, pos = parse_utf8(mv, 0)
        proto_level = mv[pos]
        pos += 1
        connect_flags = mv[pos]
        pos += 1
        keep_alive = (mv[pos] << 8) + mv[pos + 1]
        pos += 2
        cid, pos = parse_utf8(mv, pos)
        logging.info(
            "CONNECT %s proto=%s level=%d keep_alive=%ds flags=0x%02x cid=%s",
            client_id,
            proto_name,
            proto_level,
            keep_alive,
            connect_flags,
            cid,
        )
        # Acknowledge connection (Connection Accepted)
        connack = bytes([0x20, 0x02, 0x00, 0x00])
        writer.write(connack)
        await writer.drain()
        self._log_packet(client_id, "=>", "CONNACK", connack)

    async def _handle_subscribe(self, payload: bytes, writer: asyncio.StreamWriter, client_id: str):
        mv = memoryview(payload)
        packet_id = (mv[0] << 8) + mv[1]
        pos = 2
        granted_qos = []
        while pos < len(mv):
            topic, pos = parse_utf8(mv, pos)
            qos = mv[pos]
            pos += 1
            granted_qos.append(min(qos, MQTT_QOS0))
            self.subscriptions.setdefault(topic, set()).add(writer)
            logging.info("%s subscribed to %s qos=%d", client_id, topic, qos)
        # SUBACK
        resp = bytes([0x90, 2 + len(granted_qos), packet_id >> 8, packet_id & 0xFF, *granted_qos])
        writer.write(resp)
        await writer.drain()
        self._log_packet(client_id, "=>", "SUBACK", resp)

    async def _handle_publish(self, flags: int, payload: bytes, client_id: str):
        dup = bool(flags & 0x08)
        retain = bool(flags & 0x01)
        mv = memoryview(payload)
        topic, pos = parse_utf8(mv, 0)
        message = mv[pos:].tobytes()
        readable = message.decode(errors="replace")
        logging.info(
            "PUBLISH %s dup=%s retain=%s topic=%s payload=%s",
            client_id,
            dup,
            retain,
            topic,
            readable if self.log_payload else f"{len(message)} bytes",
        )
        if self.reflect:
            await self._broadcast(topic, message, origin=client_id)

    async def _broadcast(self, topic: str, payload: bytes, origin: str):
        if topic not in self.subscriptions:
            return
        packet_id = secrets.randbelow(0xFFFF)
        fixed_header = bytes([0x30, 0])
        variable_header = len(topic).to_bytes(2, "big") + topic.encode()
        payload_bytes = variable_header + payload
        remaining = encode_var_int(len(payload_bytes))
        packet = bytes([0x30]) + remaining + payload_bytes
        for subscriber in list(self.subscriptions.get(topic, [])):
            try:
                subscriber.write(packet)
                await subscriber.drain()
                client_name = getattr(subscriber, "_client_id", "subscriber")
                self._log_packet(client_name, "=>", "PUBLISH", packet)
            except (ConnectionResetError, BrokenPipeError):
                logging.warning("Dropping stale subscriber for %s", topic)
                self.subscriptions[topic].discard(subscriber)

    def _packet_name(self, packet_type: int) -> str:
        names = {
            1: "CONNECT",
            3: "PUBLISH",
            8: "SUBSCRIBE",
            12: "PINGREQ",
            14: "DISCONNECT",
        }
        return names.get(packet_type, f"UNKNOWN({packet_type})")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Minimal MQTT debug broker")
    parser.add_argument("--host", default="0.0.0.0", help="Host to bind")
    parser.add_argument("--port", type=int, default=1883, help="TCP port to listen on")
    parser.add_argument("--log-raw", action="store_true", help="Dump raw packets in hex")
    parser.add_argument("--log-payload", action="store_true", help="Render payloads as UTF-8 when possible")
    parser.add_argument("--reflect", action="store_true", help="Rebroadcast any received publish to subscribers")
    parser.add_argument("--disconnect-on-error", action="store_true", help="Close connections on malformed packets")
    parser.add_argument("--timestamp", action="store_true", help="Prefix logs with timestamps")
    return parser.parse_args()


def configure_logging(use_timestamps: bool):
    if use_timestamps:
        logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    else:
        logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")


def main():
    args = parse_args()
    configure_logging(args.timestamp)
    broker = DebugBroker(
        log_raw=args.log_raw,
        log_payload=args.log_payload,
        reflect=args.reflect,
        disconnect_on_error=args.disconnect_on_error,
    )
    try:
        asyncio.run(broker.start(args.host, args.port))
    except KeyboardInterrupt:
        logging.info("Shutting down MQTT debug broker")
    except OSError as exc:
        logging.error("Failed to bind: %s", exc)


if __name__ == "__main__":
    main()
