#!/usr/bin/env python3
"""LuaMD launcher -- sends lua/md.lua to the Luac0re loader.

    python md_launcher.py <PS5_IP>
    python md_launcher.py <PS5_IP> --log
    python md_launcher.py <PS5_IP> --payload lua/md.lua

ROMs are NOT uploaded by this script. They are read from a `roms/` folder
inside the game's savedata container and nowhere else -- put them there with
the same save manager used to install Luac0re, with the game closed. The
payload also serves FTP on port 1337 while it waits, which can write into the
container directly; see the README.

Before sending, arm the loader: launch the host game and open
OPTIONS -> HALL OF FAME.
"""

import argparse
import os
import socket
import sys
import threading

PAYLOAD_PORT = 9026
LOG_PORT = 9027


def send_payload(host, path, port=PAYLOAD_PORT):
    """The loader reads until the peer closes, then executes what it got."""
    with open(path, "rb") as f:
        data = f.read()

    print("Sending %s (%d bytes) to %s:%d" % (path, len(data), host, port))
    s = socket.create_connection((host, port), timeout=15)
    try:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)
    finally:
        s.close()
    print("Sent. The payload takes over the display within a second or two.")


def log_listener(port=LOG_PORT):
    """Prints the payload's UDP log.

    The payload sends with no framing, so one datagram is not necessarily one
    line. Printing them as they arrive is good enough for watching a boot.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    print("Listening for payload log on UDP %d (ctrl-c to stop)\n" % port)
    while True:
        data, _ = s.recvfrom(4096)
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description="Send LuaMD to a PS5 running Luac0re.")
    ap.add_argument("host", help="PS5 IP address")
    ap.add_argument("--payload", default=None,
                    help="path to md.lua (default: lua/md.lua next to this script)")
    ap.add_argument("--port", type=int, default=PAYLOAD_PORT)
    ap.add_argument("--log", action="store_true",
                    help="also listen for the payload's UDP log on 9027")
    args = ap.parse_args()

    payload = args.payload
    if payload is None:
        payload = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "lua", "md.lua")
    if not os.path.exists(payload):
        raise SystemExit("payload not found: %s\nRun `make` first." % payload)

    # Started before the send so the boot lines are not missed.
    if args.log:
        t = threading.Thread(target=log_listener, daemon=True)
        t.start()

    send_payload(args.host, payload, args.port)

    if args.log:
        try:
            t.join()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
