#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FieldLink Modbus TCP 从站模拟器（纯 Python 标准库，零依赖）

用途：没有真实 Modbus 设备时，模拟一台从站供 FieldLink / MCP 工具测试。

寄存器布局（模拟一台"温度采集器"）：
  保持寄存器(FC03/06/16) 0-99 :
      reg0 = 模拟温度，正弦缓慢变化（放大10倍，如 250 = 25.0°C）→ 适合观察实时曲线/仪表盘
      reg1 = 随机游走值 → 适合触发报警阈值
      reg2 = 固定 42
      reg3..99 = 规律变化值
  输入寄存器(FC04) 0-99 : 与保持寄存器联动
  线圈(FC01/05/15) 0-31, 离散输入(FC02) 0-31 : discrete[0] 每 5 秒翻转

用法：
  python deploy/modbus_tcp_simulator.py                     # 监听 0.0.0.0:1502
  python deploy/modbus_tcp_simulator.py --port 502          # 标准 Modbus TCP 端口
  python deploy/modbus_tcp_simulator.py --unit 1            # 指定从站地址(默认1)
  python deploy/modbus_tcp_simulator.py --selftest          # 内置协议自检（不需要 FieldLink）
"""

from __future__ import annotations

import argparse
import math
import random
import socket
import struct
import threading
import time

# ---------------- 数据模型 ----------------

class DataStore:
    """模拟从站数据区，后台线程让数值持续变化，方便观察曲线与报警。"""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.holding: list[int] = [0] * 100
        self.input: list[int] = [0] * 100
        self.coils: list[bool] = [False] * 32
        self.discrete: list[bool] = [False] * 32
        self._t0 = time.time()
        self._walk = 1000
        threading.Thread(target=self._tick, daemon=True).start()

    def _tick(self) -> None:
        while True:
            t = time.time() - self._t0
            with self.lock:
                self.holding[0] = int(250 + 100 * math.sin(t / 30))      # 温度 15.0~35.0°C
                self._walk = max(0, min(5000, self._walk + random.randint(-40, 40)))
                self.holding[1] = self._walk
                self.holding[2] = 42
                for i in range(3, 100):
                    self.holding[i] = (i * 137 + int(t)) % 65536
                self.input[0] = self.holding[0]
                self.input[1] = self._walk % 65536
                self.discrete[0] = (int(t) % 10) < 5                     # 每 5 秒翻转
            time.sleep(0.5)

    def read_regs(self, table: list[int], addr: int, qty: int) -> list[int]:
        with self.lock:
            return table[addr:addr + qty]

    def write_reg(self, addr: int, value: int) -> None:
        with self.lock:
            self.holding[addr] = value & 0xFFFF

    def write_regs(self, addr: int, values: list[int]) -> None:
        with self.lock:
            for i, v in enumerate(values):
                self.holding[addr + i] = v & 0xFFFF

    def read_bits(self, table: list[bool], addr: int, qty: int) -> list[bool]:
        with self.lock:
            return table[addr:addr + qty]

    def write_bit(self, table: list[bool], addr: int, value: bool) -> None:
        with self.lock:
            table[addr] = value

    def write_bits(self, table: list[bool], addr: int, values: list[bool]) -> None:
        with self.lock:
            for i, v in enumerate(values):
                table[addr + i] = v


# ---------------- Modbus TCP 协议 ----------------

FC_READ_COILS = 0x01
FC_READ_DISCRETE = 0x02
FC_READ_HOLDING = 0x03
FC_READ_INPUT = 0x04
FC_WRITE_COIL = 0x05
FC_WRITE_REG = 0x06
FC_WRITE_COILS = 0x0F
FC_WRITE_REGS = 0x10

EX_ILLEGAL_FUNCTION = 0x01
EX_ILLEGAL_ADDRESS = 0x02
EX_ILLEGAL_VALUE = 0x03
EX_DEVICE_FAILURE = 0x04


def pack_bits(bits: list[bool]) -> bytes:
    """Modbus 位响应打包：每字节 8 位，LSB 为最低地址位。"""
    out = bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        if bit:
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


def handle_pdu(store: DataStore, pdu: bytes) -> bytes:
    """处理一帧 PDU（不含 MBAP），返回响应 PDU。"""
    if not pdu:
        return bytes([EX_ILLEGAL_FUNCTION | 0x80, EX_ILLEGAL_VALUE])
    fc = pdu[0]

    # ---- 读类型：addr(2) qty(2) ----
    if fc in (FC_READ_COILS, FC_READ_DISCRETE, FC_READ_HOLDING, FC_READ_INPUT):
        if len(pdu) < 5:
            return bytes([fc | 0x80, EX_ILLEGAL_VALUE])
        addr, qty = struct.unpack(">HH", pdu[1:5])
        if fc in (FC_READ_COILS, FC_READ_DISCRETE):
            if not (1 <= qty <= 2000):
                return bytes([fc | 0x80, EX_ILLEGAL_VALUE])
            table = store.coils if fc == FC_READ_COILS else store.discrete
            if addr + qty > len(table):
                return bytes([fc | 0x80, EX_ILLEGAL_ADDRESS])
            data = pack_bits(store.read_bits(table, addr, qty))
        else:
            if not (1 <= qty <= 125):
                return bytes([fc | 0x80, EX_ILLEGAL_VALUE])
            table = store.holding if fc == FC_READ_HOLDING else store.input
            if addr + qty > len(table):
                return bytes([fc | 0x80, EX_ILLEGAL_ADDRESS])
            regs = store.read_regs(table, addr, qty)
            data = struct.pack(f">{len(regs)}H", *regs)
        return bytes([fc, len(data)]) + data

    # ---- 写单线圈：addr(2) value(2) -> 原样回显 ----
    if fc == FC_WRITE_COIL:
        if len(pdu) < 5:
            return bytes([fc | 0x80, EX_ILLEGAL_VALUE])
        addr, value = struct.unpack(">HH", pdu[1:5])
        if addr >= len(store.coils):
            return bytes([fc | 0x80, EX_ILLEGAL_ADDRESS])
        store.write_bit(store.coils, addr, value == 0xFF00)
        return pdu[:5]

    # ---- 写单寄存器：addr(2) value(2) -> 原样回显 ----
    if fc == FC_WRITE_REG:
        if len(pdu) < 5:
            return bytes([fc | 0x80, EX_ILLEGAL_VALUE])
        addr, value = struct.unpack(">HH", pdu[1:5])
        if addr >= len(store.holding):
            return bytes([fc | 0x80, EX_ILLEGAL_ADDRESS])
        store.write_reg(addr, value)
        return pdu[:5]

    # ---- 写多线圈：addr(2) qty(2) bc(1) data -> addr(2) qty(2) ----
    if fc == FC_WRITE_COILS:
        if len(pdu) < 6:
            return bytes([fc | 0x80, EX_ILLEGAL_VALUE])
        addr, qty = struct.unpack(">HH", pdu[1:5])
        bc = pdu[5]
        if addr + qty > len(store.coils) or qty == 0 or bc != (qty + 7) // 8 or len(pdu) < 6 + bc:
            return bytes([fc | 0x80, EX_ILLEGAL_ADDRESS])
        values = [(pdu[6 + i // 8] >> (i % 8)) & 1 for i in range(qty)]
        store.write_bits(store.coils, addr, [bool(v) for v in values])
        return pdu[:5]

    # ---- 写多寄存器：addr(2) qty(2) bc(1) data -> addr(2) qty(2) ----
    if fc == FC_WRITE_REGS:
        if len(pdu) < 6:
            return bytes([fc | 0x80, EX_ILLEGAL_VALUE])
        addr, qty = struct.unpack(">HH", pdu[1:5])
        bc = pdu[5]
        if not (1 <= qty <= 123) or addr + qty > len(store.holding) \
                or bc != qty * 2 or len(pdu) < 6 + bc:
            return bytes([fc | 0x80, EX_ILLEGAL_ADDRESS])
        values = list(struct.unpack(f">{qty}H", pdu[6:6 + bc]))
        store.write_regs(addr, values)
        return pdu[:5]

    return bytes([fc | 0x80, EX_ILLEGAL_FUNCTION])


def serve_client(sock: socket.socket, store: DataStore, unit_id: int) -> None:
    try:
        while True:
            header = recv_exact(sock, 7)
            if header is None:
                break
            tid, pid, length, uid = struct.unpack(">HHHB", header)
            if pid != 0 or length < 2:
                break                                    # 非法帧，断开
            pdu = recv_exact(sock, length - 1)
            if pdu is None:
                break
            if unit_id != 0 and uid not in (unit_id, 0):
                continue                                 # 其他从站的请求，忽略
            resp_pdu = handle_pdu(store, pdu)
            resp = struct.pack(">HHHB", tid, 0, len(resp_pdu) + 1, uid) + resp_pdu
            sock.sendall(resp)
    except (ConnectionError, OSError):
        pass
    finally:
        sock.close()


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


# ---------------- 内置协议自检 ----------------

def selftest(port: int, unit: int) -> int:
    """不依赖 FieldLink，用内置客户端验证模拟器协议实现的正确性。"""
    store = DataStore()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(4)
    threading.Thread(target=accept_loop, args=(server, store, unit), daemon=True).start()
    time.sleep(0.3)

    failures = 0
    tid = 0

    def call(pdu: bytes) -> bytes:
        nonlocal tid
        tid += 1
        frame = struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu
        client.sendall(frame)
        header = recv_exact(client, 7)
        assert header is not None, "连接被关闭"
        _, _, length, _ = struct.unpack(">HHHB", header)
        resp = recv_exact(client, length - 1)
        assert resp is not None
        return resp

    client = socket.create_connection(("127.0.0.1", port), timeout=3)

    def check(name: str, cond: bool) -> None:
        nonlocal failures
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        if not cond:
            failures += 1

    # FC03 读保持寄存器
    resp = call(bytes([FC_READ_HOLDING]) + struct.pack(">HH", 0, 5))
    check("FC03 读保持寄存器 0-4", resp[0] == FC_READ_HOLDING and resp[1] == 10 and len(resp) == 12)
    reg2 = struct.unpack(">H", resp[6:8])[0]
    check("FC03 reg2 恒为 42", reg2 == 42)

    # FC06 写单寄存器 + 回读
    call(bytes([FC_WRITE_REG]) + struct.pack(">HH", 10, 1234))
    resp = call(bytes([FC_READ_HOLDING]) + struct.pack(">HH", 10, 1))
    check("FC06 写 1234 后回读一致", struct.unpack(">H", resp[2:4])[0] == 1234)

    # FC16 写多寄存器 + 回读
    call(bytes([FC_WRITE_REGS]) + struct.pack(">HHB", 20, 4, 8) + struct.pack(">4H", 11, 22, 33, 44))
    resp = call(bytes([FC_READ_HOLDING]) + struct.pack(">HH", 20, 4))
    check("FC16 写多寄存器回读一致",
          list(struct.unpack(">4H", resp[2:10])) == [11, 22, 33, 44])

    # FC04 读输入寄存器
    resp = call(bytes([FC_READ_INPUT]) + struct.pack(">HH", 0, 2))
    check("FC04 读输入寄存器", resp[0] == FC_READ_INPUT and resp[1] == 4)

    # FC05 写线圈 + FC01 回读
    call(bytes([FC_WRITE_COIL]) + struct.pack(">HH", 3, 0xFF00))
    resp = call(bytes([FC_READ_COILS]) + struct.pack(">HH", 0, 8))
    check("FC05 置位线圈 3 后回读", resp[0] == FC_READ_COILS and (resp[2] >> 3) & 1 == 1)

    # FC02 读离散输入
    resp = call(bytes([FC_READ_DISCRETE]) + struct.pack(">HH", 0, 4))
    check("FC02 读离散输入", resp[0] == FC_READ_DISCRETE)

    # 越界地址 → 异常 0x02
    resp = call(bytes([FC_READ_HOLDING]) + struct.pack(">HH", 200, 10))
    check("FC03 越界返回异常码 0x02", resp[0] == (FC_READ_HOLDING | 0x80) and resp[1] == EX_ILLEGAL_ADDRESS)

    # 非法功能 → 异常 0x01
    resp = call(bytes([0x2B, 0x0E])[:1] + b"\x0e")   # FC43(未实现)
    check("未实现功能返回异常码 0x01", resp[0] == 0xAB and resp[1] == EX_ILLEGAL_FUNCTION)

    client.close()
    server.close()
    print(f"自检完成：{'全部通过' if failures == 0 else f'{failures} 项失败'}")
    return 0 if failures == 0 else 1


def accept_loop(server: socket.socket, store: DataStore, unit: int) -> None:
    while True:
        try:
            client, addr = server.accept()
        except OSError:
            return
        threading.Thread(target=serve_client, args=(client, store, unit), daemon=True).start()


def main() -> int:
    parser = argparse.ArgumentParser(description="FieldLink Modbus TCP 从站模拟器")
    parser.add_argument("--port", type=int, default=1502, help="监听端口（默认 1502）")
    parser.add_argument("--unit", type=int, default=1, help="从站地址（默认 1）")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址（默认 0.0.0.0）")
    parser.add_argument("--selftest", action="store_true", help="运行内置协议自检后退出")
    args = parser.parse_args()

    if args.selftest:
        return selftest(args.port, args.unit)

    store = DataStore()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(8)
    print(f"Modbus TCP 从站模拟器已启动")
    print(f"  监听: {args.host}:{args.port}  从站地址: {args.unit}")
    print(f"  提示: FieldLink 中连接类型选 TCP，地址填 127.0.0.1:{args.port}，从站地址填 {args.unit}")
    print(f"  reg0=模拟温度(正弦)  reg1=随机游走  reg2=42  Ctrl+C 退出")
    try:
        while True:
            client, addr = server.accept()
            print(f"  客户端接入: {addr[0]}:{addr[1]}")
            threading.Thread(target=serve_client, args=(client, store, args.unit), daemon=True).start()
    except KeyboardInterrupt:
        print("\n模拟器已退出")
    return 0


if __name__ == "__main__":
    sys_exit = __import__("sys").exit
    sys_exit(main())
