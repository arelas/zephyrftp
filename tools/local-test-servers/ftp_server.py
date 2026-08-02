#!/usr/bin/env python3
"""Throwaway local FTP/FTPS server for exercising FtpBackend against a
real server — per ARCHITECTURE.md's "Known gaps", the control
connection, PASV handling, the AUTH TLS upgrade, and actual transfers
had never been tried against a live server before this existed.

Shared by start-ftp.sh (plain) and start-ftps.sh (--tls) rather than
duplicated, since the only real difference is which handler class
pyftpdlib uses and whether a cert is required.
"""
import argparse
import os
import sys

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
from pyftpdlib.servers import FTPServer

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int, required=True)
parser.add_argument("--root", required=True)
parser.add_argument("--user", required=True)
parser.add_argument("--password", required=True)
parser.add_argument("--pid-file", required=True)
parser.add_argument("--passive-ports", required=True, help="e.g. 2130-2140")
parser.add_argument("--tls", action="store_true")
parser.add_argument("--certfile")
parser.add_argument("--legacy-list", action="store_true",
                     help="Disable MLSD/MLST so the server genuinely only understands "
                          "the older, non-standardized LIST format — for exercising "
                          "FtpBackend's real fallback trigger, not just its parser in "
                          "isolation.")
parser.add_argument("--no-pasv", action="store_true",
                     help="Disable PASV/EPSV so the server genuinely requires active "
                          "(PORT) mode — for exercising FtpBackend's real active-mode "
                          "fallback against a server that actually refuses PASV.")
args = parser.parse_args()

authorizer = DummyAuthorizer()
authorizer.add_user(args.user, args.password, args.root, perm="elradfmwMT")

if args.tls:
    if not args.certfile:
        sys.exit("--certfile is required with --tls")
    handler = TLS_FTPHandler
    handler.certfile = args.certfile
    # Explicit FTPS (AUTH TLS on the plain control port), matching what
    # FtpBackend actually implements — not implicit TLS-from-connect,
    # which is a different, older convention this app doesn't use.
    handler.tls_control_required = True
    handler.tls_data_required = True
else:
    handler = FTPHandler

handler.authorizer = authorizer

if args.legacy_list:
    # Removing these from proto_cmds makes the server reply with a real
    # 500/502 to MLSD/MLST, the same as a genuinely older server that
    # never implemented RFC 3659 — forces FtpBackend's LIST fallback to
    # trigger for real rather than only being unit-tested against
    # crafted sample lines.
    handler.proto_cmds.pop("MLSD", None)
    handler.proto_cmds.pop("MLST", None)

if args.no_pasv:
    # Same idea for active/PORT mode: a genuine 502 to PASV/EPSV, not a
    # client-side toggle, so FtpBackend's fallback is exercised against
    # a server that actually behaves this way.
    handler.proto_cmds.pop("PASV", None)
    handler.proto_cmds.pop("EPSV", None)

lo, hi = (int(x) for x in args.passive_ports.split("-"))
handler.passive_ports = range(lo, hi)
# Loopback-only masquerade — without this pyftpdlib can advertise a PASV
# address other than 127.0.0.1 on some setups, which a same-machine
# client would still reach, but pin it explicitly rather than rely on
# that.
handler.masquerade_address = "127.0.0.1"

server = FTPServer(("127.0.0.1", args.port), handler)

with open(args.pid_file, "w") as f:
    f.write(str(os.getpid()))

server.serve_forever()
