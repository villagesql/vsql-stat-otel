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
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VSQL_STAT_OTEL_OTEL_SINK_H
#define VSQL_STAT_OTEL_OTEL_SINK_H

#include <string>
#include <vector>

#include "core/event_row.h"
#include "core/sink.h"

namespace vsql_stat_otel {

// OTLP sink: ships each captured statement as one OpenTelemetry LogRecord, over
// OTLP/HTTP with a JSON body (POST <endpoint>/v1/logs). One flush builds one
// ExportLogsServiceRequest: a single resource (service.name, db.system) with a
// scopeLogs list of one logRecord per event. Reads its config through pointers
// to the sink's sysvar-backed globals, so a live SET GLOBAL is picked up on the
// next flush. Runs only on the core's single flush worker.
//
// The body follows protobuf's canonical JSON mapping (the OTLP/HTTP+JSON wire
// format), NOT free-form JSON: 64-bit integers are emitted as quoted strings
// and attribute values are tagged unions ({"stringValue":...} etc). A strict
// receiver rejects a body that gets this wrong.
class OtelSink : public ::vsql_stat::Sink {
public:
  struct Config {
    char **endpoint; // "http://host:4318" (empty disables sending); "/v1/logs"
                     // is appended
    char **service_name; // resource attribute service.name
    int64_t *http_timeout_secs;
  };

  explicit OtelSink(const Config &config) : config_(config) {}

  bool flush(const std::vector<::vsql_stat::EventRow> &batch,
             std::string &err) override;

private:
  Config config_;
};

} // namespace vsql_stat_otel

#endif // VSQL_STAT_OTEL_OTEL_SINK_H
