"""Host/static regression guards for the IDF v5.5.6 WebSocket handshake fix.

Background (docs/idf-5.5.6-upgrade.md §9): ESP-IDF v5.5.6 stopped invoking the
URI handler for the WebSocket handshake request (v5.2.1 did).  The firmware
relied on that call to

  * register the client in the heartbeat/broadcast list (`ws_add_fd`), and
  * run the Host/Origin guard for the upgrade request,

so on v5.5.6 the heartbeat task broadcast to an empty list (browser UI flapped
"connected/connecting/disconnected" every few seconds) and `/ws/curve` clients
were never attached.  The fix registers clients from
`httpd_uri_t::ws_post_handshake_cb` (Kconfig
``CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT``) and keeps a first-frame
lazy-attach fallback.

A second regression is guarded here as well: for zero-length CLOSE/PING frames a
second ``httpd_ws_recv_frame()`` call re-reads the *next* frame header and blocks
the single httpd task for ``recv_wait_timeout`` (10 s).  Browsers send a
zero-payload CLOSE on ``ws.close()``, so every UI reconnect used to stall the
whole HTTP server.

These are deliberately source-shape guards: removing the fix (or reintroducing
the unguarded calls) must fail the suite.  No ESP-IDF, Docker or hardware needed.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import script_loader as sl

ROOT = sl.ROOT
URI_WS_C = ROOT / "project_components/web_server/uri_modules/uri_ws.c"
CURVE_WS_C = ROOT / "components/curve_sampler/src/curve_ws.c"
SDKCONFIG_DEFAULTS = [
    ROOT / "sdkconfig.defaults.esp32s3",
    ROOT / "sdkconfig.defaults.esp32c3",
    ROOT / "sdkconfig.defaults.esp32",
    ROOT / "sdkconfig.defaults.esp32c5",
]


def read(path: Path) -> str:
    if not path.is_file():
        raise AssertionError(f"expected file missing: {path}")
    return path.read_text(encoding="utf-8")


def strip_c_comments(src: str) -> str:
    """Remove comments so negative guards only inspect executable code."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def function_body(src: str, name: str) -> str:
    """Return the body of the first C definition of ``name``.

    Finds a call/definition site ``name(...)``, requires the next non-space
    character to be ``{`` (calls are followed by ``;``/operators and skipped)
    and returns the balanced braces block.  Comments are stripped first.
    """
    src = strip_c_comments(src)
    pattern = re.compile(rf"\b{re.escape(name)}\s*\(")
    for match in pattern.finditer(src):
        # Find the matching ')' of the argument list.
        depth = 0
        close = -1
        for idx in range(match.end() - 1, len(src)):
            if src[idx] == "(":
                depth += 1
            elif src[idx] == ")":
                depth -= 1
                if depth == 0:
                    close = idx
                    break
        if close < 0:
            continue
        nxt = close + 1
        while nxt < len(src) and src[nxt] in " \t\r\n":
            nxt += 1
        # Tolerate attributes/macros between ')' and '{' only via '{' right after.
        if nxt >= len(src) or src[nxt] != "{":
            continue
        depth = 0
        for idx in range(nxt, len(src)):
            if src[idx] == "{":
                depth += 1
            elif src[idx] == "}":
                depth -= 1
                if depth == 0:
                    return src[nxt : idx + 1]
    raise AssertionError(f"definition of {name}() not found")


class WsHandshakeFixGuards(unittest.TestCase):
    def test_kconfig_post_handshake_cb_enabled_for_all_targets(self) -> None:
        for path in SDKCONFIG_DEFAULTS:
            src = read(path)
            self.assertIn(
                "CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y",
                src,
                f"{path.name}: IDF v5.5 WS handshake callback must be enabled",
            )
            # Must not be overridden to 'not set' later in the same defaults file.
            self.assertNotIn(
                "# CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT is not set",
                src,
                f"{path.name}: post-handshake callback support was disabled",
            )

    def test_uri_ws_registers_post_handshake_callback(self) -> None:
        src = read(URI_WS_C)
        stripped = strip_c_comments(src)
        # Function definition exists and is wired into the httpd_uri_t.
        self.assertRegex(
            stripped, r"ws_post_handshake_cb\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{"
        )
        self.assertIn(".ws_post_handshake_cb = ws_post_handshake_cb", stripped)
        # It must be compiled only when the Kconfig option is available.
        self.assertIn(
            "#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT",
            src,
            "post_handshake callback registration must be Kconfig-guarded",
        )

    def test_uri_ws_guard_and_registration_share_one_path(self) -> None:
        """Host/Origin guard + ws_add_fd must run inside ws_attach_client().

        The post-handshake callback must route through that helper so the CSWSH
        guard cannot be bypassed on IDF v5.5.6.
        """
        src = read(URI_WS_C)
        attach = function_body(src, "ws_attach_client")
        self.assertIn("web_host_guard_ws_ok(req)", attach)
        self.assertIn("ws_add_fd(req->handle", attach)

        cb = function_body(src, "ws_post_handshake_cb")
        self.assertIn("ws_attach_client(req)", cb)

        # Legacy IDF <= 5.2 path must route through the same guard as well.
        legacy = function_body(src, "ws_on_socket_open")
        self.assertIn("ws_attach_client(req)", legacy)

    def test_uri_ws_first_frame_lazy_attach_fallback(self) -> None:
        src = read(URI_WS_C)
        handler = function_body(src, "ws_req_handler")
        self.assertIn("ws_fd_registered", handler)
        self.assertIn("ws_add_fd(", handler)

        # Registration list must be idempotent: ws_add_fd() scans the existing
        # fd list before appending, so the hook + first-frame fallback cannot
        # register one client twice (heartbeat duplication / list overflow).
        add_fd = function_body(src, "ws_add_fd")
        self.assertIn("ws_ctx.valid_fd[i]", add_fd)
        self.assertIn("goto out", add_fd)

    def test_uri_ws_zero_length_close_ping_do_not_double_recv(self) -> None:
        src = read(URI_WS_C)
        stripped = strip_c_comments(src)
        # The old unguarded payload drain with max_len 126 must be gone.
        self.assertNotRegex(
            stripped,
            r"httpd_ws_recv_frame\s*\(\s*req\s*,\s*ws_pkt\s*,\s*126\s*\)",
        )
        handler = function_body(src, "ws_req_handler")
        segments = {
            "CONTINUE": handler.split("case HTTPD_WS_TYPE_CONTINUE:")[1].split(
                "case HTTPD_WS_TYPE_TEXT:"
            )[0],
            "CLOSE": handler.split("case HTTPD_WS_TYPE_CLOSE:")[1].split(
                "case HTTPD_WS_TYPE_PING:"
            )[0],
            "PING": handler.split("case HTTPD_WS_TYPE_PING:")[1].split(
                "case HTTPD_WS_TYPE_PONG:"
            )[0],
        }
        # Positive guard: every payload drain of a control/data frame checks
        # len > 0 first (a zero-length frame must not trigger a second recv).
        for name, case in segments.items():
            self.assertRegex(
                case,
                r"if\s*\(\s*ws_pkt->len\s*>\s*0U\b",
                f"{name} handling must only drain a payload when len > 0",
            )
            self.assertIn("httpd_ws_recv_frame(req, ws_pkt, ws_pkt->len)", case)

    def test_curve_ws_registers_post_handshake_callback(self) -> None:
        src = read(CURVE_WS_C)
        stripped = strip_c_comments(src)
        self.assertRegex(
            stripped,
            r"curve_ws_post_handshake_cb\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{",
        )
        self.assertIn(".ws_post_handshake_cb = curve_ws_post_handshake_cb", stripped)
        self.assertIn("#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT", src)

    def test_curve_ws_guard_and_registration_share_one_path(self) -> None:
        src = read(CURVE_WS_C)
        register = function_body(src, "curve_ws_register_req")
        self.assertIn("web_host_guard_ws_ok(req)", register)
        self.assertIn("curve_ws_attach(&s_ws, fd)", register)

        cb = function_body(src, "curve_ws_post_handshake_cb")
        self.assertIn("curve_ws_register_req(req)", cb)

        # Legacy handler path and the first-frame fallback route through the
        # same attach helper/state (no unregistered push clients).
        handler = function_body(src, "curve_ws_handler")
        self.assertIn("curve_ws_register_req(req)", handler)
        self.assertIn("curve_ws_attach(&s_ws, fd)", handler)
        self.assertIn("curve_ws_find(&s_ws, fd)", handler)

    def test_curve_ws_zero_length_close_ping_do_not_double_recv(self) -> None:
        src = read(CURVE_WS_C)
        stripped = strip_c_comments(src)
        self.assertNotIn("httpd_ws_recv_frame(req, &pkt, sizeof(close_payload))", stripped)
        self.assertNotIn("httpd_ws_recv_frame(req, &pkt, sizeof(ping_payload))", stripped)
        self.assertIn("if (pkt.len > 0U)", stripped)
        self.assertIn("if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK)", stripped)


if __name__ == "__main__":
    unittest.main()
