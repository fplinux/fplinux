# Controlled MIDP network probe

This GPL fixture uses `Connector.open` and `HttpConnection`, with a Form UI.
Compile against the freshly built phoneME system `classes.zip`, not desktop
Java libraries. `build.sh` uses the existing selftest `javac -source 1.3
-target 1.3`, CLDC bootclasspath and preverifier recipe. It does not alter
the package recipe, VM, or build cache.

Run `sh build.sh OUTPUT_DIR [BASE_URL]` with `FPLINUX_PM_JDK`,
`FPLINUX_PM_PREVERIFY` and `FPLINUX_PM_CLASSES` set as for `../run.sh`.
The JDK must be the project's JDK 8; the preverifier may need the project's
configured launcher/environment. Output is `network.jar` and `network.jad`.
`NetProbe-Base-URL` in both the manifest and JAD defaults to
`http://10.0.2.2:39081`, the QEMU user-network address for host loopback.
An alternate port/base URL can be supplied as the second build argument.

Start the evidence `probe_server.py --port 39081` only after VM coordination.
The server binds `127.0.0.1`, logs JSON lines, and serves these cases:

| Command  | Requests                                  | Expected result                                     |
| -------- | ----------------------------------------- | --------------------------------------------------- |
| GET      | GET `/probe`                              | HTTP 200, `PM-NET-OK 20261006`                      |
| Redirect | GET `/redirect`, then GET `/probe`        | 302 with Location `/probe`, then 200 body above     |
| POST     | POST `/post`, body `pm-net-post=20261006` | HTTP 200, `PM-NET-POST-OK 20261006`                 |
| Slow     | GET `/slow`                               | First body bytes immediately; rest after 12 seconds |
| UI       | No network request                        | Increments UI counter even while Slow runs          |

Each network operation runs on a Java worker thread. Logs begin `PM-NET`.
Only one request runs at a time. Redirect follows one Location explicitly;
the test does not assume automatic redirection by MIDP. Controlled ASCII
responses are capped at 4096 characters. Error class/message appears in UI
and log. This fixture does not test TLS or an Opera Mini server.

For cancellation, start Slow, press UI to verify responsiveness, then press
the device Home while the server logs `slow_wait`. Confirm the Java process
and its socket disappear, and the server logs `peer_closed` during that wait.
The fixture deliberately performs no blocking I/O in lifecycle callbacks;
VM teardown must cancel/reclaim pending native work. A normal completed Slow
is a separate case and should display both `begin` and `end` lines.

Compilation alone is not evidence of a working Java network path. Run the
real GUI MIDlet and inspect both VM logs and server requests/results.
