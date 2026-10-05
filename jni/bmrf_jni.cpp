// JNI bridge between the Kotlin UI and the native BMRF core.
//
// The Kotlin side drives the core by calling nativePoll(handle, nowMs) on a
// background thread. Each call returns a JSON snapshot:
//
//   { "status": {...}, "events": [ {...}, ... ] }
//
// This keeps the JNI surface tiny and avoids a large object-marshalling layer.

#include <jni.h>

#include "bmrf/audio/audio_output.hpp"
#include "bmrf/audio/voice_pipeline.hpp"
#include "bmrf/brandmeister/client.hpp"
#include "bmrf/brandmeister/rx.hpp"
#include "bmrf/brandmeister/transport.hpp"
#include "bmrf/common/log.hpp"
#include "bmrf/common/time.hpp"
#include "bmrf/dmr/call_manager.hpp"
#include "bmrf/dmr/event_bus.hpp"
#include "bmrf/rf/burst_source.hpp"
#include "bmrf/rf/rf_receiver.hpp"
#include "bmrf/capture/capture.hpp"
#include "bmrf/rf/rtl_sdr.hpp"
#include "bmrf/rf/rtl_sdr_burst_source.hpp"
#include "bmrf/vocoder/ambe_decoder.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using bmrf::DMRCall;
using bmrf::DMREvent;
using bmrf::DMRFrame;
using bmrf::brandmeister::HomebrewClient;
using bmrf::brandmeister::HomebrewParser;
using bmrf::brandmeister::PacketType;
using bmrf::brandmeister::UdpTransport;

struct CoreContext {
    bmrf::EventBus bus;
    bmrf::CallManager callManager{&bus};
    std::unique_ptr<bmrf::EventBus::Subscription> subscription;
    std::unique_ptr<HomebrewClient> client;
    std::unique_ptr<bmrf::VoicePipeline> pipeline;
    std::unique_ptr<bmrf::rf::RfReceiver> rfReceiver;
    std::vector<uint8_t> sdrScratch;
    std::mutex mutex;

    std::string masterHost = "brandmeister.network";
    uint16_t masterPort = 62031;
    uint32_t repeaterId = 0;
    bool started = false;
    bool bmAudio = false;
    bool rfAudio = false;
};

CoreContext* toContext(jlong handle)
{
    return reinterpret_cast<CoreContext*>(handle);
}

std::string escapeJson(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    return out;
}

template <typename T>
std::string optionalNumber(const std::optional<T>& value)
{
    if (!value)
        return "null";
    std::ostringstream stream;
    stream << *value;
    return stream.str();
}

// uint8_t must be widened, otherwise the stream prints it as a character.
std::string optionalByte(const std::optional<uint8_t>& value)
{
    if (!value)
        return "null";
    return std::to_string(static_cast<int>(*value));
}

std::string callToJson(const DMRCall& call)
{
    std::ostringstream out;
    out << "{";
    out << "\"callId\":" << call.call_id;
    out << ",\"sourceId\":" << call.source_id;
    out << ",\"destId\":" << call.destination_id;
    out << ",\"callType\":\"" << bmrf::toString(call.call_type) << "\"";
    out << ",\"state\":\"" << bmrf::toString(call.state) << "\"";
    out << ",\"source\":\"" << bmrf::toString(call.source) << "\"";
    out << ",\"timeslot\":" << optionalNumber(call.timeslot);
    out << ",\"colorCode\":" << optionalNumber(call.color_code);
    out << ",\"flco\":" << optionalByte(call.flco);
    out << ",\"serviceOptions\":" << optionalByte(call.service_options);
    out << ",\"frameCount\":" << call.frame_count;
    out << ",\"lostFrameCount\":" << call.lost_frame_count;
    out << ",\"durationMs\":" << call.durationMs();
    out << ",\"startNs\":" << call.start_timestamp_ns;
    out << ",\"lastFrameNs\":" << call.last_frame_timestamp_ns;
    out << ",\"rssi\":" << optionalNumber(call.rssi);
    out << ",\"snr\":" << optionalNumber(call.snr);
    out << ",\"ber\":" << optionalNumber(call.ber);
    out << "}";
    return out.str();
}

std::string frameToJson(const DMRFrame& frame)
{
    std::ostringstream out;
    out << "{";
    out << "\"sourceId\":" << frame.source_id;
    out << ",\"destId\":" << frame.destination_id;
    out << ",\"timeslot\":" << frame.timeslot;
    out << ",\"dataType\":\"" << bmrf::toString(frame.data_type) << "\"";
    out << ",\"callType\":\"" << bmrf::toString(frame.call_type) << "\"";
    out << ",\"sequence\":" << frame.sequence;
    out << ",\"hasVoice\":" << (frame.has_voice ? "true" : "false");
    out << ",\"rssi\":" << optionalNumber(frame.rssi_dbm);
    out << ",\"snr\":" << optionalNumber(frame.snr_db);
    out << ",\"ber\":" << optionalNumber(frame.ber_percent);
    out << "}";
    return out.str();
}

std::string eventToJson(const DMREvent& event)
{
    std::ostringstream out;
    out << "{";
    out << "\"type\":\"" << bmrf::toString(event.type) << "\"";
    out << ",\"message\":\"" << escapeJson(event.message) << "\"";
    out << ",\"timestampNs\":" << event.timestamp_ns;
    out << ",\"call\":" << callToJson(event.call);
    out << ",\"frame\":";
    if (event.frame)
        out << frameToJson(*event.frame);
    else
        out << "null";
    out << "}";
    return out.str();
}

std::string statusToJson(const CoreContext& ctx)
{
    std::ostringstream out;
    out << "{";
    out << "\"state\":\""
        << (ctx.client ? bmrf::brandmeister::toString(ctx.client->state()) : "DISCONNECTED")
        << "\"";
    out << ",\"master\":\"" << escapeJson(ctx.masterHost) << "\"";
    out << ",\"bmPeer\":\""
        << escapeJson(ctx.client ? ctx.client->masterPeer() : std::string()) << "\"";
    out << ",\"port\":" << ctx.masterPort;
    out << ",\"repeaterId\":" << ctx.repeaterId;
    out << ",\"latencyMs\":" << (ctx.client ? ctx.client->latencyMs() : 0u);
    out << ",\"loggedIn\":" << ((ctx.client && ctx.client->loggedIn()) ? "true" : "false");
    out << ",\"activeCalls\":" << ctx.callManager.activeCallCount();
    out << ",\"decoder\":\"" << (ctx.pipeline ? ctx.pipeline->decoderName() : "none") << "\"";
    out << ",\"audioEnabled\":" << (ctx.bmAudio ? "true" : "false");
    out << ",\"audioPlaying\":" << ((ctx.pipeline && ctx.pipeline->playing()) ? "true" : "false");
    out << ",\"audioQueue\":" << (ctx.pipeline ? ctx.pipeline->queueSize() : 0);
    out << ",\"audioLatencyMs\":" << (ctx.pipeline ? ctx.pipeline->lastLatencyMs() : 0ULL);
    const bmrf::CallManagerStats stats = ctx.callManager.stats();
    out << ",\"totalCalls\":" << stats.total_calls;
    out << ",\"totalFrames\":" << stats.total_frames;
    out << ",\"totalLost\":" << stats.total_lost;

    const bmrf::rf::RtlSdrStatus sdr = bmrf::rf::rtlSdrStatus();
    out << ",\"sdrOpen\":" << (sdr.open ? "true" : "false");
    out << ",\"sdrCenterHz\":" << sdr.center_hz;
    out << ",\"sdrSampleRate\":" << sdr.sample_rate_hz;
    out << ",\"sdrSamples\":" << sdr.samples_read;
    out << ",\"sdrRms\":" << sdr.rms;

    out << ",\"rfAudioEnabled\":"
        << ((ctx.rfReceiver && ctx.rfReceiver->audioEnabled()) ? "true" : "false");
    out << ",\"rfForwardEnabled\":"
        << ((ctx.rfReceiver && ctx.rfReceiver->forwardEnabled()) ? "true" : "false");
    out << ",\"rfForwarded\":" << (ctx.rfReceiver ? ctx.rfReceiver->forwardedFrames() : 0);
    out << ",\"rfLevelDb\":" << (ctx.rfReceiver ? ctx.rfReceiver->rfLevelDb() : 0.0);
    out << ",\"rfSnrDb\":" << (ctx.rfReceiver ? ctx.rfReceiver->rfSnrDb() : 0.0);
    out << "}";
    return out.str();
}

std::string snapshotToJson(CoreContext& ctx, const std::vector<DMREvent>& events)
{
    std::ostringstream out;
    out << "{\"status\":" << statusToJson(ctx) << ",\"events\":[";
    for (size_t i = 0; i < events.size(); ++i) {
        if (i != 0)
            out << ",";
        out << eventToJson(events[i]);
    }
    out << "]}";
    return out.str();
}

std::string toString(JNIEnv* env, jstring value)
{
    if (value == nullptr)
        return std::string();
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars != nullptr ? std::string(chars) : std::string();
    if (chars != nullptr)
        env->ReleaseStringUTFChars(value, chars);
    return result;
}

std::vector<uint8_t> toBytes(JNIEnv* env, jbyteArray array)
{
    if (array == nullptr)
        return {};
    const jsize length = env->GetArrayLength(array);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (length > 0)
        env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    return bytes;
}

std::vector<DMREvent> drain(CoreContext& ctx)
{
    if (!ctx.subscription)
        return {};
    return ctx.subscription->popAll();
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_bmrf_gateway_BmrfCore_nativeCreate(JNIEnv*, jobject)
{
    auto* ctx = new CoreContext();
    ctx->subscription = ctx->bus.subscribe(8192);
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeDestroy(JNIEnv*, jobject, jlong handle)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    if (ctx->subscription)
        ctx->subscription->close();
    if (ctx->client)
        ctx->client->stop();
    if (ctx->rfReceiver)
        ctx->rfReceiver->stop();
    if (ctx->pipeline)
        ctx->pipeline->stop();
    delete ctx;
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeConfigure(
    JNIEnv* env, jobject, jlong handle, jstring host, jint port, jint repeaterId,
    jstring callsign, jstring password)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;

    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->masterHost = toString(env, host);
    ctx->masterPort = static_cast<uint16_t>(port);
    ctx->repeaterId = static_cast<uint32_t>(repeaterId);

    bmrf::brandmeister::ClientConfig config;
    config.master_host = ctx->masterHost;
    config.master_port = ctx->masterPort;
    config.repeater_id = ctx->repeaterId;
    config.password = toString(env, password);
    config.repeater.id = ctx->repeaterId;
    config.repeater.callsign = toString(env, callsign);
    if (config.repeater.callsign.empty())
        config.repeater.callsign = "BMRF";
    // Advertise the exact software/package identity DroidStar uses: the native
    // MMDVM RPTC is rejected with MSTNAK unless the software field looks like a
    // dated MMDVM build ("YYYYMMDD_..."), and BrandMeister only authorises
    // transmission for recognised hotspot software.
    config.repeater.software_id = "20181107_Pi-Star";
    config.repeater.package_id = "MMDVM_MMDVM_HS_Hat";
    config.repeater.url = "www.qrz.com";
    config.repeater.tx_power = 1;

    // BrandMeister's master rejects an RPTC configuration whose RX/TX frequency
    // is zero (it answers MSTNAK). Advertise the SDR centre frequency when the
    // radio is open, otherwise a sane non-zero default, plus a valid colour code.
    {
        uint32_t rfFreq = 438800000U;
        const bmrf::rf::RtlSdrStatus sdr = bmrf::rf::rtlSdrStatus();
        if (sdr.center_hz > 0)
            rfFreq = sdr.center_hz;
        config.repeater.rx_freq = rfFreq;
        config.repeater.tx_freq = rfFreq;
        config.repeater.color_code = 1;
    }

    ctx->client = std::make_unique<HomebrewClient>(
        std::make_unique<UdpTransport>(), std::move(config));
    ctx->client->setCallManager(&ctx->callManager);
    ctx->client->setEventBus(&ctx->bus);

    ctx->pipeline = std::make_unique<bmrf::VoicePipeline>(
        [] { return bmrf::createAmbeDecoder(); }, bmrf::createAudioOutput());
    ctx->client->setVoicePipeline(ctx->pipeline.get());

    ctx->rfReceiver =
        std::make_unique<bmrf::rf::RfReceiver>(bmrf::rf::createDefaultRfBurstSource());
    {
        std::string rfError;
        ctx->rfReceiver->open("", rfError);
    }
    ctx->rfReceiver->setCallManager(&ctx->callManager);
    ctx->rfReceiver->setEventBus(&ctx->bus);
    ctx->rfReceiver->setVoicePipeline(ctx->pipeline.get());
    {
        auto* client = ctx->client.get();
        ctx->rfReceiver->setUplink([client](const bmrf::DMRFrame& frame) {
            return client != nullptr && client->sendFrame(frame);
        });
    }
    ctx->started = false;
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeStart(JNIEnv*, jobject, jlong handle)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->pipeline)
        ctx->pipeline->start();
    if (ctx->rfReceiver)
        ctx->rfReceiver->start();
    if (ctx->client) {
        ctx->client->start(bmrf::nowMs());
        ctx->started = true;
    }
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeStop(JNIEnv*, jobject, jlong handle)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->client)
        ctx->client->stop();
    if (ctx->rfReceiver)
        ctx->rfReceiver->stop();
    if (ctx->pipeline)
        ctx->pipeline->stop();
    ctx->started = false;
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeStartRf(JNIEnv*, jobject, jlong handle)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->pipeline)
        ctx->pipeline->start();
    if (ctx->rfReceiver)
        ctx->rfReceiver->start();
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSetRfAudioEnabled(
    JNIEnv*, jobject, jlong handle, jboolean enabled)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->rfAudio = (enabled == JNI_TRUE);
    if (ctx->rfReceiver)
        ctx->rfReceiver->setAudioEnabled(ctx->rfAudio);
    if (ctx->pipeline)
        ctx->pipeline->setEnabled(ctx->bmAudio || ctx->rfAudio);
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSetRfForwardEnabled(
    JNIEnv*, jobject, jlong handle, jboolean enabled)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->rfReceiver)
        ctx->rfReceiver->setForwardEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSetCaptureOnly(
    JNIEnv*, jobject, jlong, jboolean captureOnly)
{
    bmrf::rf::rtlSdrBurstSourceSetCaptureOnly(captureOnly == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSetRfTimeslot(
    JNIEnv*, jobject, jlong handle, jint slot)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->rfReceiver)
        ctx->rfReceiver->setTimeslot(static_cast<int>(slot));
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSetAudioEnabled(
    JNIEnv*, jobject, jlong handle, jboolean enabled)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->bmAudio = (enabled == JNI_TRUE);
    if (ctx->client)
        ctx->client->setAudioEnabled(ctx->bmAudio);
    if (ctx->pipeline)
        ctx->pipeline->setEnabled(ctx->bmAudio || ctx->rfAudio);
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSetAudioTalkgroups(
    JNIEnv* env, jobject, jlong handle, jstring csv)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr || ctx->client == nullptr)
        return;
    const std::string text = toString(env, csv);
    std::vector<uint32_t> talkgroups;
    std::string token;
    auto flush = [&]() {
        if (token.empty())
            return;
        try {
            const long value = std::stol(token);
            if (value > 0)
                talkgroups.push_back(static_cast<uint32_t>(value));
        } catch (...) {
        }
        token.clear();
    };
    for (char c : text) {
        if (c == ',' || c == ' ' || c == ';' || c == '\t')
            flush();
        else
            token.push_back(c);
    }
    flush();
    ctx->client->setAudioTalkgroups(std::move(talkgroups));
}

JNIEXPORT jstring JNICALL Java_com_bmrf_gateway_BmrfCore_nativePoll(
    JNIEnv* env, jobject, jlong handle, jlong /*nowMs*/)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return env->NewStringUTF("{\"status\":null,\"events\":[]}");

    std::lock_guard<std::mutex> lock(ctx->mutex);
    // Use the native monotonic clock so it matches HomebrewClient::start();
    // the caller-supplied wall-clock value is intentionally ignored.
    const uint64_t now = bmrf::nowMs();

    if (ctx->client)
        ctx->client->clock(now);

    // The RF receiver thread reads the SDR when present; only read here when
    // no RF receiver exists (e.g. SDR opened without a core configure).
    if (!ctx->rfReceiver && bmrf::rf::rtlSdrIsOpen()) {
        if (ctx->sdrScratch.size() < 16384)
            ctx->sdrScratch.resize(16384);
        bmrf::rf::rtlSdrRead(ctx->sdrScratch.data(), 8192);
    }

    ctx->callManager.tick(bmrf::msToNs(now));

    const std::string json = snapshotToJson(*ctx, drain(*ctx));
    return env->NewStringUTF(json.c_str());
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSetUsbFd(
    JNIEnv*, jobject, jlong, jint fd)
{
    bmrf::rf::rtlSdrSetAndroidFd(static_cast<int>(fd));
}

JNIEXPORT jboolean JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSdrOpen(
    JNIEnv*, jobject, jlong, jint centerHz, jint sampleRateHz, jint gainTenthsDb, jint ppm)
{
    std::string error;
    const bool ok = bmrf::rf::rtlSdrOpen(static_cast<uint32_t>(centerHz),
                                         static_cast<uint32_t>(sampleRateHz),
                                         static_cast<int>(gainTenthsDb),
                                         static_cast<int>(ppm), error);
    if (!ok)
        bmrf::logWarn("rtlsdr", "open failed: " + error);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSdrClose(JNIEnv*, jobject, jlong)
{
    bmrf::rf::rtlSdrClose();
}

JNIEXPORT jboolean JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSdrRecord(
    JNIEnv* env, jobject, jlong, jstring path)
{
    std::string error;
    const bool ok = bmrf::rf::rtlSdrStartRecording(toString(env, path), error);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeSdrStopRecord(JNIEnv*, jobject, jlong)
{
    bmrf::rf::rtlSdrStopRecording();
}

JNIEXPORT jboolean JNICALL Java_com_bmrf_gateway_BmrfCore_nativeCaptureStart(
    JNIEnv* env, jobject, jlong, jstring path)
{
    std::string error;
    const bool ok = bmrf::captureStart(toString(env, path), error);
    if (!ok)
        bmrf::logWarn("capture", error);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_bmrf_gateway_BmrfCore_nativeCaptureStop(
    JNIEnv*, jobject, jlong)
{
    bmrf::captureStop();
}

JNIEXPORT jstring JNICALL Java_com_bmrf_gateway_BmrfCore_nativeInject(
    JNIEnv* env, jobject, jlong handle, jbyteArray datagram, jlong /*nowMs*/)
{
    auto* ctx = toContext(handle);
    if (ctx == nullptr)
        return env->NewStringUTF("{\"status\":null,\"events\":[]}");

    const std::vector<uint8_t> bytes = toBytes(env, datagram);
    const uint64_t now = bmrf::nowMs();

    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->client) {
        ctx->client->inject(bytes.data(), bytes.size(), now);
    } else if (!bytes.empty()) {
        const auto packet = HomebrewParser::parse(bytes.data(), bytes.size());
        if (packet.type == PacketType::DmrData && packet.dmr_data)
            ctx->callManager.onFrame(HomebrewParser::toFrame(*packet.dmr_data, bmrf::msToNs(now)));
    }

    const std::string json = snapshotToJson(*ctx, drain(*ctx));
    return env->NewStringUTF(json.c_str());
}

JNIEXPORT jstring JNICALL Java_com_bmrf_gateway_BmrfCore_nativeVersion(JNIEnv* env, jobject)
{
    return env->NewStringUTF("BMRF-Gateway/0.1");
}

} // extern "C"
