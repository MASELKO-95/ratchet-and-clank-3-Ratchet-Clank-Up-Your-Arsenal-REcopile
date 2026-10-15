#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "Syscalls/System.h"
#include "Stubs/MPEG.h"

#include <cstring>
#include <vector>

void reset_ps2_test_function_table();

namespace {
constexpr uint32_t mpeg = 0x00123000u;
constexpr uint32_t packetAddress = 0x00128000u;
constexpr uint32_t callbackEntry = 0x00124000u;
constexpr uint32_t stopEntry = 0x00124ff0u;
constexpr uint32_t pictureEntry = 0x00125100u;
constexpr uint32_t noDataEntry = 0x00125200u;
uint32_t callbackCount, callbackType, callbackUser, callbackPayload, callbackLength;
uint32_t noDataCount, noDataUser;

void stop(uint8_t*, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0;
    runtime->requestStop();
}

void record(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t data = getRegU32(ctx, 5) & PS2_RAM_MASK;
    std::memcpy(&callbackType, ram + data, 4);
    std::memcpy(&callbackPayload, ram + data + 8, 4);
    std::memcpy(&callbackLength, ram + data + 12, 4);
    callbackUser = getRegU32(ctx, 6);
    ++callbackCount;
    stop(ram, ctx, runtime);
}

void picture(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    SET_GPR_U32(ctx, 4, mpeg);
    SET_GPR_U32(ctx, 5, 0x00130000u);
    ctx->pc = stopEntry;
    ps2_stubs::sceMpegGetPicture(ram, ctx, runtime);
}

void endInput(uint8_t*, R5900Context* ctx, PS2Runtime* runtime) {
    ++noDataCount;
    noDataUser = getRegU32(ctx, 5);
    const uint8_t end[] = {0, 0, 1, 0xb7};
    ps2_stubs::notifyMpegIpuInput(end, sizeof(end), runtime);
    ctx->pc = 0;
}

void registerStream(PS2Runtime& runtime, uint8_t* ram, uint32_t type, uint32_t channel,
                    uint32_t user) {
    R5900Context ctx{};
    SET_GPR_U32(&ctx, 4, mpeg);
    SET_GPR_U32(&ctx, 5, type);
    SET_GPR_U32(&ctx, 6, channel);
    SET_GPR_U32(&ctx, 7, callbackEntry);
    SET_GPR_U32(&ctx, 8, user);
    ps2_stubs::sceMpegAddStrCallback(ram, &ctx, &runtime);
}

void demux(PS2Runtime& runtime, std::vector<uint8_t>& ram, uint8_t stream,
           const std::vector<uint8_t>& payload) {
    const uint16_t length = static_cast<uint16_t>(payload.size() + 3);
    std::vector<uint8_t> packet{0, 0, 1, stream, static_cast<uint8_t>(length >> 8),
                               static_cast<uint8_t>(length), 0x80, 0, 0};
    packet.insert(packet.end(), payload.begin(), payload.end());
    std::memcpy(ram.data() + packetAddress, packet.data(), packet.size());
    R5900Context idle{};
    idle.pc = stopEntry;
    runtime.eeScheduler().reset(ram.data(), idle);
    R5900Context ctx{};
    SET_GPR_U32(&ctx, 4, mpeg);
    SET_GPR_U32(&ctx, 5, packetAddress);
    SET_GPR_U32(&ctx, 6, packet.size());
    SET_GPR_U32(&ctx, 7, packetAddress);
    SET_GPR_U32(&ctx, 8, packet.size());
    ps2_stubs::sceMpegDemuxPssRing(ram.data(), &ctx, &runtime);
    runtime.eeScheduler().run();
}

void prepare(PS2Runtime& runtime) {
    ps2_stubs::resetMpegStubState();
    callbackCount = callbackType = callbackUser = callbackPayload = callbackLength = 0;
    runtime.registerFunction(callbackEntry, record);
    runtime.registerFunction(stopEntry, stop);
}
}

int main() {
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    MiniTest::Case("MPEG stream routing", [](TestCase& tests) {
    tests.Run("Nodata callback runs before a native picture and publishes actual input end", [](auto& t) {
        PS2Runtime runtime;
        std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
        prepare(runtime);
        noDataCount = noDataUser = 0;
        runtime.registerFunction(pictureEntry, picture);
        runtime.registerFunction(noDataEntry, endInput);
        R5900Context registration{};
        SET_GPR_U32(&registration, 4, mpeg);
        SET_GPR_U32(&registration, 5, 1u);
        SET_GPR_U32(&registration, 6, noDataEntry);
        SET_GPR_U32(&registration, 7, 0x1234u);
        ps2_stubs::sceMpegAddCallback(ram.data(), &registration, &runtime);
        const uint32_t work = 0x00180000u;
        std::memcpy(ram.data() + mpeg + 0x40u, &work, sizeof(work));
        ps2_stubs::enqueueMpegDecodedFrameForTesting(mpeg);
        R5900Context ctx{};
        ctx.pc = pictureEntry;
        runtime.eeScheduler().reset(ram.data(), ctx);
        runtime.eeScheduler().run();
        uint32_t ended = 0, pending = 0;
        std::memcpy(&ended, ram.data() + work, 4);
        std::memcpy(&pending, ram.data() + work + 4, 4);
        t.Equals(noDataCount, 1u, "the producer callback runs once before completing the picture");
        t.Equals(noDataUser, 0x1234u, "Nodata receives the registered user data");
        t.Equals(ended, 1u, "original libmpeg IsEnd may observe the consumed trailer");
        t.Equals(pending, 0u, "no decoded reference pictures remain after the final picture");
    });
    tests.Run("CRT heap near RAM end supports callbacks without overlapping stacks", [](auto& t) {
        PS2Runtime runtime;
        std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
        prepare(runtime);
        R5900Context ctx{};
        ctx.pc = stopEntry;
        runtime.eeScheduler().reset(ram.data(), ctx);
        runtime.eeScheduler().bindMainContextForSyscall(ctx, ram.data());
        runtime.eeScheduler().setupCurrentThread(0x01ffc000u, 0x4000u, 0);
        SET_GPR_U32(&ctx, 4, 0x01ff8000u);
        SET_GPR_U32(&ctx, 5, 0x4000u);
        ps2_syscalls::SetupHeap(ram.data(), &ctx, &runtime);
        t.Equals(runtime.guestHeapBase(), 0x01ff8000u, "explicit high heap base is valid");
        t.Equals(runtime.guestHeapLimit(), 0x01ffc000u, "explicit heap size is retained");
        const uint32_t data = runtime.guestMalloc(0x20, 16);
        t.Equals(data, 0x01ff8000u, "MPEG callback descriptors must be allocated");
        const uint32_t callbackStack = runtime.reserveAsyncCallbackStack(0x4000u, 16);
        t.IsTrue(callbackStack >= 0x01f00000u && callbackStack < 0x01ff8000u,
                 "callback stack must stay below heap and main thread stack");
        runtime.guestFree(data);
    });
    tests.Run("PAL private audio type 3 reaches only its registered language channel", [](auto& t) {
        PS2Runtime runtime;
        std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
        prepare(runtime);
        // The first real PAL movie packets start FF A1 00 03; type 3/channel 3.
        registerStream(runtime, ram.data(), 3, 0, 0xbad0);
        registerStream(runtime, ram.data(), 2, 3, 0xbad2);
        registerStream(runtime, ram.data(), 3, 3, 0x1234);
        demux(runtime, ram, 0xbd, {0xff, 0xa1, 0, 3, 0x48, 0x53, 0x53, 0x64});
        t.Equals(callbackCount, 1u, "one matching audio callback must execute");
        t.Equals(callbackType, 3u, "audio packet must retain its libmpeg type");
        t.Equals(callbackUser, 0x1234u, "only the selected channel may receive audio");
        t.Equals(callbackPayload, packetAddress + 9, "callback retains private stream header");
        t.Equals(callbackLength, 8u, "callback receives the complete private payload");
    });
    tests.Run("unregistered private audio channel does not call another channel", [](auto& t) {
        PS2Runtime runtime;
        std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
        prepare(runtime);
        registerStream(runtime, ram.data(), 3, 1, 0x1234);
        demux(runtime, ram, 0xbd, {0xff, 0xa1, 0, 3, 0, 1});
        t.Equals(callbackCount, 0u, "a different channel must remain untouched");
    });
    tests.Run("video callback stream id selects its PES channel", [](auto& t) {
        PS2Runtime runtime;
        std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
        prepare(runtime);
        registerStream(runtime, ram.data(), 0, 0, 0xbad0);
        registerStream(runtime, ram.data(), 0, 1, 0x1234);
        demux(runtime, ram, 0xe1, {0, 0, 1, 0xb3, 0x14, 0, 0xf0, 0x13});
        t.Equals(callbackCount, 1u, "one matching video callback must execute");
        t.Equals(callbackType, 0u, "video keeps libmpeg type 0");
        t.Equals(callbackUser, 0x1234u, "PES E1 must select video channel 1");
    });
    });
    return MiniTest::Run();
}
