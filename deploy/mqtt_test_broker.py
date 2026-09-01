#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FieldLink MQTT 测试用迷你 broker（纯 Python 标准库，零依赖）

用途：没有 mosquitto/EMQX 等真实 broker 时，验证 FieldLink MQTT 发布端
      的协议行为（CONNECT / CONNACK / PUBLISH / PINGREQ / DISCONNECT）。
      支持订阅回环（SUBSCRIBE 后把匹配主题的 PUBLISH 转发回来）。

日志走 stdout，每收到一条 PUBLISH 打一行：
  PUBLISH topic=fieldlink/data/1/HoldingRegisters/0 retain=0 qos=0 bytes=123 payload={"timestamp":...}

用法：
  python deploy/mqtt_test_broker.py                       # 监听 0.0.0.0:1883
  python deploy/mqtt_test_broker.py --port 11883          # 指定端口
  python deploy/mqtt_test_broker.py --user test --pass secret   # 开启认证校验
"""

from __future__ import annotations

import argparse
import socket
import struct
import threading
import signal
import sys


def log(message: str) -> None:
    print(message, flush=True)


class MiniBroker:
    def __init__(self, host: str, port: int, username: str | None, password: str | None) -> None:
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        # 订阅表：socket -> set(topic)
        self.subscriptions: dict[socket.socket, set[str]] = {}
        self.lock = threading.Lock()
        self.running = True  # 新增：运行标志
        self.server_socket: socket.socket | None = None  # 新增：保存服务器socket引用

    # ---------- MQTT 编解码辅助 ----------

    @staticmethod
    def recv_exact(sock: socket.socket, size: int) -> bytes | None:
        buf = b""
        while len(buf) < size:
            try:
                chunk = sock.recv(size - len(buf))
            except (ConnectionError, OSError):
                return None
            if not chunk:
                return None
            buf += chunk
        return buf

    @staticmethod
    def recv_packet(sock: socket.socket) -> tuple[int, bytes] | None:
        """读取一个 MQTT 控制包，返回 (类型|标志, 包体)；连接关闭返回 None。"""
        first = MiniBroker.recv_exact(sock, 1)
        if first is None:
            log("RECV-EOF first-byte")
            return None
        type_flags = first[0]
        remaining = 0
        multiplier = 1
        while True:
            raw = MiniBroker.recv_exact(sock, 1)
            if raw is None:
                return None
            byte = raw[0]
            remaining += (byte & 0x7F) * multiplier
            multiplier *= 128
            if (byte & 0x80) == 0:
                break
        body = MiniBroker.recv_exact(sock, remaining)
        if body is None or len(body) != remaining:
            return None
        return type_flags, body

    @staticmethod
    def read_string(body: bytes, offset: int) -> tuple[str, int]:
        (length,) = struct.unpack_from(">H", body, offset)
        text = body[offset + 2:offset + 2 + length].decode("utf-8", errors="replace")
        return text, offset + 2 + length

    @staticmethod
    def encode_string(text: str) -> bytes:
        raw = text.encode("utf-8")
        return struct.pack(">H", len(raw)) + raw

    # ---------- 服务端行为 ----------

    def handle_client(self, sock: socket.socket, addr) -> None:
        client_id = "?"
        log(f"ACCEPT addr={addr[0]}:{addr[1]}")
        try:
            while self.running:  # 检查运行标志
                packet = self.recv_packet(sock)
                if packet is None:
                    break
                type_flags, body = packet
                packet_type = type_flags >> 4

                if packet_type == 1:  # CONNECT
                    name, offset = self.read_string(body, 0)
                    level = body[offset]
                    flags = body[offset + 1]
                    # 可变头：协议名(6) + level(1) + flags(1) + keepalive(2) → 跳过 4 字节
                    offset += 4
                    client_id, offset = self.read_string(body, offset)
                    if flags & 0x80:
                        username, offset = self.read_string(body, offset)
                    else:
                        username = None
                    if flags & 0x40:
                        password, offset = self.read_string(body, offset)
                    else:
                        password = None

                    # 认证校验（配置了 --user 时启用）
                    if self.username is not None and (username != self.username or password != self.password):
                        sock.sendall(b"\x20\x02\x00\x04")   # CONNACK 拒绝：用户名密码错误
                        log(f"CONNACK REJECT(4) client={client_id} addr={addr[0]}")
                        break

                    log(f"CONNECT client={client_id} proto={name} level={level} clean={bool(flags & 2)}")
                    sock.sendall(b"\x20\x02\x00\x00")        # CONNACK 接受
                    log(f"CONNACK ACCEPT client={client_id}")

                elif packet_type == 3:  # PUBLISH
                    qos = (type_flags >> 1) & 0x03
                    retain = type_flags & 0x01
                    topic, offset = self.read_string(body, 0)
                    if qos > 0:
                        offset += 2                          # 跳过 packet id
                    payload = body[offset:]
                    log(f"PUBLISH topic={topic} retain={retain} qos={qos} "
                        f"bytes={len(payload)} payload={payload.decode('utf-8', errors='replace')}")
                    self.forward(topic, payload)
                    if qos > 0:
                        (pid,) = struct.unpack_from(">H", body, 2 + len(topic.encode('utf-8')))
                        sock.sendall(struct.pack(">BBH", 0x40, 2, pid))

                elif packet_type == 8:  # SUBSCRIBE
                    (pid,) = struct.unpack_from(">H", body, 0)
                    offset = 2
                    granted = b""
                    topics = []
                    while offset < len(body):
                        topic, offset = self.read_string(body, offset)
                        offset += 1                          # 请求的 QoS
                        topics.append(topic)
                        granted += b"\x00"                   # 全部授予 QoS0
                    sock.sendall(struct.pack(">BB", 0x90, 2 + len(granted)) + struct.pack(">H", pid) + granted)
                    with self.lock:
                        self.subscriptions.setdefault(sock, set()).update(topics)
                    log(f"SUBSCRIBE client={client_id} topics={topics}")

                elif packet_type == 12:  # PINGREQ
                    log(f"PINGREQ client={client_id}")
                    sock.sendall(b"\xd0\x00")

                elif packet_type == 14:  # DISCONNECT
                    log(f"DISCONNECT client={client_id}")
                    break

        except (ConnectionError, OSError, struct.error, IndexError):
            pass
        finally:
            with self.lock:
                self.subscriptions.pop(sock, None)
            sock.close()

    def forward(self, topic: str, payload: bytes) -> None:
        """把 PUBLISH 转发给主题匹配的订阅者（支持 '+' 单层通配）。"""
        def matches(sub: str) -> bool:
            sub_parts = sub.split("/")
            pub_parts = topic.split("/")
            if len(sub_parts) != len(pub_parts):
                return False
            return all(s == "+" or s == p for s, p in zip(sub_parts, pub_parts))

        frame = b"\x30" + self._remaining(len(self.encode_string(topic)) + len(payload)) \
            + self.encode_string(topic) + payload
        with self.lock:
            targets = [sock for sock, subs in self.subscriptions.items()
                       if any(matches(s) for s in subs)]
        for sock in targets:
            try:
                sock.sendall(frame)
            except (ConnectionError, OSError):
                pass

    @staticmethod
    def _remaining(length: int) -> bytes:
        out = b""
        while True:
            digit = length % 128
            length //= 128
            if length > 0:
                digit |= 0x80
            out += bytes([digit])
            if length == 0:
                return out

    def stop(self) -> None:
        """停止broker"""
        log("正在停止 MQTT broker...")
        self.running = False
        # 关闭服务器socket以解除accept阻塞
        if self.server_socket:
            try:
                self.server_socket.close()
            except:
                pass

    def run(self) -> None:
        """启动broker主循环"""
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.bind((self.host, self.port))
        self.server_socket.listen(8)
        log(f"MQTT mini broker 已启动 {self.host}:{self.port} "
            f"(认证={'开启' if self.username else '关闭'})")
        log("按 Ctrl+C 停止服务")

        # 设置非阻塞模式以便检查运行标志
        self.server_socket.settimeout(1.0)

        while self.running:
            try:
                client, addr = self.server_socket.accept()
                # 检查是否在停止过程中
                if not self.running:
                    client.close()
                    break
                threading.Thread(target=self.handle_client, args=(client, addr), daemon=True).start()
            except socket.timeout:
                # 超时是为了能够检查running标志
                continue
            except OSError:
                # socket被关闭，跳出循环
                break


def signal_handler(broker: MiniBroker):
    """信号处理函数，用于优雅地停止broker"""
    def handler(signum, frame):
        log(f"\n收到终止信号 (Signal {signum})")
        broker.stop()
    return handler


def main() -> int:
    parser = argparse.ArgumentParser(description="FieldLink MQTT 测试迷你 broker")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--user", default=None, help="开启认证校验的用户名")
    parser.add_argument("--pass", dest="password", default=None, help="开启认证校验的密码")
    args = parser.parse_args()

    broker = MiniBroker(args.host, args.port, args.user, args.password)

    # 注册信号处理器
    signal.signal(signal.SIGINT, signal_handler(broker))
    signal.signal(signal.SIGTERM, signal_handler(broker))

    try:
        broker.run()
    except KeyboardInterrupt:
        # 双重保险，处理可能的KeyboardInterrupt
        log("\n收到 KeyboardInterrupt")
        broker.stop()
    finally:
        log("MQTT broker 已停止")
        # 关闭所有客户端连接
        with broker.lock:
            for sock in list(broker.subscriptions.keys()):
                try:
                    sock.close()
                except:
                    pass
            broker.subscriptions.clear()

    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())