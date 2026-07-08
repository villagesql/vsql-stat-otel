# Testing vsql-stat-otel

Two tiers, mirroring the rest of the `vsql-stat` family:

- **`otel.test`** — hermetic, runs everywhere (including CI) with no external
  service. Verifies the OTLP body shape.
- **`otel_live.test`** — skip-gated, runs only against a real OpenTelemetry
  Collector. Verifies genuine interop.

## Running the tests

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./test.sh            # build the VEB, stage it, run the MTR suite
RECORD=1 ./test.sh   # same, but record the expected .result files
```

`test.sh` builds via `build.sh`, stages the VEB into a temp veb-dir, and runs
`mysql-test-run.pl` from the dev server's `mysql-test` directory. MTR manages
its own server; do not start one by hand.

## Tier 1 — `otel.test` (hermetic)

The mapping and the OTLP/HTTP+JSON encoding are the bug-prone part, so the
default test **verifies the body**, not just that a POST happened. It:

1. Writes and backgrounds a ~20-line Python `http.server` that accepts the POST
   to any path, writes the request body to a temp file, and returns `200 {}`.
   Python 3 is present on every CI runner and dev box, so this stays hermetic.
2. Installs the extension, points `otlp_endpoint` at the in-test receiver, and
   enables capture with a short flush interval and `batch_max = 1`.
3. Runs a query and waits on `events_captured` then `events_archived` (the
   worker POSTed and got a 200).
4. Reads the captured body and asserts:
   - the OTLP envelope is present (`resourceLogs`, `scopeLogs`, `logRecords`);
   - the semantic-convention attributes are present (`db.query.text`,
     `db.system`) and the query marker round-tripped;
   - **the footgun guard**: 64-bit int attribute values are **quoted strings**
     (`"intValue":"..."`), per protobuf's canonical JSON mapping.
5. Uninstalls cleanly and stops the receiver.

This catches encoding and semantic-convention regressions with no docker.

## Tier 2 — `otel_live.test` (real collector, skip-gated)

Proves genuine interop: a real collector both **validates** the OTLP (rejecting
a malformed body with a 400, which would surface as a flush error) and writes
what it received to a file we read back.

Start the reference collector (config checked in at
`test/otel-collector-config.yaml`):

```bash
docker run -d --name vsql-otel -p 4318:4318 \
  -v "$PWD/test/otel-collector-config.yaml:/etc/otelcol/config.yaml" \
  -v "$PWD/otel-out:/output" \
  otel/opentelemetry-collector
```

`test.sh` auto-detects a collector on `http://127.0.0.1:4318` (an empty
`{}` POST to `/v1/logs` returns 200 when it is up) and, if found, exports
`VSQL_STAT_OTEL_ENDPOINT` and `VSQL_STAT_OTEL_OUTFILE`
(`./otel-out/received.json`). The live test then inserts for real, asserts no
flush errors, and greps the collector's file-exporter output for the query
marker. A pass means the **reference implementation accepted the body as
conformant OTLP** and the fields round-tripped — a stronger guarantee than a
sink that just gets a 200 from an arbitrary endpoint.

When no collector is reachable, `test.sh` leaves those vars unset and
`otel_live.test` skips, so the default suite stays hermetic and cannot hang.

## Notes

- Both tests are gated on `not_windows.inc` (they shell out to Python / grep and
  use POSIX paths).
- Result files use `--replace_result` where a machine-specific value (the
  receiver port) would otherwise leak into `.result`.
