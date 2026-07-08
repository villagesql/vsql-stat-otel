# AGENTS.md

This file provides guidance to AI coding assistants (Claude Code, Gemini Code Assist, etc.) when working with code in this repository.

**Note**: Also check `AGENTS.local.md` for additional local development instructions when present.

## Project Overview

This is a per-query telemetry extension for VillageSQL (a MySQL-compatible database). It captures one event per finished statement (query text, user, timing, rows, error state, ...) and ships it as one **OpenTelemetry log record** over **OTLP/HTTP** (a JSON body POSTed to `:4318/v1/logs`). Part of the `vsql-stat` family; the shared capture pipeline lives in [`vsql-stat-core`](https://github.com/villagesql/vsql-stat-core), a git submodule at `core/`.

OTLP is a standardized protocol (OpenTelemetry / CNCF), so one OTLP sink reaches every downstream backend via an OpenTelemetry Collector (ClickHouse, Elasticsearch, Loki, Datadog, ...). This is why the family has one OTLP sink rather than one sink per backend.

Unlike function extensions (VDFs), this uses VillageSQL **preview capabilities**: a POSTEXECUTE `statement_event` hook (capture on the connection thread) plus a `thread_worker` (background flush), with `sys_var`/`status_var` for config and observability.

## Build System

**IMPORTANT**: Always build in the `build/` directory, never in the source root.

### Build

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./build.sh
```

`build.sh` selects the newest staged SDK under `VillageSQL_BUILD_DIR` and builds
`build/vsql_stat_otel.veb`.

### Test

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./test.sh            # build + run the MTR suite
RECORD=1 ./test.sh   # build + record the .result files
```

`test.sh` builds, stages the VEB, and runs MTR. The `otel` test is hermetic (an
in-test Python receiver captures the POST body and asserts the OTLP-JSON shape);
`otel_live` is skip-gated and runs only when a real OTel Collector is reachable
(`test.sh` auto-detects one on `http://127.0.0.1:4318`). See `TESTING.md`.

### Cloning

This repo has **one git submodule**: the shared pipeline `vsql-stat-core` at
`core/`.

```bash
git clone --recurse-submodules <this-repo>
# or, after a plain clone:
git submodule update --init
```

### Dependencies

- VillageSQL Extension Framework SDK — **preview** headers (`include-dev/`), for
  the statement_event / thread_worker / sys_var / status_var capabilities. The
  bundled `cmake/FindVillageSQL.cmake` selects `include-dev/` when
  `VillageSQL_USE_DEV_HEADERS` is ON (set in `CMakeLists.txt`).
- libcurl (`libcurl4-openssl-dev`, or the Xcode CLT on macOS) — the OTLP/HTTP
  POST.

## Architecture

**Shared core (`core/`, from vsql-stat-core):**
- `event_row.h` — the captured statement row.
- `event_queue.{h,cc}` — bounded MPSC queue (drop-newest overflow).
- `sink.h` — the `Sink` interface this extension implements.
- `capture_pipeline.{h,cc}` — the POSTEXECUTE hook (capture→enqueue) and the
  background worker (drain→`sink.flush`). All capture/buffering logic lives here.

**This sink (`src/`):**
- `extension.cc` — the SDK capability objects (statement_event hook,
  thread_worker, sysvars, statusvars), the `OtelSink` instance, static-init
  binding into the core pipeline, and `VEF_GENERATE_ENTRY_POINTS`.
- `otel_sink.{h,cc}` — the `Sink`: builds one OTLP `ExportLogsServiceRequest`
  (a single resource → scopeLogs → one logRecord per event) and POSTs the JSON
  body to `<otlp_endpoint>/v1/logs` via libcurl.

**Config (sysvars, `vsql_stat_otel.*`):** `otlp_endpoint` (collector base URL),
`service_name` (resource attribute), `http_timeout_secs`; plus shared behavior
sysvars (`enabled`, `queue_capacity`, `batch_max`, `flush_interval_ms`,
`statement_max_bytes`). Integer sysvars use `int64_t` backing globals to match
the SDK's fixed-width ABI. Status vars: `events_captured`, `events_archived`
(= exports the collector accepted), `events_dropped`, `queue_depth`,
`flush_errors`, `last_flush_utime`.

**All network I/O runs on the worker, off the query path** — the hook only
captures + enqueues, so telemetry never adds query latency.

### The OTLP/HTTP+JSON footgun

The body is **protobuf's canonical JSON mapping**, not free-form JSON. The rules
`otel_sink.cc` must follow (and the hermetic test guards):

- **64-bit integers are quoted strings** — `timeUnixNano` and every `intValue`
  are emitted as `"..."`, not bare numbers. A strict receiver rejects otherwise.
- Attribute values are **tagged unions**: `{"stringValue":...}`,
  `{"intValue":"..."}`, `{"doubleValue":...}`, `{"boolValue":...}`.
- `query_start_utime` is **microseconds** (per the statement_event ABI); OTLP
  wants nanoseconds, so `otel_sink.cc` multiplies by 1000.

## Updating the shared core

`core/` is a git submodule pointing at `vsql-stat-core`. To move to a later core
commit:

```bash
git -C core fetch && git -C core checkout <ref>
git add core && git commit -m "Bump core"
```

Do NOT edit `core/` files directly here — change them in `vsql-stat-core`, then
bump the submodule. Core-internal includes are sibling-relative
(`#include "event_queue.h"`); this sink includes them as `#include "core/..."`.

## Licensing and Copyright

All source files (`.cc`, `.h`) and `CMakeLists.txt` must carry this header:

```
// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
```

## Common Tasks for AI Agents

- **Adding a config knob**: add an `int64_t`/`char*` global + a `sv::make_int`/
  `make_str` entry in `src/extension.cc`, add it to `OtelSink::Config`, thread it
  through the constructor, and consume it in `otel_sink.cc`. Integer sysvars must
  use `int64_t` (the SDK's `make_int` takes `int64_t*`).
- **Changing the LogRecord mapping**: `build_body()` in `otel_sink.cc`. Keep the
  int64-as-quoted-string and tagged-union rules — the hermetic test asserts them.
- **Changing capture fields**: `EventRow` and the capture logic live in `core/`
  — change them in `vsql-stat-core` and bump the submodule, not here.
- **Testing**: add/update `mysql-test/t/*.test`, regenerate with
  `RECORD=1 ./test.sh`. Keep the default (`otel`) test hermetic (in-test Python
  receiver); put real-collector assertions in the skip-gated live test.

Always maintain existing code style and include the copyright header.
