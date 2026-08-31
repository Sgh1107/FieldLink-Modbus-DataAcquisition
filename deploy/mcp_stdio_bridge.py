#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FieldLink MCP stdio 桥接脚本

作用：把只支持 stdio 传输的 MCP 客户端（Claude Desktop / Cursor 等）
      桥接到 FieldLink 内置的 Streamable HTTP MCP 服务器。

  AI 客户端 ⇄ (stdio, 按行分隔 JSON-RPC) ⇄ 本脚本 ⇄ (HTTP POST /mcp) ⇄ FieldLink

仅使用 Python 标准库，无需 pip install。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8")  # Windows 控制台默认 GBK，统一 UTF-8
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass


def log(message: str) -> None:
    """日志走 stderr，stdout 严格保留给 MCP 协议。"""
    print(f"[fieldlink-mcp-bridge] {message}", file=sys.stderr, flush=True)


def forward(payload: bytes, base_url: str, token: str, timeout: float) -> bytes | None:
    """把一条 JSON-RPC 消息 POST 到 FieldLink /mcp，返回响应体（通知返回 None）。"""
    headers = {"Content-Type": "application/json"}
    if token:
        headers["X-Api-Token"] = token
        headers["Authorization"] = f"Bearer {token}"

    request = urllib.request.Request(base_url, data=payload, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read() or None
    except urllib.error.HTTPError as error:
        body = error.read()
        # 4xx 时服务器一般会带 JSON-RPC error 体，原样回传给客户端
        if body:
            return body
        if error.code == 401:
            return json.dumps({
                "jsonrpc": "2.0", "id": None,
                "error": {"code": -32000, "message": f"HTTP {error.code}: 鉴权失败，请检查 --token 与 FieldLink 中输入的 Token 是否一致"}
            }).encode("utf-8")
        return json.dumps({
            "jsonrpc": "2.0", "id": None,
            "error": {"code": -32000, "message": f"HTTP {error.code}: {error.reason}"}
        }).encode("utf-8")
    except (urllib.error.URLError, OSError) as error:
        return json.dumps({
            "jsonrpc": "2.0", "id": None,
            "error": {"code": -32000, "message": f"无法连接 FieldLink MCP 服务: {error}（请确认程序已启动并开启 MCP 服务）"}
        }).encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="FieldLink MCP stdio -> HTTP 桥")
    parser.add_argument("--host", default=os.environ.get("FIELDLINK_MCP_HOST", "127.0.0.1"),
                        help="FieldLink 所在主机（默认 127.0.0.1）")
    parser.add_argument("--port", type=int, default=int(os.environ.get("FIELDLINK_MCP_PORT", "8180")),
                        help="FieldLink MCP 端口（默认 8180）")
    parser.add_argument("--token", default=os.environ.get("FIELDLINK_MCP_TOKEN", ""),
                        help="API Token，与启动 MCP 服务时在 FieldLink 中输入的一致")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="单次请求超时秒数（Modbus 读写可能较慢，默认 30）")
    args = parser.parse_args()

    base_url = f"http://{args.host}:{args.port}/mcp"
    log(f"桥接已就绪: stdio <-> {base_url} (token={'已配置' if args.token else '未配置'})")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        # 取 id 用于失败时回错；通知无需响应
        request_id = None
        try:
            parsed = json.loads(line)
            if isinstance(parsed, dict) and "id" in parsed:
                request_id = parsed["id"]
        except json.JSONDecodeError:
            request_id = None

        response = forward(line.encode("utf-8"), base_url, args.token, args.timeout)
        if response:
            sys.stdout.write(response.decode("utf-8", errors="replace").strip() + "\n")
            sys.stdout.flush()
        # 202（通知）无响应体，不写 stdout
        _ = request_id  # 失败回错已由 forward 生成的 error 响应覆盖

    log("stdin 已关闭，桥接退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
