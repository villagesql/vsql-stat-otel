# vsql-stat-otel — OpenTelemetry (OTLP) telemetry sink

A VillageSQL extension that captures one event per finished statement and ships
it as an **OpenTelemetry log record** over **OTLP/HTTP** (JSON body, POST to
`:4318/v1/logs`). Part of the `vsql-stat` family; shares the capture pipeline in
[`vsql-stat-core`](https://github.com/villagesql/vsql-stat-core) (a git submodule
under `core/`).

The value is leverage, not a new backend: OTLP is a standardized protocol
(OpenTelemetry / CNCF), so one OTLP sink reaches every downstream — point it at
an [OpenTelemetry Collector](https://opentelemetry.io/docs/collector/) and the
collector fans out to ClickHouse, Elasticsearch, Loki, Datadog, Splunk, etc.

## Status

Preview. Requires a server started with `--vsql_allow_preview_extensions=ON`.

## Build

```bash
git clone --recurse-submodules <this-repo>   # pulls the core submodule
export VillageSQL_BUILD_DIR=$HOME/build/villagesql
./build.sh               # build the VEB
./test.sh                # build + run the MTR suite
```

If cloned without `--recurse-submodules`:

```bash
git submodule update --init   # core (shared pipeline)
```

Prerequisites: a VillageSQL build dir (`VillageSQL_BUILD_DIR`), CMake 3.16+, a
C++17 compiler, and libcurl development headers (`libcurl4-openssl-dev` on
Debian/Ubuntu; included with the Xcode Command Line Tools on macOS). The build
produces `build/vsql_stat_otel.veb`.

## Usage

```sql
INSTALL EXTENSION vsql_stat_otel;

SET GLOBAL vsql_stat_otel.otlp_endpoint = 'http://collector:4318';  -- default localhost:4318
SET GLOBAL vsql_stat_otel.service_name  = 'villagesql';
SET GLOBAL vsql_stat_otel.enabled       = ON;   -- start capturing
```

`/v1/logs` is appended to `otlp_endpoint`. An empty endpoint disables sending.

Sysvars:

| Variable | Purpose |
|----------|---------|
| `otlp_endpoint` | Collector base URL, e.g. `http://host:4318` (empty disables sending) |
| `service_name` | OTLP resource attribute `service.name` |
| `http_timeout_secs` | Timeout for one OTLP export |

Shared behavior sysvars (`enabled`, `queue_capacity`, `batch_max`,
`flush_interval_ms`, `statement_max_bytes`) and status vars (`events_captured`,
`events_archived`, `events_dropped`, `queue_depth`, `flush_errors`,
`last_flush_utime`) come from the shared core.

## What is emitted

Each finished statement becomes one OTLP `LogRecord`. The mapping uses
OpenTelemetry semantic conventions where one exists, and `vsql.*` attributes
otherwise:

| Statement field | OTLP location |
|---|---|
| start time (µs → ns) | `timeUnixNano` |
| query text | `body` + attribute `db.query.text` |
| — | attribute `db.system` = `mysql` (resource) |
| sql command | attribute `db.operation.name` |
| schema | attribute `db.namespace` |
| user | attribute `db.user` |
| client ip | attribute `client.address` |
| rows sent | attribute `db.response.returned_rows` |
| query time (s) | attribute `db.query.duration` |
| status | `severityNumber` (non-zero → ERROR) + attribute `vsql.status` |
| client port | attribute `network.peer.port` |
| sqlstate / error message | attributes `db.response.status_code` / `error.message` (on error) |
| digest (normalized query) | attribute `vsql.digest_text` |
| connection id / in-transaction / rows examined / rows affected / warnings / lock time / bytes sent+received | `vsql.*` attributes |
| optimizer indicators (`select_*`), sort metrics (`sort_*`), temp-table usage (`created_tmp_*`), index-usage flags | `vsql.*` attributes |

The body is OTLP/HTTP+JSON (protobuf's canonical JSON mapping): 64-bit integers
are emitted as quoted strings and attribute values are tagged unions.

## Collector side (operator-owned)

Run any OpenTelemetry Collector with an OTLP/HTTP receiver on `:4318` and export
to wherever you like. A minimal collector for local testing (writes received
logs to a file):

```bash
docker run -d --name vsql-otel -p 4318:4318 \
  -v "$PWD/test/otel-collector-config.yaml:/etc/otelcol/config.yaml" \
  -v "$PWD/otel-out:/output" \
  otel/opentelemetry-collector
```

`test/otel-collector-config.yaml` is checked into this repo.

## Testing

`./test.sh` builds and runs the MTR suite (2 tests: one hermetic, one
skip-gated live). The hermetic `otel` test needs no collector — an in-test
Python receiver captures the POST body and the test asserts the OTLP-JSON
structure (including that int64 fields are quoted). The `otel_live` test is
skip-gated: it runs only when a real OTel Collector is reachable (`test.sh`
auto-detects one on `http://127.0.0.1:4318`), exports for real, and reads the
record back from the collector's file exporter. See `TESTING.md`.

## Updating the shared core

`core/` is a git submodule pointing at `vsql-stat-core`. To move to a later
core commit:

```bash
git -C core fetch && git -C core checkout <ref>
git add core && git commit -m "Bump core"
```

## License

GPL v2 — see LICENSE.
