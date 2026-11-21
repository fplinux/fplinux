# SPDX-License-Identifier: GPL-2.0-only
"""Run the production HTTP broker against a controlled loopback peer."""

from __future__ import annotations

import os
import socket
import time
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path
    from subprocess import Popen

APORT = ROOT / "alpine/aports/fplinux-mre"


@pytest.fixture(scope="module")
def http_broker(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "mre-http-broker") as directory:
        executable = directory / "http-broker"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{APORT}",
                str(ROOT / "tests/host_tool/mre/http-broker.c"),
                str(APORT / "brokers/http-broker.c"),
                "-lcurl",
            ],
            name="compile production MRE HTTP broker",
        )
        yield executable


@pytest.mark.parametrize(
    ("url_host", "request_host"),
    [
        ("127.0.0.1", "127.0.0.1"),
        ("local%EF%BD%88ost", "localhost"),  # U+FF48 is normalized by IDN.
    ],
)
def test_get_range_preserves_request_target_and_returns_final_headers_and_raw_content(
    http_broker: Path, url_host: str, request_host: str
) -> None:
    final_headers = (
        b"HTTP/1.1 206 Partial Content\r\n"
        b"Content-Range: bytes 3-6/10\r\n"
        b"Content-Encoding: gzip\r\n"
        b"Transfer-Encoding: chunked\r\n"
        b"Connection: close\r\n\r\n"
    )
    with socket.create_server(("127.0.0.1", 0)) as server:
        port = server.getsockname()[1]

        def reply(_process: Popen[str], deadline: float) -> None:
            server.settimeout(max(0, deadline - time.monotonic()))
            connection, _ = server.accept()
            with connection, connection.makefile("rb") as request:
                connection.settimeout(max(0, deadline - time.monotonic()))
                assert request.readline() == b"GET /a/../%2f?order=%7e HTTP/1.1\r\n"
                headers = []
                for line in iter(request.readline, b""):
                    if line == b"\r\n":
                        break
                    headers.append(line)
                assert b"Range: bytes=3-6\r\n" in headers
                assert f"Host: {request_host}:{port}\r\n".encode() in headers
                connection.sendall(
                    b"HTTP/1.1 103 Early Hints\r\nLink: </early>\r\n\r\n"
                    + final_headers
                    + b"4\r\ndata\r\n0\r\nX-Trailer: ignored\r\n\r\n"
                )

        result = run_process(
            [str(http_broker), f"http://{url_host}:{port}/a/../%2f?order=%7e"],
            name="MRE HTTP broker loopback Range request",
            timeout=10,
            env=os.environ | {"http_proxy": "http://127.0.0.1:1", "no_proxy": ""},
            while_running=reply,
            check=True,
        )

    assert result.stdout == ("1 0 206\n" + final_headers.decode().replace("\r\n", "\n") + "data")


def test_connection_refusal_reports_transport_error_without_http_status(http_broker: Path) -> None:
    with socket.socket() as unused_port:
        unused_port.bind(("127.0.0.1", 0))
        port = unused_port.getsockname()[1]
        result = run_process(
            [str(http_broker), f"http://127.0.0.1:{port}/"],
            name="MRE HTTP broker connection refusal",
            timeout=10,
            check=True,
        )

    assert result.stdout == "1 7 0\n"
