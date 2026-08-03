import http.server
import ssl
import json
import uuid
import requests
from http.server import BaseHTTPRequestHandler, HTTPServer

# --- CONFIGURATION ---
OLLAMA_API_KEY = "acd25ddd950d494ba8e637a9187b7c38.wbof3773LpDpMdEorp51478A"
OLLAMA_BASE_URL = "https://ollama.com"
OLLAMA_MODEL = "gemma4:31b"
LISTEN_PORT = 443
CERT_FILE = "/home/wouter/Development/fable-os/cert.pem"
KEY_FILE = "/home/wouter/Development/fable-os/key.pem"
# ---------------------


def translate_tools_to_ollama(tools):
    """
    Translates Anthropic tool definitions to Ollama tool definitions.

    Anthropic:
      {"name": ..., "description": ..., "input_schema": {"type": "object", ...}}

    Ollama:
      {"type": "function", "function": {"name": ..., "description": ..., "parameters": {...}}}
    """
    if not tools:
        return []
    result = []
    for t in tools:
        result.append({
            "type": "function",
            "function": {
                "name": t.get("name", ""),
                "description": t.get("description", ""),
                "parameters": t.get("input_schema", {"type": "object", "properties": {}}),
            }
        })
    return result


def translate_messages_to_ollama(messages):
    """
    Translates Anthropic-style messages to Ollama-style messages.

    Handles:
    - Plain text content (string or list of text blocks)
    - tool_use blocks in assistant messages -> tool_calls
    - tool_result blocks in user messages -> role "tool" messages
    """
    result = []
    for msg in messages:
        role = msg.get("role", "user")
        content_data = msg.get("content", "")

        if isinstance(content_data, str):
            result.append({"role": role, "content": content_data})
            continue

        # content_data is a list of blocks
        text_parts = []
        tool_calls = []
        tool_results = []

        for block in content_data:
            if not isinstance(block, dict):
                text_parts.append(str(block))
                continue

            btype = block.get("type", "text")

            if btype == "text":
                text_parts.append(block.get("text", ""))

            elif btype == "tool_use":
                # Anthropic assistant tool_use -> Ollama tool_calls
                tool_calls.append({
                    "id": block.get("id", f"call_{uuid.uuid4().hex[:8]}"),
                    "type": "function",
                    "function": {
                        "name": block.get("name", ""),
                        "arguments": block.get("input", {}),
                    }
                })

            elif btype == "tool_result":
                # Anthropic user tool_result -> Ollama role "tool" message
                tool_content = block.get("content", "")
                if isinstance(tool_content, list):
                    tool_content = "\n".join(
                        b.get("text", "") if isinstance(b, dict) else str(b)
                        for b in tool_content
                    )
                tool_results.append({
                    "role": "tool",
                    "tool_call_id": block.get("tool_use_id", ""),
                    "content": str(tool_content),
                })

        if tool_results:
            # tool_result blocks become separate tool-role messages
            result.extend(tool_results)
        elif tool_calls:
            # assistant message with tool calls
            combined_text = "\n".join(text_parts)
            ollama_msg = {
                "role": "assistant",
                "content": combined_text,
                "tool_calls": tool_calls,
            }
            result.append(ollama_msg)
        else:
            combined_text = "\n".join(text_parts)
            result.append({"role": role, "content": combined_text})

    return result


def translate_response_to_anthropic(ollama_res, requested_model):
    """
    Translates an Ollama chat response to an Anthropic-style response.

    Handles both plain text responses and tool_calls.
    """
    message = ollama_res.get("message", {})
    content_blocks = []
    stop_reason = "end_turn"

    tool_calls = message.get("tool_calls", [])
    if tool_calls:
        for tc in tool_calls:
            fn = tc.get("function", {})
            arguments = fn.get("arguments", {})
            # Ollama may return arguments as a JSON string
            if isinstance(arguments, str):
                try:
                    arguments = json.loads(arguments)
                except Exception:
                    arguments = {}
            content_blocks.append({
                "type": "tool_use",
                "id": tc.get("id", f"toolu_{uuid.uuid4().hex[:24]}"),
                "name": fn.get("name", ""),
                "input": arguments,
            })
        stop_reason = "tool_use"
    else:
        text = message.get("content", "")
        if text:
            content_blocks.append({"type": "text", "text": text})

    return {
        "id": f"siphon-{uuid.uuid4().hex[:20]}",
        "type": "message",
        "role": "assistant",
        "content": content_blocks,
        "model": requested_model,
        "stop_reason": stop_reason,
        "stop_sequence": None,
        "usage": {"input_tokens": 0, "output_tokens": 0},
    }


class CloudSiphonHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path == "/v1/messages":
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length)

            try:
                fable_req = json.loads(post_data)
                print(f"\n[Siphon] >>> Request received from FableOS")

                raw_messages = fable_req.get("messages", [])
                raw_tools    = fable_req.get("tools", [])
                requested_model = fable_req.get("model", "unknown")

                ollama_messages = translate_messages_to_ollama(raw_messages)
                ollama_tools    = translate_tools_to_ollama(raw_tools)

                ollama_payload = {
                    "model": OLLAMA_MODEL,
                    "messages": ollama_messages,
                    "stream": False,
                }
                if ollama_tools:
                    ollama_payload["tools"] = ollama_tools

                print(f"[Siphon] Mapping: {requested_model} -> {OLLAMA_MODEL}")
                print(f"[Siphon] Tools forwarded: {len(ollama_tools)}")
                full_url = f"{OLLAMA_BASE_URL}/api/chat"
                print(f"[Siphon] Forwarding to: {full_url}...")

                response = requests.post(
                    full_url,
                    headers={"Authorization": f"Bearer {OLLAMA_API_KEY}"},
                    json=ollama_payload,
                    timeout=60
                )

                print(f"[Siphon] <<< Cloud Response Code: {response.status_code}")

                if response.status_code != 200:
                    print(f"[Siphon] Error Body: {response.text}")
                    self.send_response(response.status_code)
                    self.end_headers()
                    self.wfile.write(response.content)
                    return

                ollama_res = response.json()
                anthropic_res = translate_response_to_anthropic(ollama_res, requested_model)

                stop_reason = anthropic_res["stop_reason"]
                n_tools = sum(1 for b in anthropic_res["content"] if b.get("type") == "tool_use")
                print(f"[Siphon] stop_reason={stop_reason}, tool_use blocks={n_tools}")

                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(anthropic_res).encode('utf-8'))
                print(f"[Siphon] Success: Response delivered to FableOS.")

            except Exception as e:
                print(f"[Siphon] Critical Error: {e}")
                self.send_response(500)
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode('utf-8'))
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass  # suppress default access log noise


def run_server():
    server_address = ('0.0.0.0', LISTEN_PORT)
    httpd = HTTPServer(server_address, CloudSiphonHandler)

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)

    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

    try:
        print(f"CloudSiphon SSL Bridge running on port {LISTEN_PORT}...")
        print(f"Targeting Model: {OLLAMA_MODEL}")
        print(f"Targeting URL: {OLLAMA_BASE_URL}")
        httpd.serve_forever()
    except Exception as e:
        print(f"Server failed: {e}")


if __name__ == "__main__":
    run_server()
