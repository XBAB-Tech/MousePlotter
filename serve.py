#!/usr/bin/env python3
"""Tiny local dev server that sets COOP/COEP headers.

Chrome needs these headers to unlock high-resolution timers (see the comment
in public/index.html). Serves the public/ directory on http://localhost:8000.

Usage: python3 serve.py [port]
"""

import http.server
import sys
from pathlib import Path

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
DIRECTORY = Path(__file__).parent / "public"


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DIRECTORY), **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


if __name__ == "__main__":
    with http.server.ThreadingHTTPServer(("", PORT), COOPCOEPHandler) as httpd:
        print(f"Serving {DIRECTORY} at http://localhost:{PORT} with COOP/COEP")
        httpd.serve_forever()
