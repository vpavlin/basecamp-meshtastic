#!/usr/bin/env python3
"""Meshtastic sidecar for the Basecamp `meshtastic_gateway` core module.

Mirrors how the stoa van bridge talks to the radio (see stoa/rpi/stoa/mesh.py): connect to a
Meshtastic node over USB serial (or TCP), stream its channels / node DB / incoming text to the
C++ gateway as JSON lines on stdout, and accept commands (send) as JSON lines on stdin. The C++
side owns relay/dedup/topic-derivation; this process only does the radio I/O.

Wire protocol — one compact JSON object per line.
  stdout (helper -> gateway):
    {"type":"status",   "present":bool, "nodeName":str}
    {"type":"channels", "channels":[{"channelIndex":int,"name":str,"psk":<b64>,"role":str}]}
    {"type":"nodes",    "total":int, "online":int}
    {"type":"message",  "channelIndex":int, "from":str, "text":str, "ts":int}
    {"type":"log",      "msg":str}
  stdin (gateway -> helper):
    {"cmd":"send", "channelIndex":int, "text":str}

Connection target (first match):
  --host / $MESHTASTIC_HOST   -> TCPInterface(hostname)     (a networked/WiFi node)
  --dev  / $MESHTASTIC_DEV    -> SerialInterface(devPath)   (a specific USB device, by-id path)
  (neither)                   -> SerialInterface()          (autodetect the first USB radio)

Requires the `meshtastic` Python package (pip install meshtastic) on the host running the gateway.
"""
import argparse
import base64
import json
import os
import sys
import threading
import time

ROLE = {0: "DISABLED", 1: "PRIMARY", 2: "SECONDARY"}


def emit(obj):
    """Write one JSON line to stdout and flush (the gateway reads line-by-line)."""
    try:
        sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
        sys.stdout.flush()
    except Exception:
        pass


class Helper:
    def __init__(self, dev=None, host=None, interval=10):
        self.dev = dev
        self.host = host
        self.interval = interval
        self.iface = None
        self._subscribed = False
        self._stop = threading.Event()

    # --- connection ------------------------------------------------------
    def connect(self):
        try:
            from pubsub import pub
            if self.host:
                import meshtastic.tcp_interface
                iface = meshtastic.tcp_interface.TCPInterface(hostname=self.host)
            else:
                import meshtastic.serial_interface
                # devPath=None lets the library autodetect the first attached radio.
                iface = meshtastic.serial_interface.SerialInterface(devPath=self.dev)
            if not self._subscribed:
                pub.subscribe(self._on_receive, "meshtastic.receive")
                self._subscribed = True
            self.iface = iface
            return True
        except Exception as e:  # transient on radio reboot / USB settle / missing lib
            emit({"type": "log", "msg": "connect failed: %s" % e})
            return False

    def _present(self):
        # by-id symlink vanishes on unplug (matches stoa's _present check)
        return (not self.dev) or os.path.exists(self.dev)

    # --- inbound text ----------------------------------------------------
    def _on_receive(self, packet, interface):
        try:
            d = packet.get("decoded", {})
            if d.get("portnum") != "TEXT_MESSAGE_APP":
                return
            text = d.get("text")
            if not text:
                return
            ch = packet.get("channel", 0)
            frm = packet.get("fromId")
            name = frm
            try:
                u = ((interface.nodes or {}).get(frm) or {}).get("user") or {}
                name = u.get("shortName") or u.get("longName") or frm
            except Exception:
                pass
            emit({"type": "message", "channelIndex": ch,
                  "from": name or "?", "text": text, "ts": int(time.time())})
        except Exception as e:
            emit({"type": "log", "msg": "rx error: %s" % e})

    # --- pushers ---------------------------------------------------------
    def push_channels(self):
        try:
            chans = []
            for ch in self.iface.localNode.channels:
                if ch.role == 0:                       # DISABLED slot
                    continue
                psk = bytes(ch.settings.psk or b"")
                chans.append({
                    "channelIndex": ch.index,
                    "name": ch.settings.name or "",
                    "psk": base64.b64encode(psk).decode(),
                    "role": ROLE.get(ch.role, "SECONDARY"),
                })
            emit({"type": "channels", "channels": chans})
        except Exception as e:
            emit({"type": "log", "msg": "channels error: %s" % e})

    def push_nodes(self):
        try:
            nodes = self.iface.nodes or {}
            now = time.time()
            online = sum(1 for n in nodes.values()
                         if n.get("lastHeard") and now - n["lastHeard"] < 7200)
            emit({"type": "nodes", "total": len(nodes), "online": online})
        except Exception as e:
            emit({"type": "log", "msg": "nodes error: %s" % e})

    def push_status(self):
        present = self.iface is not None and self._present()
        name = ""
        if present:
            try:
                u = (self.iface.getMyNodeInfo() or {}).get("user", {}) or {}
                name = u.get("longName") or u.get("shortName") or ""
            except Exception:
                pass
        emit({"type": "status", "present": present, "nodeName": name})

    # --- outbound (commands from the gateway) ----------------------------
    def send(self, channel_index, text):
        if self.iface is None:
            emit({"type": "log", "msg": "send dropped — not connected"})
            return
        try:
            self.iface.sendText(text, channelIndex=int(channel_index))
            emit({"type": "log", "msg": "tx ch%s: %s" % (channel_index, text)})
        except Exception as e:
            emit({"type": "log", "msg": "tx error: %s" % e})

    def _stdin_reader(self):
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                cmd = json.loads(line)
            except Exception:
                continue
            if cmd.get("cmd") == "send":
                self.send(cmd.get("channelIndex", 0), cmd.get("text", ""))

    # --- main loop -------------------------------------------------------
    def run(self):
        threading.Thread(target=self._stdin_reader, name="stdin", daemon=True).start()
        while not self._stop.is_set():
            if self.iface is not None and not self._present():
                # radio unplugged — drop and report
                try:
                    self.iface.close()
                except Exception:
                    pass
                self.iface = None
                self.push_status()
            elif self.iface is None:
                if self.connect():
                    self.push_status()
                    self.push_channels()
                    self.push_nodes()
                else:
                    self.push_status()
            else:
                # periodic refresh so channel edits / node churn show up
                self.push_status()
                self.push_channels()
                self.push_nodes()
            self._stop.wait(self.interval)


def main():
    ap = argparse.ArgumentParser(description="Meshtastic sidecar for the Basecamp gateway")
    ap.add_argument("--dev", default=os.environ.get("MESHTASTIC_DEV") or None,
                    help="serial device path (by-id); omit to autodetect")
    ap.add_argument("--host", default=os.environ.get("MESHTASTIC_HOST") or None,
                    help="TCP host of a networked Meshtastic node (overrides --dev)")
    ap.add_argument("--interval", type=int, default=10, help="refresh seconds")
    a = ap.parse_args()
    try:
        Helper(dev=a.dev, host=a.host, interval=a.interval).run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
