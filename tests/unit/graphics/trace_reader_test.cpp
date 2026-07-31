#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/trace_dump.h>
#include <rex/graphics/trace_player.h>
#include <rex/graphics/trace_reader.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>

namespace {

using rex::graphics::EdramSnapshotCommand;
using rex::graphics::EventCommand;
using rex::graphics::MemoryCommand;
using rex::graphics::MemoryEncodingFormat;
using rex::graphics::PacketStartCommand;
using rex::graphics::PrimaryBufferEndCommand;
using rex::graphics::TraceCommandType;
using rex::graphics::TraceDump;
using rex::graphics::TraceHeader;
using rex::graphics::TraceReader;
using rex::graphics::TraceReplayPreflight;
using rex::graphics::TraceEdramRequirements;
using rex::graphics::TraceWriter;

template <typename T>
void AppendPod(std::vector<uint8_t>& bytes, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  size_t offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::vector<uint8_t> MakeTraceHeader(TraceEdramRequirements requirements = {},
                                     bool requirements_tracked = true,
                                     bool finalized = true) {
  TraceHeader header = {};
  header.version = rex::graphics::kTraceFormatVersion;
  std::memcpy(header.build_commit_sha, "trace-reader-test", 17);
  header.edram_manifest = rex::graphics::MakeTraceEdramRequirementsManifest(
      requirements, requirements_tracked, finalized);
  std::vector<uint8_t> bytes;
  AppendPod(bytes, header);
  return bytes;
}

void AppendCompletedFrame(std::vector<uint8_t>& bytes) {
  AppendPod(bytes, PrimaryBufferEndCommand{TraceCommandType::kPrimaryBufferEnd});
  AppendPod(bytes, EventCommand{TraceCommandType::kEvent, EventCommand::Type::kSwap});
}

void AppendEdramSnapshot(std::vector<uint8_t>& bytes) {
  AppendPod(bytes,
            EdramSnapshotCommand{TraceCommandType::kEdramSnapshot, MemoryEncodingFormat::kNone,
                                 rex::graphics::xenos::kEdramSizeBytes});
  bytes.resize(bytes.size() + rex::graphics::xenos::kEdramSizeBytes);
}

class TemporaryTrace {
 public:
  TemporaryTrace() {
    static std::atomic<uint64_t> next_id{0};
    path_ = std::filesystem::temp_directory_path() /
            ("rex-trace-reader-" + std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)) +
             ".xtr");
  }

  ~TemporaryTrace() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  bool Write(const std::vector<uint8_t>& bytes) const {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return stream.good();
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ContractTraceDump final : public TraceDump {
 public:
  explicit ContractTraceDump(bool supports_contract)
      : supports_contract_(supports_contract) {}

 protected:
  std::unique_ptr<rex::graphics::GraphicsSystem> CreateGraphicsSystem() override { return nullptr; }
  void BeginHostCapture() override {}
  void EndHostCapture() override {}
  bool SupportsCanonicalEdramRequirements(
      const TraceEdramRequirements& requirements,
      std::string& limitation_out) const override {
    (void)requirements;
    limitation_out =
        supports_contract_ ? std::string() : "unit-test unsupported EDRAM contract";
    return supports_contract_;
  }

 private:
  bool supports_contract_;
};

class PlaybackTestGraphicsSystem final : public rex::graphics::GraphicsSystem {
 public:
  explicit PlaybackTestGraphicsSystem(rex::memory::Memory& memory) {
    memory_ = &memory;
  }

  std::string name() const override { return "Metal"; }

  void InstallCommandProcessor(
      std::unique_ptr<rex::graphics::CommandProcessor> command_processor) {
    command_processor_ = std::move(command_processor);
  }

 protected:
  void CreateProvider(bool with_presentation) override {
    (void)with_presentation;
  }
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return nullptr;
  }
};

class PlaybackCommandProcessor final
    : public rex::graphics::CommandProcessor {
 public:
  explicit PlaybackCommandProcessor(PlaybackTestGraphicsSystem& graphics_system,
                                    bool restore_succeeds)
      : CommandProcessor(&graphics_system, nullptr),
        restore_succeeds_(restore_succeeds) {}

  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}
  void TracePlaybackWroteMemory(uint32_t, uint32_t) override {}
  bool RestoreEdramSnapshot(const void*) override {
    ++restore_attempt_count_;
    return restore_succeeds_;
  }

  uint32_t restore_attempt_count() const { return restore_attempt_count_; }

  void AcceptCallsForTest() {
    std::lock_guard<std::mutex> lock(pending_fns_mutex_);
    worker_running_.store(true, std::memory_order_release);
    worker_accepting_functions_ = true;
  }

  bool RunOnePendingCallForTest() {
    std::function<void()> fn;
    if (!TryPopPendingFunction(&fn)) {
      return false;
    }
    fn();
    return true;
  }

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType, uint32_t,
                                    const uint32_t*, uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t,
                 IndexBufferInfo*, bool) override {
    return true;
  }
  bool IssueCopy() override { return true; }

 private:
  bool restore_succeeds_;
  std::atomic<uint32_t> restore_attempt_count_ = 0;
};

}  // namespace

TEST_CASE("TraceReader ends stream frames at swap events", "[graphics][trace]") {
  TemporaryTrace trace;
  std::vector<uint8_t> bytes = MakeTraceHeader();
  AppendCompletedFrame(bytes);
  AppendCompletedFrame(bytes);
  REQUIRE(trace.Write(bytes));

  TraceReader reader;
  REQUIRE(reader.Open(trace.path().string()));
  REQUIRE(reader.frame_count() == 2);
  CHECK(reader.frame(0)->end_ptr == reader.frame(1)->start_ptr);
  CHECK(size_t(reader.frame(0)->end_ptr - reader.frame(0)->start_ptr) ==
        sizeof(PrimaryBufferEndCommand) + sizeof(EventCommand));
  CHECK(size_t(reader.frame(1)->end_ptr - reader.frame(1)->start_ptr) ==
        sizeof(PrimaryBufferEndCommand) + sizeof(EventCommand));
}

TEST_CASE("TraceReader does not expose incomplete EOF tails", "[graphics][trace]") {
  SECTION("commands without a swap") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendPod(bytes, PrimaryBufferEndCommand{TraceCommandType::kPrimaryBufferEnd});
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK(reader.frame_count() == 0);
  }

  SECTION("unterminated packet") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendPod(bytes, PacketStartCommand{TraceCommandType::kPacketStart, 0x1000, 1});
    AppendPod(bytes, uint32_t(0));
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK(reader.frame_count() == 0);
  }
}

TEST_CASE("TraceReader rejects malformed command spans", "[graphics][trace]") {
  SECTION("truncated packet payload") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendPod(bytes, PacketStartCommand{TraceCommandType::kPacketStart, 0x1000, 2});
    AppendPod(bytes, uint32_t(0));
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    CHECK_FALSE(reader.Open(trace.path().string()));
    CHECK(reader.frame_count() == 0);
  }

  SECTION("uncompressed payload length mismatch") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendPod(bytes, MemoryCommand{TraceCommandType::kMemoryRead, 0x1000,
                                   MemoryEncodingFormat::kNone, 1, 2});
    bytes.push_back(0);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    CHECK_FALSE(reader.Open(trace.path().string()));
    CHECK(reader.frame_count() == 0);
  }

  SECTION("malformed Snappy payload") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendPod(bytes, MemoryCommand{TraceCommandType::kMemoryRead, 0x1000,
                                   MemoryEncodingFormat::kSnappy, 1, 4});
    // Valid uncompressed-length prefix, but no compressed body.
    bytes.push_back(4);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    CHECK_FALSE(reader.Open(trace.path().string()));
    CHECK(reader.frame_count() == 0);
  }

  SECTION("unknown command") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendPod(bytes, uint32_t(0xFFFFFFFFu));
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    CHECK_FALSE(reader.Open(trace.path().string()));
    CHECK(reader.frame_count() == 0);
  }
}

TEST_CASE("TraceReader reopening replaces frame metadata", "[graphics][trace]") {
  TemporaryTrace trace;
  std::vector<uint8_t> bytes = MakeTraceHeader();
  AppendCompletedFrame(bytes);
  AppendCompletedFrame(bytes);
  REQUIRE(trace.Write(bytes));

  TraceReader reader;
  REQUIRE(reader.Open(trace.path().string()));
  REQUIRE(reader.frame_count() == 2);
  reader.Close();

  bytes = MakeTraceHeader();
  AppendCompletedFrame(bytes);
  REQUIRE(trace.Write(bytes));
  REQUIRE(reader.Open(trace.path().string()));
  CHECK(reader.frame_count() == 1);
}

TEST_CASE("TraceReader validates EDRAM requirement manifests", "[graphics][trace]") {
  SECTION("clean finalized tracked contract") {
    TemporaryTrace trace;
    const TraceEdramRequirements requirements = {
        1u << uint32_t(rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8),
        1u << uint32_t(rex::graphics::xenos::DepthRenderTargetFormat::kD24FS8),
        1u << uint32_t(rex::graphics::xenos::MsaaSamples::k4X),
    };
    std::vector<uint8_t> bytes = MakeTraceHeader(requirements);
    AppendCompletedFrame(bytes);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK(reader.edram_requirements_finalized());
    CHECK(reader.edram_requirements_tracked());
    CHECK(reader.has_finalized_edram_requirements());
    CHECK(reader.edram_requirements() == requirements);
  }

  SECTION("clean finalized unknown contract remains unknown") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader({}, false, true);
    AppendCompletedFrame(bytes);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK(reader.edram_requirements_finalized());
    CHECK_FALSE(reader.edram_requirements_tracked());
    CHECK_FALSE(reader.has_finalized_edram_requirements());
  }

  SECTION("abrupt unfinalized contract is readable but not deterministic") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader({}, false, false);
    AppendCompletedFrame(bytes);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK_FALSE(reader.edram_requirements_finalized());
    CHECK_FALSE(reader.has_finalized_edram_requirements());
  }

  SECTION("truncated manifest") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    bytes.resize(sizeof(TraceHeader) - 1);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    CHECK_FALSE(reader.Open(trace.path().string()));
  }

  SECTION("corrupt checksum") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    auto* header = reinterpret_cast<TraceHeader*>(bytes.data());
    header->edram_manifest.checksum ^= 1;
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    CHECK_FALSE(reader.Open(trace.path().string()));
  }

  SECTION("unknown format bit with a matching checksum") {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes = MakeTraceHeader();
    auto* header = reinterpret_cast<TraceHeader*>(bytes.data());
    header->edram_manifest.requirements.color_format_mask |= 1u << 9;
    header->edram_manifest.checksum =
        rex::graphics::ComputeTraceEdramManifestChecksum(header->edram_manifest);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    CHECK_FALSE(reader.Open(trace.path().string()));
  }
}

TEST_CASE("TraceWriter finalizes whole-capture EDRAM requirements",
          "[graphics][trace][trace_writer]") {
  SECTION("tracked requirements are published only on clean close") {
    TemporaryTrace trace;
    {
      TraceWriter writer(nullptr);
      REQUIRE(writer.Open(trace.path(), 0x12345678));
      REQUIRE(writer.BeginEdramRequirementsTracking());
      REQUIRE(writer.RequireEdramColorFormat(
          rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8));
      REQUIRE(writer.RequireEdramDepthFormat(
          rex::graphics::xenos::DepthRenderTargetFormat::kD24S8));
      REQUIRE(writer.RequireEdramMsaaSamples(rex::graphics::xenos::MsaaSamples::k2X));
      writer.WritePrimaryBufferEnd();
      writer.WriteEvent(EventCommand::Type::kSwap);
      writer.Close();
    }

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    REQUIRE(reader.has_finalized_edram_requirements());
    CHECK(reader.edram_requirements().color_format_mask == 1u);
    CHECK(reader.edram_requirements().depth_format_mask == 1u);
    CHECK(reader.edram_requirements().msaa_samples_mask == 2u);
  }

  SECTION("tracking disabled is finalized as unknown, not empty") {
    TemporaryTrace trace;
    {
      TraceWriter writer(nullptr);
      REQUIRE(writer.Open(trace.path(), 0));
      writer.WritePrimaryBufferEnd();
      writer.WriteEvent(EventCommand::Type::kSwap);
      writer.Close();
    }

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK(reader.edram_requirements_finalized());
    CHECK_FALSE(reader.edram_requirements_tracked());
  }

  SECTION("tracking must begin before initialization commands") {
    TemporaryTrace trace;
    TraceWriter writer(nullptr);
    REQUIRE(writer.Open(trace.path(), 0));
    const uint32_t register_value = 0;
    writer.WriteRegisters(0, &register_value, 1, false);
    CHECK_FALSE(writer.BeginEdramRequirementsTracking());
    writer.Close();
  }
}

TEST_CASE("TraceReader identifies standalone EDRAM initialization", "[graphics][trace]") {
  TemporaryTrace trace;

  SECTION("snapshot before GPU packets is initial state") {
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendEdramSnapshot(bytes);
    AppendCompletedFrame(bytes);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK(reader.has_initial_edram_snapshot());
  }

  SECTION("missing snapshot is not inferred as zero") {
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendCompletedFrame(bytes);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK_FALSE(reader.has_initial_edram_snapshot());
  }

  SECTION("snapshot after a packet is too late for standalone initialization") {
    std::vector<uint8_t> bytes = MakeTraceHeader();
    AppendPod(bytes, PacketStartCommand{TraceCommandType::kPacketStart, 0x1000, 1});
    AppendPod(bytes, uint32_t(0));
    AppendPod(bytes, rex::graphics::PacketEndCommand{TraceCommandType::kPacketEnd});
    AppendEdramSnapshot(bytes);
    AppendCompletedFrame(bytes);
    REQUIRE(trace.Write(bytes));

    TraceReader reader;
    REQUIRE(reader.Open(trace.path().string()));
    CHECK_FALSE(reader.has_initial_edram_snapshot());
  }
}

TEST_CASE("TraceDump fails closed for incomplete EDRAM restoration",
          "[graphics][trace][trace_dump]") {
  TemporaryTrace trace;
  std::vector<uint8_t> bytes = MakeTraceHeader();
  AppendEdramSnapshot(bytes);
  AppendCompletedFrame(bytes);
  REQUIRE(trace.Write(bytes));

  ContractTraceDump dump(false);
  CHECK(dump.Main({"trace_dump_test", trace.path().string()}) == 6);
}

TEST_CASE("Trace replay best-effort policy is explicit", "[graphics][trace][trace_dump]") {
  constexpr TraceReplayPreflight complete = {true, true, true, true};
  constexpr TraceReplayPreflight missing_snapshot = {false, true, true, true};
  constexpr TraceReplayPreflight unfinalized = {true, false, false, false};
  constexpr TraceReplayPreflight unknown_contract = {true, true, false, false};
  constexpr TraceReplayPreflight incomplete_backend = {true, true, true, false};

  STATIC_CHECK(complete.HasDeterministicEdramState());
  STATIC_CHECK(complete.PermitsReplay(false));
  STATIC_CHECK_FALSE(missing_snapshot.HasDeterministicEdramState());
  STATIC_CHECK_FALSE(missing_snapshot.PermitsReplay(false));
  STATIC_CHECK(missing_snapshot.PermitsReplay(true));
  STATIC_CHECK_FALSE(unfinalized.HasDeterministicEdramState());
  STATIC_CHECK_FALSE(unfinalized.PermitsReplay(false));
  STATIC_CHECK(unfinalized.PermitsReplay(true));
  STATIC_CHECK_FALSE(unknown_contract.HasDeterministicEdramState());
  STATIC_CHECK_FALSE(unknown_contract.PermitsReplay(false));
  STATIC_CHECK(unknown_contract.PermitsReplay(true));
  STATIC_CHECK_FALSE(incomplete_backend.HasDeterministicEdramState());
  STATIC_CHECK_FALSE(incomplete_backend.PermitsReplay(false));
  STATIC_CHECK(incomplete_backend.PermitsReplay(true));
}

TEST_CASE("TraceDump preflights the specific finalized EDRAM contract",
          "[graphics][trace][trace_dump]") {
  TemporaryTrace trace;
  const TraceEdramRequirements requirements = {
      1u << uint32_t(rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8),
      1u << uint32_t(rex::graphics::xenos::DepthRenderTargetFormat::kD24S8),
      1u << uint32_t(rex::graphics::xenos::MsaaSamples::k1X),
  };
  std::vector<uint8_t> bytes = MakeTraceHeader(requirements);
  AppendEdramSnapshot(bytes);
  AppendCompletedFrame(bytes);
  REQUIRE(trace.Write(bytes));

  ContractTraceDump unsupported(false);
  CHECK(unsupported.Main({"trace_dump_test", trace.path().string()}) == 6);

  // The supported contract passes preflight and reaches runtime setup. This
  // test backend intentionally returns no GraphicsSystem, so setup returns 4.
  ContractTraceDump supported(true);
  CHECK(supported.Main({"trace_dump_test", trace.path().string()}) == 4);
}

TEST_CASE("TraceDump rejects unfinalized and unknown EDRAM contracts",
          "[graphics][trace][trace_dump]") {
  for (const auto [requirements_tracked, finalized] :
       {std::pair{false, false}, std::pair{false, true}}) {
    TemporaryTrace trace;
    std::vector<uint8_t> bytes =
        MakeTraceHeader({}, requirements_tracked, finalized);
    AppendEdramSnapshot(bytes);
    AppendCompletedFrame(bytes);
    REQUIRE(trace.Write(bytes));

    ContractTraceDump supported_backend(true);
    CHECK(supported_backend.Main({"trace_dump_test", trace.path().string()}) == 6);
    // Best-effort passes the deterministic gate and reaches the intentionally
    // absent test GraphicsSystem.
    CHECK(supported_backend.Main(
              {"trace_dump_test", trace.path().string(), "--best-effort"}) == 4);
  }
}

TEST_CASE("TracePlayer latches canonical EDRAM restore failure",
          "[graphics][trace][trace_player]") {
  TemporaryTrace trace;
  std::vector<uint8_t> bytes = MakeTraceHeader();
  AppendEdramSnapshot(bytes);
  AppendCompletedFrame(bytes);
  REQUIRE(trace.Write(bytes));

  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());
  PlaybackTestGraphicsSystem graphics_system(memory);
  auto command_processor =
      std::make_unique<PlaybackCommandProcessor>(graphics_system, false);
  auto* command_processor_ptr = command_processor.get();
  command_processor->AcceptCallsForTest();
  graphics_system.InstallCommandProcessor(std::move(command_processor));

  {
    rex::graphics::TracePlayer player(&graphics_system);
    REQUIRE(player.Open(trace.path().string()));
    REQUIRE(player.SeekFrame(0));
    REQUIRE(command_processor_ptr->RunOnePendingCallForTest());
    CHECK_FALSE(player.WaitOnPlayback());
    CHECK(command_processor_ptr->restore_attempt_count() == 1);
  }
  graphics_system.Shutdown();
}

TEST_CASE("TracePlayer rejects Metal packets that live playback skips",
          "[graphics][trace][trace_player]") {
  constexpr uint32_t kPacketAddress = 0x1000;
  constexpr auto kUnsupportedOpcode =
      static_cast<rex::graphics::xenos::Type3Opcode>(0x7E);
  const uint32_t packet =
      rex::graphics::xenos::MakePacketType3(kUnsupportedOpcode, 1);

  TemporaryTrace trace;
  std::vector<uint8_t> bytes = MakeTraceHeader();
  AppendEdramSnapshot(bytes);
  AppendPod(bytes,
            PacketStartCommand{TraceCommandType::kPacketStart, kPacketAddress, 2});
  AppendPod(bytes, rex::byte_swap(packet));
  AppendPod(bytes, uint32_t(0));
  AppendPod(bytes,
            rex::graphics::PacketEndCommand{TraceCommandType::kPacketEnd});
  AppendCompletedFrame(bytes);
  REQUIRE(trace.Write(bytes));

  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());
  PlaybackTestGraphicsSystem graphics_system(memory);
  auto command_processor =
      std::make_unique<PlaybackCommandProcessor>(graphics_system, true);
  auto* command_processor_ptr = command_processor.get();

  // Preserve gameplay's established fail-soft policy for the same packet.
  std::memcpy(memory.TranslatePhysical(kPacketAddress),
              &bytes[sizeof(TraceHeader) + sizeof(EdramSnapshotCommand) +
                     rex::graphics::xenos::kEdramSizeBytes +
                     sizeof(PacketStartCommand)],
              sizeof(uint32_t) * 2);
  CHECK(command_processor_ptr->ExecutePacket(kPacketAddress, 2));

  command_processor->AcceptCallsForTest();
  graphics_system.InstallCommandProcessor(std::move(command_processor));
  {
    rex::graphics::TracePlayer player(&graphics_system);
    REQUIRE(player.Open(trace.path().string()));
    REQUIRE(player.SeekFrame(0));
    REQUIRE(command_processor_ptr->RunOnePendingCallForTest());
    CHECK_FALSE(player.WaitOnPlayback());
    CHECK(command_processor_ptr->restore_attempt_count() == 1);
  }
  graphics_system.Shutdown();
}
