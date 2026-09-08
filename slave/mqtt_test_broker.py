#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FieldLink MQTT 测试用迷你 broker（纯 Python 标准库，零依赖）

【使用方式】
  python slave/mqtt_test_broker.py                                  # 监听 0.0.0.0:1883
  python slave/mqtt_test_broker.py --port 11883                     # 指定端口
  python slave/mqtt_test_broker.py --user admin --pass admin123     # 开启认证校验

【连接方法】
  FieldLink 菜单 Advanced → MQTT Publishing，Broker 地址填 127.0.0.1:<端口>
  若 broker 开启了认证则填同样的用户名/密码 → 保存并连接。
  本脚本会把收到的每条 PUBLISH 打印到控制台，便于核对上送数据。

用途：没有 mosquitto/EMQX 等真实 broker 时，验证 FieldLink MQTT 发布端
      的协议行为（CONNECT / CONNACK / PUBLISH / PINGREQ / DISCONNECT）。
      支持订阅回环（SUBSCRIBE 后把匹配主题的 PUBLISH 转发回来）。

日志走 stdout，每收到一条 PUBLISH 打一行：
  PUBLISH topic=fieldlink/data/1/HoldingRegisters/0 retain=0 qos=0 bytes=123 payload={"timestamp":...}
"""

from __future__ import annotations

import argparse
import socket
import struct
import threading
import signal
import sys

# 客户端 socket 接收超时时间（秒），用于定期检查运行标志
CLIENT_SOCKET_TIMEOUT = 1.0

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
        self.running = True
        self.server_socket: socket.socket | None = None  # 保存服务器socket引用
        # 用于协调线程退出的停止事件
        self.stop_event = threading.Event()

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
        """ 读取一个 MQTT 控制包，返回 (类型|标志, 包体)；连接关闭返回 None """
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
        # 设置 socket 超时，使 recv 不会永久阻塞
        sock.settimeout(CLIENT_SOCKET_TIMEOUT)
        try:
            while self.running:
                try:
                    packet = self.recv_packet(sock)
                except socket.timeout:
                    # 超时是为了检查运行标志，继续循环
                    continue
                except (ConnectionError, OSError):
                    break
                
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
        """
        把 PUBLISH 转发给主题匹配的订阅者。
        支持 MQTT 通配符：
        - '+' 匹配单层
        - '#' 匹配多层（必须放在最后）
        """
        def matches(sub: str) -> bool:
            sub_parts = sub.split("/")
            pub_parts = topic.split("/")
            # 处理 # 通配符（必须出现在订阅主题的最后）
            if sub_parts[-1] == "#":
                # 如果订阅是 "sensors/#"，可以匹配 "sensors/temp" 和 "sensors/temp/inside"
                # 要求前面的部分必须完全匹配
                if len(sub_parts) - 1 > len(pub_parts):
                    return False
                # 检查前面的部分是否匹配（不包括最后的 #）
                for i in range(len(sub_parts) - 1):
                    if sub_parts[i] != "+" and sub_parts[i] != pub_parts[i]:
                        return False
                return True
            # 没有 # 通配符，层数必须相同
            if len(sub_parts) != len(pub_parts):
                return False
            # 逐层匹配，支持 + 通配符
            for s, p in zip(sub_parts, pub_parts):
                if s != "+" and s != p:
                    return False
            return True

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
        """停止 broker，设置停止事件并关闭服务器 socket"""
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
        if sys.platform == "win32":
            # Windows：SO_REUSEADDR 允许端口被重复绑定（旧实例仍占着端口时新实例
            # 也能 bind 成功，但连接会进到旧进程 → 表现为"连不上且无任何日志"）。
            # 改用独占绑定：端口被占用时 bind 直接失败并大声报错。
            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.server_socket.bind((self.host, self.port))
        except OSError as error:
            log(f"错误：端口 {self.port} 绑定失败（{error}）。")
            log("可能原因：端口已被占用——含此前未关闭的本脚本实例（任务管理器结束旧 python 进程）或 mosquitto 等。")
            return
        self.server_socket.listen(8)
        log(f"MQTT mini broker 已启动 {self.host}:{self.port} "
            f"(认证={'开启' if self.username else '关闭'})")
        log("按 Ctrl+C 停止服务")

        # 服务器 socket 也设置超时，定期检查停止事件
        self.server_socket.settimeout(CLIENT_SOCKET_TIMEOUT)

        while self.running and not self.stop_event.is_set():
            try:
                client, addr = self.server_socket.accept()
                # 快速检查是否在停止过程中
                if not self.running or self.stop_event.is_set():
                    client.close()
                    break
                # 为每个客户端创建线程，传递停止事件引用
                thread = threading.Thread(
                    target=self.handle_client, 
                    args=(client, addr), 
                    daemon=True
                )
                thread.start()
            except socket.timeout:
                # 超时是为了检查停止事件
                continue
            except OSError:
                # socket 被关闭，跳出循环
                break
        
        # 等待所有客户端线程结束（给它们一点时间）
        log("正在等待客户端断开...")
        # 由于客户端线程设置了超时，它们会在检查到 stop_event 后自行退出
        # 等待主线程中的所有非守护线程结束
        for thread in threading.enumerate():
            if thread is not threading.main_thread() and thread.daemon:
                thread.join(timeout=0.5)


def signal_handler(broker: MiniBroker):
    """ 信号处理函数用于优雅地停止broker """
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
