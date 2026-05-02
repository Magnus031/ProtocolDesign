# IoPool Test Documentation

## Overview

This directory contains integration tests for the `IoPool` component provided by the
third-party GKC library (`third_party/GKC/RT/GkcSys/src/pool/IoPool.cpp`).

`IoPool` is the asynchronous I/O engine used by every network-facing component in this
project (AppHost, Gateway, Client).  It wraps Linux epoll into a thread pool and exposes
a C interface based on plain function-pointer callbacks.

### Files

| File | Purpose |
|------|---------|
| `io_pool_test.cpp` | All integration test scenarios |
| `BUILD` | Bazel build rule for the test binary |
| `test.md` | This document |

---

## Public API Under Test

The API is declared in `third_party/GKC/RT/GkcSys/public/_GkcSys.h`.

### Global functions

| Function | Description |
|----------|-------------|
| `_IoPool_Fetch()` | Return the process-global `LcInterface<_IIoPool>` singleton, creating it on first call |
| `_IoPool_Disable()` | Shut down all handles and stop the internal thread pool; irreversible in the same process |

### `_IIoPool` interface methods

| Method | Description |
|--------|-------------|
| `StartListen(port, ioFunc, ctx, cancelled)` | Bind and listen on *port*; returns a handle id |
| `StartConnect(host, port, ioFunc, ctx, cancelled)` | Initiate an outbound TCP connection; returns a handle id |
| `SetHandleFunc(id, ioFunc, ctx, cancelled)` | Replace the callback function and context for an existing handle |
| `BeginInput(id, len, cancelled)` | Acquire the send buffer (max 4000 bytes); returns `NULL` if a send is already in progress |
| `EndInput(id)` | Submit the buffer filled by `BeginInput` and trigger the send |
| `DisableHandle(id)` | Shut down a handle's socket (`SHUT_RDWR`) and enqueue it for deferred cleanup |

### `_IoFunc` callback event types

| Event constant | When fired | `uParam` |
|---------------|-----------|---------|
| `IO_TYPE_ACCEPT_INIT` | New inbound connection pending; return `1` to accept, `0` to reject | new handle id |
| `IO_TYPE_ACCEPTED` | Accepted connection is ready for I/O | — |
| `IO_TYPE_CONNECT_ERROR` | Outbound `StartConnect` failed | — |
| `IO_TYPE_CONNECTED` | Outbound `StartConnect` succeeded | — |
| `IO_TYPE_RECV_ERROR` | Receive-side socket error | — |
| `IO_TYPE_RECV_TIMEOUT` | No data received for `RECV_TIMEOUT` (5 minutes) | — |
| `IO_TYPE_RECEIVED` | Data arrived | `(_IoRecvInfo*)` with `.p` and `.len` |
| `IO_TYPE_SEND_ERROR` | Send-side socket error | — |
| `IO_TYPE_SENT` | `EndInput` data was fully delivered to the kernel | — |
| `IO_TYPE_BEFORE_CLOSE` | Deferred cleanup; fires `RECV_TIMEOUT` (5 min) after `DisableHandle` | — |

---

## Key Design Constraints

### Singleton lifecycle

`_IoPool_Fetch()` returns a process-global singleton.  Once `_IoPool_Disable()` is
called the pool is destroyed and **cannot be re-initialised** within the same process.
This means:

- All test scenarios must run inside **one `TEST()` function**.
- `_IoPool_Disable()` is called **exactly once** at the very end of that function.

This constraint mirrors the pattern already established in `src/apphost/apphost_test.cpp`.

### Accept callback flow

When a new connection arrives the pool calls the **listener's** callback with
`IO_TYPE_ACCEPT_INIT` and passes the new handle id as `uParam`.  The user is expected
to call `SetHandleFunc(new_id, conn_func, conn_ctx, cancelled)` inside that callback to
attach a per-connection function and context.  The pool then fires `IO_TYPE_ACCEPTED`
via the **connection's** callback (the one just assigned).

### Send lock (`iSend` flag)

`BeginInput` sets an atomic `iSend` flag to `1`.  A subsequent `BeginInput` on the same
handle before `EndInput` is called returns `NULL` (the send lock is held).  The flag is
cleared to `0` only after the `IO_TYPE_SENT` callback fires.

### `IO_TYPE_BEFORE_CLOSE` deferred timing

`DisableHandle` calls `SHUT_RDWR` on the socket immediately (so the remote peer sees
EOF at once), but it places the handle on a deferred close-list.  `IO_TYPE_BEFORE_CLOSE`
is fired only after `RECV_TIMEOUT` (5 minutes) has elapsed so that any in-flight I/O
can drain.  This event is therefore **not asserted** in the test suite.

---

## Test Scenarios

All scenarios use raw POSIX sockets as the "other end" of the connection, matching
the technique used in `src/apphost/apphost_test.cpp`.  Each scenario runs on a
distinct port to avoid `TIME_WAIT` collisions.

| Scenario | Port | What is tested |
|----------|------|---------------|
| S1 | — | Pool fetch |
| S2 | 19201 | Listen + Accept lifecycle |
| S3 | 19202 | Receive data |
| S4 | 19203 | Send data |
| S5 | 19204 | ACCEPT_INIT rejection |
| S6 | 19205 | DisableHandle |
| S7 | 19206 | BeginInput send-lock guard |
| S8 | 19207 | Multiple concurrent connections |
| S9 | 19208 | StartConnect (outbound) |

### S1 — Pool fetch

`_IoPool_Fetch()` must return a non-null interface.  A second call must return the
same context pointer, confirming there is only one singleton instance.

### S2 — Listen + Accept lifecycle

`StartListen` must succeed and return a non-zero handle id.  When a POSIX client
connects, the listener callback receives `IO_TYPE_ACCEPT_INIT` with a valid new handle
id.  After `SetHandleFunc` assigns the per-connection callback, `IO_TYPE_ACCEPTED` must
fire on the connection handle.

### S3 — Receive data

After a connection is established, the POSIX client sends a fixed 7-byte payload
(`DE AD BE EF 01 02 03`).  `IO_TYPE_RECEIVED` must fire and the bytes delivered via
`_IoRecvInfo` must exactly match the sent payload.

### S4 — Send data (BeginInput / EndInput)

`BeginInput` must return a non-null buffer pointer with no cancellation.  After copying
a 4-byte payload (`CA FE BA BE`) and calling `EndInput`, `IO_TYPE_SENT` must fire.  The
POSIX client must then receive those exact 4 bytes.

### S5 — ACCEPT_INIT rejection

When the listener callback returns `0` from `IO_TYPE_ACCEPT_INIT`, the pool must
immediately close the new socket.  A blocking `recv` on the POSIX client must return
`<= 0` (EOF or error) without the client ever receiving data.

### S6 — DisableHandle

After accepting a connection, calling `DisableHandle` on the connection handle must
cause the POSIX client to see EOF within the client's `SO_RCVTIMEO` timeout (3 s).
This confirms that `SHUT_RDWR` is applied synchronously.

### S7 — BeginInput send-lock guard

Two `BeginInput` calls on the same handle must not both succeed when no `EndInput` has
been called between them.  The second call must return `NULL` (send already locked).
After `EndInput` and `IO_TYPE_SENT`, `BeginInput` must succeed again, confirming the
lock is released by the sent notification.

### S8 — Multiple concurrent connections

Two POSIX clients connect to the same listener simultaneously.  Each must receive its
own `IO_TYPE_ACCEPTED` callback with an independent handle id.  Sending distinct
payloads from each client must result in each connection's callback receiving only its
own data (no cross-contamination).

### S9 — StartConnect (outbound)

A POSIX server is created with `socket`/`bind`/`listen` on port 19208.  The pool's
`StartConnect` is called to connect to it.  `IO_TYPE_CONNECTED` must fire.  The POSIX
server then sends 4 bytes; `IO_TYPE_RECEIVED` must fire with matching data.  The pool
side then sends 2 bytes back via `BeginInput`/`EndInput`; the POSIX server receives
them correctly.

---

## Running the Tests

```bash
# Build and run
bazel test //tests/IoPool:io_pool_test

# View full output
bazel test //tests/IoPool:io_pool_test --test_output=all
```

Expected result: `1 test passes` in under 2 seconds.

---

## Synchronisation Strategy

Each scenario uses a lightweight `Sem` (counting semaphore built on
`std::mutex` + `std::condition_variable`) to block the test thread until an
async IoPool callback fires.  All `wait_ms` calls use a 2-second timeout so
that a missing callback causes a clear test failure rather than an infinite hang.

The `ConnState` struct holds one `Sem` per significant event type (`accepted`,
`received`, `sent`, `recv_error`, `connect_error`) plus a mutex-protected
`recv_data` vector for byte-level assertions.

For S8, a separate `MultiListenState` with an atomic index dispatches each
incoming connection to its own `ConnState` slot.
