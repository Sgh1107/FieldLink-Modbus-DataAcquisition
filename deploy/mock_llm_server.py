#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FieldLink AI Agent 测试用 Mock LLM 服务器（纯 Python 标准库，零依赖）

模拟 OpenAI 兼容的 /v1/chat/completions 端点，脚本化验证 Agent 循环：
  第 1 轮：返回 tool_calls（调用 get_system_status 工具）
  第 2 轮：messages 里已有 role:"tool" 结果 → 返回最终文本回答（包含收到的工具结果摘要）

同时校验请求形状：tools 是否为 OpenAI function 格式、工具结果是否按
role:"tool" + tool_call_id 回填。所有请求打印到 stdout 供测试断言。

用法：
  python deploy/mock_llm_server.py --port 11890
"""

from __future__ import annotations

import json
import struct  # noqa: F401  (保留给扩展用)
from http.server import BaseHTTPRequestHandler, HTTPServer


def log(message: str) -> None:
    print(message, flush=True)


class MockHandler(BaseHTTPRequestHandler):
    server_version = "FieldLinkMockLLM/1.0"

    def do_POST(self) -> None:
        if not self.path.endswith("/chat/completions"):
            self._send(404, {"error": {"message": f"unknown path {self.path}"}})
            return

        length = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(length))
        except json.JSONDecodeError as error:
            self._send(400, {"error": {"message": f"bad json: {error}"}})
            return

        model = body.get("model", "(none)")
        messages = body.get("messages", [])
        tools = body.get("tools", [])

        # ---- 形状校验与日志 ----
        has_tool_result = any(m.get("role") == "tool" for m in messages)
        log(f"REQ model={model} messages={len(messages)} tools={len(tools)} has_tool_result={has_tool_result}")
        for m in messages:
            role = m.get("role")
            if role == "tool":
                log(f"  tool-result id={m.get('tool_call_id')} content={str(m.get('content'))[:120]}")
            elif role == "assistant" and m.get("tool_calls"):
                for call in m["tool_calls"]:
                    fn = call.get("function", {})
                    log(f"  assistant-tool-call name={fn.get('name')} args={fn.get('arguments')}")

        if tools:
            first = tools[0]
            ok_shape = first.get("type") == "function" \
                and "name" in first.get("function", {}) \
                and "parameters" in first.get("function", {})
            log(f"  tools-shape={'OK' if ok_shape else 'BAD'} first={tools[0].get('function', {}).get('name')}")
        else:
            log("  tools-shape=EMPTY")

        # ---- 脚本化应答 ----
        if not has_tool_result:
            # 第 1 轮：让 Agent 去调用 get_system_status
            response = {
                "id": "chatcmpl-mock-1",
                "object": "chat.completion",
                "choices": [{
                    "index": 0,
                    "finish_reason": "tool_calls",
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [{
                            "id": "call_mock_1",
                            "type": "function",
                            "function": {
                                "name": "get_system_status",
                                "arguments": "{}"
                            }
                        }]
                    }
                }]
            }
            log("RESP tool_calls -> get_system_status")
        else:
            # 第 2 轮：拿到工具结果，给出最终回答
            tool_msg = next(m for m in messages if m.get("role") == "tool")
            response = {
                "id": "chatcmpl-mock-2",
                "object": "chat.completion",
                "choices": [{
                    "index": 0,
                    "finish_reason": "stop",
                    "message": {
                        "role": "assistant",
                        "content": f"Mock-FINAL: system status fetched, tool result={str(tool_msg.get('content'))[:160]}"
                    }
                }]
            }
            log("RESP final answer")

        self._send(200, response)

    def _send(self, code: int, obj: dict) -> None:
        payload = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args) -> None:  # 屏蔽默认访问日志，只保留结构化日志
        pass


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description="FieldLink Mock LLM 服务器（OpenAI 兼容）")
    parser.add_argument("--port", type=int, default=11890)
    args = parser.parse_args()

    server = HTTPServer(("127.0.0.1", args.port), MockHandler)
    log(f"Mock LLM 已启动 http://127.0.0.1:{args.port}/v1/chat/completions")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
