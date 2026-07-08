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

#include "otel_sink.h"

#include <curl/curl.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace vsql_stat_otel {

namespace {

using ::vsql_stat::EventRow;

// OTLP LogRecord severityNumber values (subset). status != 0 -> ERROR.
constexpr int kSeverityInfo = 9;   // SEVERITY_NUMBER_INFO
constexpr int kSeverityError = 17; // SEVERITY_NUMBER_ERROR

size_t discard_cb(char *, size_t size, size_t nmemb, void *) {
  return size * nmemb; // we don't need the response body
}

// Minimal JSON string escaping for the values we emit (control chars, quotes,
// backslashes). OTLP/HTTP+JSON expects standard JSON string escaping.
void append_json_string(std::string &out, const std::string &s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += c;
      }
    }
  }
  out += '"';
}

// One {"key":k,"value":{"<type>Value":v}} KeyValue. In OTLP/HTTP+JSON the value
// is a tagged union; 64-bit ints MUST be quoted strings (protobuf JSON rule).
void append_attr_str(std::string &out, const char *key, const std::string &val,
                     bool first) {
  if (!first)
    out += ',';
  out += "{\"key\":";
  append_json_string(out, key);
  out += ",\"value\":{\"stringValue\":";
  append_json_string(out, val);
  out += "}}";
}

void append_attr_int(std::string &out, const char *key, uint64_t val) {
  out += ",{\"key\":";
  append_json_string(out, key);
  // intValue is an int64 -> serialized as a quoted string.
  out += ",\"value\":{\"intValue\":\"";
  out += std::to_string(val);
  out += "\"}}";
}

void append_attr_dbl(std::string &out, const char *key, double val) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.6f", val);
  out += ",{\"key\":";
  append_json_string(out, key);
  out += ",\"value\":{\"doubleValue\":";
  out += buf;
  out += "}}";
}

void append_attr_bool(std::string &out, const char *key, bool val) {
  out += ",{\"key\":";
  append_json_string(out, key);
  out += ",\"value\":{\"boolValue\":";
  out += (val ? "true" : "false");
  out += "}}";
}

// Build one ExportLogsServiceRequest: a single resource (service.name +
// db.system) whose scopeLogs holds one logRecord per event.
std::string build_body(const std::vector<EventRow> &batch,
                       const std::string &service_name) {
  std::string body;
  body.reserve(batch.size() * 512);

  body += "{\"resourceLogs\":[{\"resource\":{\"attributes\":[";
  append_attr_str(body, "service.name", service_name, /*first=*/true);
  append_attr_str(body, "db.system", "mysql", /*first=*/false);
  body += "]},\"scopeLogs\":[{\"scope\":{\"name\":\"vsql-stat-otel\","
          "\"version\":\"0.0.1\"},\"logRecords\":[";

  bool first_rec = true;
  for (const EventRow &r : batch) {
    if (!first_rec)
      body += ',';
    first_rec = false;

    // timeUnixNano: query_start_utime is microseconds since epoch (ABI:
    // statement_event.query_start_utime); OTLP wants nanoseconds, and the value
    // is an int64 -> quoted string.
    const uint64_t time_ns = r.query_start_utime * 1000ULL;
    const int severity = (r.status != 0) ? kSeverityError : kSeverityInfo;

    body += "{\"timeUnixNano\":\"";
    body += std::to_string(time_ns);
    body += "\",\"severityNumber\":";
    body += std::to_string(severity);
    body += ",\"severityText\":";
    append_json_string(body, severity == kSeverityError ? "ERROR" : "INFO");
    body += ",\"body\":{\"stringValue\":";
    append_json_string(body, r.query);
    body += "},\"attributes\":[";

    append_attr_str(body, "db.query.text", r.query, /*first=*/true);
    append_attr_str(body, "db.operation.name", r.sql_command, false);
    append_attr_str(body, "db.namespace", r.schema, false);
    append_attr_str(body, "db.user", r.user, false);
    append_attr_str(body, "client.address", r.client_ip, false);
    append_attr_int(body, "vsql.connection_id", r.connection_id);
    append_attr_bool(body, "vsql.in_transaction", r.in_transaction);
    append_attr_dbl(body, "db.query.duration", r.query_time_secs);
    append_attr_dbl(body, "vsql.lock_time_secs", r.lock_time_secs);
    append_attr_int(body, "db.response.returned_rows", r.rows_sent);
    append_attr_int(body, "vsql.rows_examined", r.rows_examined);
    append_attr_int(body, "vsql.rows_affected", r.rows_affected);
    append_attr_int(body, "vsql.warning_count", r.warning_count);
    append_attr_int(body, "vsql.status", static_cast<uint64_t>(r.status));
    append_attr_int(body, "network.peer.port", r.port);
    if (!r.sqlstate.empty())
      append_attr_str(body, "db.response.status_code", r.sqlstate, false);
    if (!r.error_message.empty())
      append_attr_str(body, "error.message", r.error_message, false);
    if (!r.digest_text.empty())
      append_attr_str(body, "vsql.digest_text", r.digest_text, false);
    append_attr_int(body, "vsql.bytes_sent", r.bytes_sent);
    append_attr_int(body, "vsql.bytes_received", r.bytes_received);
    append_attr_int(body, "vsql.select_full_join", r.select_full_join);
    append_attr_int(body, "vsql.select_full_range_join",
                    r.select_full_range_join);
    append_attr_int(body, "vsql.select_range", r.select_range);
    append_attr_int(body, "vsql.select_range_check", r.select_range_check);
    append_attr_int(body, "vsql.select_scan", r.select_scan);
    append_attr_int(body, "vsql.sort_merge_passes", r.sort_merge_passes);
    append_attr_int(body, "vsql.sort_range", r.sort_range);
    append_attr_int(body, "vsql.sort_rows", r.sort_rows);
    append_attr_int(body, "vsql.sort_scan", r.sort_scan);
    append_attr_int(body, "vsql.created_tmp_tables", r.created_tmp_tables);
    append_attr_int(body, "vsql.created_tmp_disk_tables",
                    r.created_tmp_disk_tables);
    append_attr_bool(body, "vsql.no_index_used", r.no_index_used);
    append_attr_bool(body, "vsql.no_good_index_used", r.no_good_index_used);

    body += "]}";
  }

  body += "]}]}]}";
  return body;
}

std::string cfg(char **p) { return (p && *p) ? std::string(*p) : ""; }

} // namespace

bool OtelSink::flush(const std::vector<EventRow> &batch, std::string &err) {
  if (batch.empty())
    return true;

  const std::string endpoint = cfg(config_.endpoint);
  if (endpoint.empty()) {
    err = "otlp_endpoint not configured";
    return false;
  }
  std::string service_name = cfg(config_.service_name);
  if (service_name.empty())
    service_name = "villagesql";
  const long http_timeout_secs = static_cast<long>(
      (config_.http_timeout_secs && *config_.http_timeout_secs > 0)
          ? *config_.http_timeout_secs
          : 10);

  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    err = "curl_easy_init failed";
    return false;
  }

  const std::string url = endpoint + "/v1/logs";
  const std::string body = build_body(batch, service_name);

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, http_timeout_secs);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    err = std::string("OTLP export failed: ") + curl_easy_strerror(rc);
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    err = "collector returned HTTP " + std::to_string(http_code);
    return false;
  }
  return true;
}

} // namespace vsql_stat_otel
