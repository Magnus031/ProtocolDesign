# CLAUDE.md — ProtocolDesign

## Project Overview

Graduation project: a binary protocol and runtime for streaming UI (pixel data, input events)
between a Windows client and a Linux server (Gateway + AppHost).

Components:
- `src/protocol/`  — wire protocol: serialization, MessageParser
- `src/gateway/`   — Linux server: accepts client connections, manages sessions and AppHost processes
- `src/apphost/`   — Linux side: hosts the actual application, captures pixel diffs, sends PIXEL_DATA
- `src/client/`    — Windows side: renders frames, captures input, sends INPUT_EVENT
- `src/common/`    — shared utilities (byte order, logging, types)
- Network I/O is handled directly via GKC `IoPool` (epoll on Linux, IOCP on Windows); no separate `src/network/` module.

## Before Starting Any Task

**Always read the relevant docs before writing or modifying code.**

- Architecture overview: `docs/design/architecture.md`
- Protocol specification: `docs/design/protocol_spec.md`
- Component-specific design docs in `docs/design/` if one exists for the area you are touching
- API reference: `docs/api/API_reference.md`

Do not proceed with implementation until you have confirmed you have read the relevant documents.

## Build & Test

Build system: Bazel 7.4.1. Always use `bazel build` / `bazel test` — do not use cmake or make directly.

Before running any build or test command, first identify the current OS/environment and choose targets for that side only. The project is intentionally split after M6:

- **Windows side**: Client executable and viewer plugin work.
  - Primary areas: `src/client/`, `plugins/client_viewer/`, client-facing shared code in `src/common/` and `src/protocol/`.
  - Build/test client targets only, such as `//src/client:client`, `//src/client:pixel_renderer_test`, `//src/client:m6_client_test` when present, and `//plugins/client_viewer:client_viewer`.
  - Do not build or test Linux-only Gateway/AppHost targets from Windows unless explicitly requested.
- **Linux side**: Gateway, AppHost, server-side plugins, protocol, and integration tests.
  - Primary areas: `src/gateway/`, `src/apphost/`, server plugins under `plugins/`, plus shared code in `src/common/` and `src/protocol/`.
  - Build/test Linux server targets only, such as `//src/gateway:gateway_bin`, `//src/gateway:gateway_test`, `//src/gateway:gateway_lifecycle_test`, `//src/gateway:gateway_m4_plugin_test`, `//src/gateway:gateway_m5_input_test`, `//src/apphost:apphost_bin`, and AppHost tests.
  - Do not build or test Windows-only client executable/viewer targets from Linux unless the target is explicitly known to be portable and relevant to the change.

Avoid `bazel build //...` and `bazel test //...` by default. Full-repo builds mix Windows-only and Linux-only targets and can fail for environment reasons unrelated to the change. Only run full-repo commands when the user explicitly asks for them or after confirming the current platform supports all selected targets.

```bash
# Build a specific target.
bazel build //src/protocol:message_parser

# Run a specific test.
bazel test //src/protocol:message_parser_test

# Linux Gateway/AppHost examples.
bazel test //src/gateway:gateway_m5_input_test
bazel build //src/gateway:gateway_bin //src/apphost:apphost_bin

# Windows Client examples.
bazel test //src/client:pixel_renderer_test
bazel build //src/client:client //plugins/client_viewer:client_viewer
```

## Testing Policy

Whenever a `.cc` file is modified, follow these steps before considering the task done:

1. **Check if a corresponding `_test.cc` file exists** (e.g. modifying `foo.cc` → look for `foo_test.cc`).
2. **If no `_test.cc` exists**: strongly prefer creating one and adding test cases that cover the changed behaviour. Do not skip this without a clear reason.
3. **If a `_test.cc` already exists**: review it and update or add test cases to reflect the changes made. Do not leave tests that no longer match the implementation.

This applies to all `.cc` files under `src/`. Test files follow the naming convention `<source>_test.cc` and live alongside the source file in the same directory.

## Code Style

### Comments
- All comments must be written in **English**. No Chinese in source files.
- Every public method in a `.h` file must have a comment explaining its purpose and behaviour.
- Inline comments should explain **why**, not what. Omit comments that just restate the code.

### Naming
- Member variables use a trailing underscore: `read_pos_`, `state_`, `pending_header_`
- Types and classes use PascalCase: `MessageParser`, `ParseResult`
- Functions and local variables use snake_case: `next_packet()`, `body_length`

### General
- C++17. Do not use features beyond C++17.
- Prefer returning error codes / enums over exceptions in networking and protocol code.
- Each `.h` file must use `#pragma once`.
