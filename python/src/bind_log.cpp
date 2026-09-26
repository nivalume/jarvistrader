// Python access to event logs: write events, read records back as model objects, fingerprint
// and compare logs, and generate the deterministic corpus.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/tuple.h>

#include "common.hpp"
#include "events.hpp"
#include "jarvis/core/event_key.hpp"
#include "jarvis/node/build_info.hpp"
#include "jarvis/node/corpus.hpp"
#include "jarvis/node/event_log.hpp"
#include "jarvis/node/event_text.hpp"
#include "jarvis/node/fingerprint.hpp"

namespace jarvis::py {

namespace {

namespace wire = jarvis::model::wire;

struct PyLogWriter {
  node::EventLogWriter writer;
};

struct PyLogReader {
  node::EventLogReader reader;
};

struct PyRecord {
  std::uint64_t seq = 0;
  std::uint64_t ts = 0;
  std::uint16_t source_id = 0;
  std::string kind;
  nb::object event;
};

template <std::size_t N> void set_text(core::FixedString<N>& out, std::string_view text) {
  static_cast<void>(core::FixedString<N>::from(text.substr(0, N), out));
}

wire::LogHeader make_header(std::uint64_t seed, const nb::bytes& config_hash,
                            std::string_view numpy_version) {
  wire::LogHeader h;
  h.seed = seed;
  if (config_hash.size() != 0 && config_hash.size() != h.config_hash.size()) {
    throw nb::value_error("config_hash must be 32 bytes (a SHA-256 digest)");
  }
  const auto* hash = static_cast<const std::uint8_t*>(config_hash.data());
  for (std::size_t i = 0; i < config_hash.size(); ++i) {
    h.config_hash[i] = hash[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  const node::BuildInfo info = node::build_info();
  set_text(h.jarvis_version, info.version);
  set_text(h.git_commit, info.git_commit);
  set_text(h.compiler, info.compiler);
  set_text(h.platform, info.platform);
  const nb::object sys = nb::module_::import_("sys");
  const nb::object v = sys.attr("version_info");
  const std::string py = std::to_string(nb::cast<int>(v.attr("major"))) + "." +
                         std::to_string(nb::cast<int>(v.attr("minor"))) + "." +
                         std::to_string(nb::cast<int>(v.attr("micro")));
  set_text(h.python_version, py);
  set_text(h.numpy_version, numpy_version);
  const nb::object environ = nb::module_::import_("os").attr("environ");
  const nb::object hash_seed = environ.attr("get")("PYTHONHASHSEED", "");
  const auto seed_text = nb::cast<std::string>(hash_seed);
  if (!seed_text.empty() && seed_text.find_first_not_of("0123456789") == std::string::npos) {
    h.python_hash_seed = std::stoull(seed_text);
  }
  return h;
}

node::RecordFilter filter_of(std::string_view records) {
  if (records == "inputs") {
    return node::RecordFilter::Inputs;
  }
  if (records == "outputs") {
    return node::RecordFilter::Outputs;
  }
  if (records == "all") {
    return node::RecordFilter::All;
  }
  throw nb::value_error("records must be 'inputs', 'outputs' or 'all'");
}

nb::dict header_dict(const wire::LogHeader& h) {
  nb::dict d;
  d["format_version"] = h.format_version;
  d["schema_version"] = h.schema_version;
  d["segment_index"] = h.segment_index;
  d["config_hash"] = nb::bytes(reinterpret_cast<const char*>(h.config_hash.data()), // NOLINT
                               h.config_hash.size());
  d["seed"] = h.seed;
  d["python_hash_seed"] = h.python_hash_seed;
  d["jarvis_version"] = std::string{h.jarvis_version.view()};
  d["git_commit"] = std::string{h.git_commit.view()};
  d["compiler"] = std::string{h.compiler.view()};
  d["platform"] = std::string{h.platform.view()};
  d["python_version"] = std::string{h.python_version.view()};
  d["numpy_version"] = std::string{h.numpy_version.view()};
  d["sbe_schema"] = std::string{h.sbe_schema.view()};
  return d;
}

} // namespace

void bind_log(nb::module_& mod) {
  nb::class_<PyLogWriter>(mod, "EventLogWriter",
                          "Writes events to a new event log directory (refuses to overwrite).")
      .def(
          "__init__",
          [](PyLogWriter* self, const std::string& directory, std::uint64_t seed,
             const nb::bytes& config_hash, std::uint64_t segment_bytes, bool sync_on_flush,
             std::string_view numpy_version) {
            new (self) PyLogWriter{};
            node::EventLogOptions options;
            options.segment_bytes = segment_bytes;
            options.sync_on_flush = sync_on_flush;
            check(self->writer.open(directory, make_header(seed, config_hash, numpy_version),
                                    options),
                  "EventLogWriter(" + directory + ")");
          },
          nb::arg("directory"), nb::kw_only(), nb::arg("seed") = 0,
          nb::arg("config_hash") = nb::bytes(""), nb::arg("segment_bytes") = 256ULL << 20U,
          nb::arg("sync_on_flush") = false, nb::arg("numpy_version") = "")
      .def(
          "append",
          [](PyLogWriter& self, nb::handle event, std::uint64_t seq, std::uint64_t ts,
             std::uint16_t source_id) {
            nb::object holder;
            const m::Event e = event_from_py(event, holder);
            check(self.writer.append(core::EventKey{core::UnixNanos{ts}, source_id, seq}, e),
                  "EventLogWriter.append");
          },
          nb::arg("event"), nb::kw_only(), nb::arg("seq"), nb::arg("ts"), nb::arg("source_id") = 0,
          "Appends `event` under the order key (ts, source_id, seq).")
      .def("flush", [](PyLogWriter& self) { check(self.writer.flush(), "EventLogWriter.flush"); })
      .def("close", [](PyLogWriter& self) { check(self.writer.close(), "EventLogWriter.close"); })
      .def_prop_ro("records", [](const PyLogWriter& self) { return self.writer.records(); })
      .def("__enter__", [](nb::handle self) { return nb::borrow(self); })
      .def(
          "__exit__",
          [](PyLogWriter& self, nb::handle, nb::handle, nb::handle) {
            check(self.writer.close(), "EventLogWriter.close");
            return false;
          },
          nb::arg("exc_type").none(), nb::arg("exc").none(), nb::arg("traceback").none());

  nb::class_<PyRecord>(mod, "Record", "One log record: its order key, kind and event.")
      .def_ro("seq", &PyRecord::seq)
      .def_ro("ts", &PyRecord::ts)
      .def_ro("source_id", &PyRecord::source_id)
      .def_ro("kind", &PyRecord::kind)
      .def_ro("event", &PyRecord::event)
      .def("__repr__", [](const PyRecord& r) {
        return "Record(seq=" + std::to_string(r.seq) + ", ts=" + std::to_string(r.ts) +
               ", source_id=" + std::to_string(r.source_id) + ", kind='" + r.kind + "')";
      });

  nb::class_<PyLogReader>(mod, "EventLogReader", "Iterates the records of an event log directory.")
      .def(
          "__init__",
          [](PyLogReader* self, const std::string& directory) {
            new (self) PyLogReader{};
            check(self->reader.open(directory), "EventLogReader(" + directory + ")");
          },
          nb::arg("directory"))
      .def_prop_ro("header",
                   [](const PyLogReader& self) { return header_dict(self.reader.header()); })
      .def("__iter__", [](nb::handle self) { return nb::borrow(self); })
      .def("__next__", [](PyLogReader& self) {
        wire::RecordView record;
        const core::Status s = self.reader.next(record);
        if (s == core::Status::EndOfStream) {
          throw nb::stop_iteration();
        }
        check(s, "EventLogReader");
        m::Event event;
        check(self.reader.decode(record, event), "EventLogReader.decode");
        return PyRecord{record.header.seq, record.header.ts.value(), record.header.source_id,
                        std::string{wire::kind_name(wire::kind_of(event))}, event_to_py(event)};
      });

  mod.def(
      "fingerprint",
      [](const std::string& directory, std::string_view records) {
        node::Fingerprint fp;
        check(node::fingerprint_log(directory, filter_of(records), fp), "fingerprint");
        return nb::make_tuple(node::hex(fp.digest), fp.records);
      },
      nb::arg("directory"), nb::arg("records") = "inputs",
      "(sha256 hex, record count) over the selected records' bytes; the header is excluded.");
  mod.def(
      "compare",
      [](const std::string& a, const std::string& b, std::string_view records) {
        node::LogComparison c;
        check(node::compare_logs(a, b, filter_of(records), c), "compare");
        return nb::make_tuple(c.equal, c.compared, c.first_diff, c.detail);
      },
      nb::arg("a"), nb::arg("b"), nb::arg("records") = "inputs",
      "(equal, records compared, index of the first difference, description).");
  mod.def(
      "write_corpus",
      [](const std::string& directory, std::uint64_t seed, std::uint64_t events) {
        node::EventLogWriter writer;
        check(writer.open(directory, make_header(seed, nb::bytes(""), ""), {}), "write_corpus");
        node::CorpusGenerator corpus{seed};
        core::EventKey key;
        m::Event event;
        for (std::uint64_t i = 0; i < events; ++i) {
          check(corpus.next(key, event), "write_corpus");
          check(writer.append(key, event), "write_corpus");
        }
        check(writer.close(), "write_corpus");
      },
      nb::arg("directory"), nb::arg("seed"), nb::arg("events"),
      "Writes the deterministic corpus (every event kind) used by the determinism gate.");
  mod.def(
      "event_text",
      [](nb::handle event) {
        nb::object holder;
        std::string out;
        node::append_event_text(out, event_from_py(event, holder));
        return out;
      },
      nb::arg("event"), "The event's line as `jarvis dump` prints it.");
}

} // namespace jarvis::py
