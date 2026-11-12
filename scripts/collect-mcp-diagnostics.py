#!/usr/bin/env python3
"""Read JSON-RPC responses from stdin; retain error reasons, never UI/arguments.

The server emits owned, sanitized reason text. This collector adds defense in
 depth for opaque IDs and explicit sensitive assignments; it is not a sanitizer
for arbitrary phone content. Do not feed raw ADB output into it.
"""
import json
import re
import sys


def reason(text):
    text = re.sub(r"(?i)\b(?:credential|serial|requestId|deviceId|leaseId|bootId|buffer|payload)\s*[:=]\s*[^\s,;]+", "[redacted]", text)
    text = re.sub(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F-]{27,}\b|\b[0-9a-fA-F]{32,}\b", "[redacted]", text)
    return text


def summarize(response):
    result = response.get("result", {})
    output = {"protocolError": bool(response.get("error")), "isError": bool(result.get("isError"))}
    if response.get("error"):
        error = response["error"]
        output["error"] = {"code": error.get("code"), "reason": reason(error.get("message", ""))}
    if result.get("isError"):
        output["reasons"] = [reason(c["text"]) for c in result.get("content", []) if c.get("type") == "text"]
        error = result.get("structuredContent", {}).get("error", {})
        output["chain"] = {k: reason(str(error[k])) for k in ("code", "phase", "recovery") if k in error}
    value = result.get("structuredContent", {}).get("value", {})
    action = value.get("action", value)
    if isinstance(action, dict) and isinstance(action.get("receipt"), dict):
        status = action["receipt"].get("inputStatus")
        output["inputStatus"] = status
        output["transport"] = action["receipt"].get("transport")
        output["reasons"] = [reason(w) for w in action.get("warnings", [])]
    return output


if __name__ == "__main__":
    for line in sys.stdin:
        print(json.dumps(summarize(json.loads(line))), flush=True)
