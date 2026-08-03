import http.server
import ssl
import json
import uuid
import threading
import time
import os
import requests
from collections import deque
from http.server import BaseHTTPRequestHandler, HTTPServer

# --- CONFIGURATION ---
# Secrets live in cloud_siphon.env (gitignored), next to this file.
# Each line: KEY=value.  Lines starting with # are ignored.
def _load_env(path):
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                key, _, val = line.partition("=")
                key = key.strip()
                if key and key not in os.environ:
                    os.environ[key] = val.strip()
    except FileNotFoundError:
        pass

_load_env(os.path.join(os.path.dirname(os.path.abspath(__file__)), "cloud_siphon.env"))

OLLAMA_API_KEY  = os.environ.get("OLLAMA_API_KEY", "")
OLLAMA_BASE_URL = "https://ollama.com"
OLLAMA_MODEL    = "gemma4:31b"
LISTEN_PORT     = 443
CERT_FILE       = "/home/wouter/Development/fable-os/cert.pem"
KEY_FILE        = "/home/wouter/Development/fable-os/key.pem"

# Telegram bot configuration — set in cloud_siphon.env.
# Leave TELEGRAM_ALLOWED_CHAT_IDS empty to accept messages from any chat,
# or list specific chat IDs (comma-separated) to restrict access.
TELEGRAM_BOT_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
_raw_ids = os.environ.get("TELEGRAM_ALLOWED_CHAT_IDS", "")
TELEGRAM_ALLOWED_CHAT_IDS = [int(x.strip()) for x in _raw_ids.split(",") if x.strip()]
# ---------------------

# ======================================================================
# Telegram queue — shared between the poller thread and the HTTP handler
# ======================================================================

# Each entry is a dict: {"chat_id": int, "from": str, "text": str}
telegram_queue = deque()
telegram_lock  = threading.Lock()


def telegram_poller():
    """Background thread: long-poll Telegram getUpdates and queue messages."""
    if not TELEGRAM_BOT_TOKEN:
        print("[Siphon] TELEGRAM_BOT_TOKEN not set — Telegram polling disabled.")
        return

    base = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}"
    offset = None
    print(f"[Siphon] Telegram poller started.")

    while True:
        try:
            params = {"timeout": 30, "allowed_updates": ["message"]}
            if offset is not None:
                params["offset"] = offset

            resp = requests.get(f"{base}/getUpdates", params=params, timeout=35)
            data = resp.json()

            if not data.get("ok"):
                print(f"[Siphon/Telegram] getUpdates error: {data}")
                time.sleep(5)
                continue

            for update in data.get("result", []):
                offset = update["update_id"] + 1
                msg = update.get("message", {})
                text = msg.get("text", "").strip()
                if not text:
                    continue

                chat_id   = msg["chat"]["id"]
                from_name = msg.get("from", {}).get("first_name", "?")

                # Apply allowlist if configured
                if TELEGRAM_ALLOWED_CHAT_IDS and chat_id not in TELEGRAM_ALLOWED_CHAT_IDS:
                    print(f"[Siphon/Telegram] Ignored message from unlisted chat {chat_id}")
                    continue

                with telegram_lock:
                    telegram_queue.append({
                        "chat_id": chat_id,
                        "from":    from_name,
                        "text":    text,
                    })
                print(f"[Siphon/Telegram] Queued message from {from_name} (chat {chat_id}): {text[:60]}")

        except Exception as e:
            print(f"[Siphon/Telegram] Poller error: {e}")
            time.sleep(5)


# ======================================================================
# Typing indicator — renewed every 4 s while the kernel processes a message
# ======================================================================

# Maps chat_id (int) -> threading.Event. Set the event to stop renewal.
typing_stop  = {}
typing_tlock = threading.Lock()


def _typing_renewal(chat_id, stop_event):
    """Keep sending 'typing' action to Telegram every 4 s until stop_event.
    Automatically stops after 120 seconds so a stuck turn never hangs forever."""
    url     = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendChatAction"
    elapsed = 0
    while elapsed < 120 and not stop_event.wait(4):
        elapsed += 4
        try:
            requests.post(url, json={"chat_id": chat_id, "action": "typing"},
                          timeout=5)
        except Exception:
            pass


def start_typing(chat_id):
    """Send an immediate typing action and start the renewal thread."""
    if not TELEGRAM_BOT_TOKEN:
        return
    stop_typing(chat_id)   # cancel any previous renewal for this chat
    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendChatAction"
    try:
        requests.post(url, json={"chat_id": chat_id, "action": "typing"},
                      timeout=5)
    except Exception:
        pass
    ev = threading.Event()
    with typing_tlock:
        typing_stop[chat_id] = ev
    t = threading.Thread(target=_typing_renewal, args=(chat_id, ev),
                         name=f"typing-{chat_id}", daemon=True)
    t.start()


def stop_typing(chat_id):
    """Signal the typing renewal thread for this chat to stop."""
    with typing_tlock:
        ev = typing_stop.pop(chat_id, None)
    if ev:
        ev.set()


def telegram_send_message(chat_id, text):
    """Send a message via the Telegram Bot API. Returns (ok, error_str)."""
    if not TELEGRAM_BOT_TOKEN:
        return False, "TELEGRAM_BOT_TOKEN not configured"
    try:
        url  = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
        resp = requests.post(url, json={"chat_id": chat_id, "text": text}, timeout=15)
        data = resp.json()
        if data.get("ok"):
            return True, None
        return False, data.get("description", "unknown error")
    except Exception as e:
        return False, str(e)


# ======================================================================
# Anthropic <-> Ollama translation helpers
# ======================================================================

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
        text_parts   = []
        tool_calls   = []
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
                        "name":      block.get("name", ""),
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
                    "role":         "tool",
                    "tool_call_id": block.get("tool_use_id", ""),
                    "content":      str(tool_content),
                })

        if tool_results:
            result.extend(tool_results)
        elif tool_calls:
            result.append({
                "role":       "assistant",
                "content":    "\n".join(text_parts),
                "tool_calls": tool_calls,
            })
        else:
            result.append({"role": role, "content": "\n".join(text_parts)})

    return result


def translate_response_to_anthropic(ollama_res, requested_model):
    """
    Translates an Ollama chat response to an Anthropic-style response.
    Handles both plain text responses and tool_calls.
    """
    message       = ollama_res.get("message", {})
    content_blocks = []
    stop_reason    = "end_turn"

    tool_calls = message.get("tool_calls", [])
    if tool_calls:
        for tc in tool_calls:
            fn        = tc.get("function", {})
            arguments = fn.get("arguments", {})
            if isinstance(arguments, str):
                try:
                    arguments = json.loads(arguments)
                except Exception:
                    arguments = {}
            content_blocks.append({
                "type":  "tool_use",
                "id":    tc.get("id", f"toolu_{uuid.uuid4().hex[:24]}"),
                "name":  fn.get("name", ""),
                "input": arguments,
            })
        stop_reason = "tool_use"
    else:
        text = message.get("content", "")
        if text:
            content_blocks.append({"type": "text", "text": text})

    return {
        "id":            f"siphon-{uuid.uuid4().hex[:20]}",
        "type":          "message",
        "role":          "assistant",
        "content":       content_blocks,
        "model":         requested_model,
        "stop_reason":   stop_reason,
        "stop_sequence": None,
        "usage":         {"input_tokens": 0, "output_tokens": 0},
    }


# ======================================================================
# HTTP request handler
# ======================================================================

class CloudSiphonHandler(BaseHTTPRequestHandler):

    # ------------------------------------------------------------------
    # GET /v1/telegram/poll
    # ------------------------------------------------------------------
    def do_GET(self):
        if self.path == "/v1/telegram/poll":
            with telegram_lock:
                if telegram_queue:
                    msg = telegram_queue.popleft()
                    payload = {
                        "pending": True,
                        "chat_id": msg["chat_id"],
                        "from":    msg["from"],
                        "text":    msg["text"],
                    }
                    print(f"[Siphon/poll] Delivering message from chat {msg['chat_id']}")
                    start_typing(msg["chat_id"])
                else:
                    payload = {"pending": False}

            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    # ------------------------------------------------------------------
    # POST /v1/messages          — LLM proxy (Anthropic -> Ollama)
    # POST /v1/telegram/send     — send a Telegram message
    # ------------------------------------------------------------------
    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        post_data      = self.rfile.read(content_length)

        # ---- Telegram send ----------------------------------------
        if self.path == "/v1/telegram/send":
            try:
                req     = json.loads(post_data)
                chat_id = req.get("chat_id")
                text    = req.get("text", "")

                if chat_id is None or not text:
                    self.send_response(400)
                    self.end_headers()
                    self.wfile.write(b'{"error":"chat_id and text required"}')
                    return

                stop_typing(int(chat_id))
                ok, err = telegram_send_message(int(chat_id), text)
                if ok:
                    body = json.dumps({"ok": True}).encode("utf-8")
                    self.send_response(200)
                else:
                    body = json.dumps({"ok": False, "error": err}).encode("utf-8")
                    self.send_response(500)

                print(f"[Siphon/send] chat_id={chat_id} ok={ok}"
                      + (f" err={err}" if not ok else ""))
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(body)

            except Exception as e:
                print(f"[Siphon/send] Error: {e}")
                self.send_response(500)
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode("utf-8"))
            return

        # ---- LLM proxy (/v1/messages) -----------------------------
        if self.path == "/v1/messages":
            try:
                fable_req       = json.loads(post_data)
                raw_messages    = fable_req.get("messages", [])
                raw_tools       = fable_req.get("tools", [])
                requested_model = fable_req.get("model", "unknown")

                ollama_messages = translate_messages_to_ollama(raw_messages)
                ollama_tools    = translate_tools_to_ollama(raw_tools)

                ollama_payload = {
                    "model":    OLLAMA_MODEL,
                    "messages": ollama_messages,
                    "stream":   False,
                }
                if ollama_tools:
                    ollama_payload["tools"] = ollama_tools

                print(f"\n[Siphon] >>> {requested_model} -> {OLLAMA_MODEL}"
                      f"  tools={len(ollama_tools)}")
                full_url = f"{OLLAMA_BASE_URL}/api/chat"

                response = requests.post(
                    full_url,
                    headers={"Authorization": f"Bearer {OLLAMA_API_KEY}"},
                    json=ollama_payload,
                    timeout=60
                )

                print(f"[Siphon] <<< HTTP {response.status_code}")

                if response.status_code != 200:
                    print(f"[Siphon] Error: {response.text}")
                    self.send_response(response.status_code)
                    self.end_headers()
                    self.wfile.write(response.content)
                    return

                ollama_res    = response.json()
                anthropic_res = translate_response_to_anthropic(ollama_res, requested_model)

                n_tools = sum(1 for b in anthropic_res["content"]
                              if b.get("type") == "tool_use")
                print(f"[Siphon] stop={anthropic_res['stop_reason']} "
                      f"tool_use_blocks={n_tools}")

                body = json.dumps(anthropic_res).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(body)

            except Exception as e:
                print(f"[Siphon] Critical Error: {e}")
                self.send_response(500)
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode("utf-8"))
            return

        self.send_response(404)
        self.end_headers()

    def log_message(self, format, *args):
        pass  # suppress default access log noise


# ======================================================================
# Entry point
# ======================================================================

def run_server():
    # Start the Telegram polling thread before the HTTPS server.
    # It is a daemon thread so it dies automatically when the process exits.
    if TELEGRAM_BOT_TOKEN:
        t = threading.Thread(target=telegram_poller, name="telegram-poller",
                             daemon=True)
        t.start()
    else:
        print("[Siphon] Telegram disabled (set TELEGRAM_BOT_TOKEN to enable).")

    server_address = ("0.0.0.0", LISTEN_PORT)
    httpd = HTTPServer(server_address, CloudSiphonHandler)

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

    print(f"CloudSiphon SSL Bridge running on port {LISTEN_PORT}...")
    print(f"LLM:      {OLLAMA_MODEL}  @  {OLLAMA_BASE_URL}")
    print(f"Telegram: {'enabled' if TELEGRAM_BOT_TOKEN else 'disabled'}")
    try:
        httpd.serve_forever()
    except Exception as e:
        print(f"Server failed: {e}")


if __name__ == "__main__":
    run_server()
