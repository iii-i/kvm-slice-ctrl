// Import host and guest trace.dat into a DuckDB database.

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>

extern "C" {
#include <event-parse.h>
#include <trace-cmd.h>
}

#include <duckdb.hpp>
#include <nlohmann/json.hpp>

namespace {

[[noreturn]] void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
  std::exit(1);
}

class TraceInput {
public:
  explicit TraceInput(const char *path) : handle_(tracecmd_open(path, 0)) {
    if (!handle_)
      die("failed to open trace: %s", path);
  }
  ~TraceInput() { tracecmd_close(handle_); }
  TraceInput(const TraceInput &) = delete;
  TraceInput &operator=(const TraceInput &) = delete;
  operator tracecmd_input *() const { return handle_; }

private:
  tracecmd_input *handle_;
};

constexpr const char *HOST_DAT = "workdir/trace-host.dat";
constexpr const char *GUEST_DAT = "workdir/trace-guest.dat";
constexpr const char *DB_PATH = "workdir/traces.duckdb";
constexpr size_t PROGRESS_EVERY = 500'000;

struct Context {
  duckdb::Appender *appender;
  tep_handle *host_tep;
  tep_handle *guest_tep;
  tracecmd_input *host_handle;
  std::unordered_map<int, int> guest_to_host_cpu;
  size_t n = 0;
  std::chrono::steady_clock::time_point t0;
};

/* See get_field_str() from libtraceevent/src/parse-filter.c */
std::string extract_fields_json(tep_event *event, tep_record *record) {
  nlohmann::json j = nlohmann::json::object();
  const char *data = static_cast<const char *>(record->data);
  for (tep_format_field *f = event->format.fields; f; f = f->next) {
    if (f->flags & TEP_FIELD_IS_STRING) {
      const char *str;
      size_t len;
      if (f->flags & TEP_FIELD_IS_DYNAMIC) {
        unsigned long long val;
        if (tep_read_number_field(f, record->data, &val) != 0)
          continue;
        size_t addr = val & 0xffff;
        if (f->flags & TEP_FIELD_IS_RELATIVE)
          addr += f->offset + f->size;
        str = data + addr;
        len = (val >> 16) & 0xffff;
      } else {
        str = data + f->offset;
        len = f->size;
      }
      // Trim at first NUL (ftrace fixed strings are NUL-padded).
      j[f->name] = std::string(str, strnlen(str, len));
    } else {
      unsigned long long val;
      if (tep_read_number_field(f, record->data, &val) == 0) {
        if (f->flags & TEP_FIELD_IS_SIGNED)
          j[f->name] = static_cast<int64_t>(val);
        else
          j[f->name] = val;
      }
    }
  }
  return j.dump();
}

int vcpu_from_comm(const char *comm) {
  int v;
  char tail;
  if (std::sscanf(comm, "CPU %d/KVM%c", &v, &tail) != 1)
    die("malformed comm: %s", comm);
  return v;
}

void append_opt(duckdb::Appender &app, std::optional<int> v) {
  if (v)
    app.Append<int32_t>(*v);
  else
    app.Append(nullptr);
}

int callback(tracecmd_input *handle, tep_record *record, int cpu,
             void *cb_data) {
  auto &ctx = *static_cast<Context *>(cb_data);
  bool is_host = (handle == ctx.host_handle);
  tep_handle *tep = is_host ? ctx.host_tep : ctx.guest_tep;

  int type = tep_data_type(tep, record);
  tep_event *event = tep_find_event(tep, type);
  if (!event)
    die("unknown event type: %d", type);

  int pid = tep_data_pid(tep, record);
  const char *comm = tep_data_comm_from_pid(tep, pid);

  std::optional<int> host_cpu;
  std::optional<int> guest_cpu;
  if (is_host) {
    host_cpu = cpu;
    bool is_entry = std::strcmp(event->name, "kvm_entry") == 0;
    bool is_exit = std::strcmp(event->name, "kvm_exit") == 0;
    if (is_entry || is_exit) {
      guest_cpu = vcpu_from_comm(comm);
      if (is_entry) {
        ctx.guest_to_host_cpu[*guest_cpu] = cpu;
      } else {
        ctx.guest_to_host_cpu.erase(*guest_cpu);
      }
    }
  } else {
    guest_cpu = cpu;
    auto it = ctx.guest_to_host_cpu.find(cpu);
    if (it != ctx.guest_to_host_cpu.end())
      host_cpu = it->second;
  }

  std::string fields = extract_fields_json(event, record);

  auto &app = *ctx.appender;
  app.BeginRow();
  app.Append<int64_t>(static_cast<int64_t>(record->ts));
  append_opt(app, host_cpu);
  append_opt(app, guest_cpu);
  app.Append<int32_t>(pid);
  app.Append(static_cast<const char *>(comm ? comm : ""));
  app.Append(static_cast<const char *>(event->name));
  app.Append(static_cast<const char *>(fields.c_str()));
  app.EndRow();

  if (++ctx.n % PROGRESS_EVERY == 0) {
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - ctx.t0).count();
    std::fprintf(stderr, "  %12zu rows  (%9.0f/s)\n", ctx.n, ctx.n / secs);
  }
  return 0;
}

} // namespace

int main() {
  std::remove(DB_PATH);

  TraceInput host_in(HOST_DAT);
  TraceInput guest_in(GUEST_DAT);

  duckdb::DuckDB db(DB_PATH);
  duckdb::Connection con(db);
  auto ddl = con.Query(R"(
        CREATE TABLE events (
            ts_ns     BIGINT  NOT NULL,
            host_cpu  INTEGER,
            guest_cpu INTEGER,
            pid       INTEGER NOT NULL,
            comm      VARCHAR NOT NULL,
            event     VARCHAR NOT NULL,
            fields    JSON
        )
    )");
  if (ddl->HasError())
    die("DDL failed: %s", ddl->GetError().c_str());

  duckdb::Appender appender(con, "events");

  Context ctx;
  ctx.appender = &appender;
  ctx.host_tep = tracecmd_get_tep(host_in);
  ctx.guest_tep = tracecmd_get_tep(guest_in);
  ctx.host_handle = host_in;
  ctx.t0 = std::chrono::steady_clock::now();

  std::fprintf(stderr, "importing\n");
  tracecmd_input *handles[] = {host_in, guest_in};
  int rc = tracecmd_iterate_events_multi(handles, 2, callback, &ctx);
  if (rc != 0)
    die("iterate stopped with rc=%d", rc);

  appender.Close();
  std::fprintf(stderr, "done: %zu rows, wrote %s\n", ctx.n, DB_PATH);
  return 0;
}
