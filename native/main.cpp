#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "raylib.h"
#include "vulkan_gs.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <array>

namespace {
PS2Runtime::RecompiledFunction originalBind = nullptr;
PS2Runtime::RecompiledFunction originalCall = nullptr;
uint32_t tracedCalls = 0;
PS2Runtime::RecompiledFunction originalModuleTransfer = nullptr;
PS2Runtime::RecompiledFunction originalSetDma = nullptr;
PS2Runtime::RecompiledFunction originalPadUpdate = nullptr;
PS2Runtime::RecompiledFunction originalMpegDemux = nullptr;
PS2Runtime::RecompiledFunction originalMpegPicture = nullptr;
PS2Runtime::RecompiledFunction originalMpegStreamRegistration = nullptr;
PS2Runtime::RecompiledFunction originalMovieAudio = nullptr;
uint32_t tracedMpegDemux = 0;
uint32_t tracedMpegPictures = 0;
uint32_t tracedMovieAudio = 0;
uint32_t tracedPadUpdates = 0;
std::set<std::pair<uint32_t, uint32_t>> observedBindings;
struct Diagnostics {
    VulkanGS *renderer = nullptr;
    std::filesystem::path screenshot, memoryDump;
    std::ofstream gsEvents;
    uint64_t lastGsSequence = 0;
    std::array<uint64_t, 5> observedGsEvents{};
};
void initCapture(PS2Runtime&, void* userData) {
    auto& diagnostics = *static_cast<Diagnostics*>(userData);
    if (!diagnostics.memoryDump.empty()) {
        std::filesystem::create_directories(diagnostics.memoryDump);
        diagnostics.gsEvents.open(diagnostics.memoryDump / "gs-events.txt");
        if (!diagnostics.gsEvents) throw std::runtime_error("Failed to open GS event log");
    }
}
void collectGsEvents(PS2Runtime& runtime, void* userData) {
    auto& diagnostics = *static_cast<Diagnostics*>(userData);
    const auto events = runtime.gs().getDebugHistory();
    if (!events.empty() && events.back().seq < diagnostics.lastGsSequence)
        diagnostics.lastGsSequence = 0;
    for (const auto& event : events) {
        if (event.seq <= diagnostics.lastGsSequence) continue;
        ++diagnostics.observedGsEvents[static_cast<size_t>(event.kind)];
        if (diagnostics.gsEvents && event.kind != GSDebugEventKind::Present) {
            diagnostics.gsEvents << "seq=" << event.seq << " tick=" << event.vsyncTick
                << " kind=" << static_cast<int>(event.kind)
                << " reg=0x" << std::hex << static_cast<unsigned>(event.reg)
                << " value=0x" << event.regValue << std::dec
                << " nloop=" << event.gifNloop << " flg=" << static_cast<unsigned>(event.gifFlg)
                << " prim=" << static_cast<int>(event.prim.type)
                << " textured=" << event.prim.tme << " frame=" << event.frame.fbp
                << " fbw=" << event.frame.fbw << " psm=" << static_cast<unsigned>(event.frame.psm)
                << " scissor=" << event.scissor.x0 << ',' << event.scissor.x1 << ','
                << event.scissor.y0 << ',' << event.scissor.y1
                << " vertices=" << event.vertexCount << " bounds=" << event.xMin << ',' << event.xMax
                << ',' << event.yMin << ',' << event.yMax
                << " z=" << event.zMin << ',' << event.zMax
                << " alpha_range=" << static_cast<unsigned>(event.aMin) << ',' << static_cast<unsigned>(event.aMax)
                << " test=0x" << std::hex << event.test << " alpha=0x" << event.alpha << std::dec
                << " texture=" << event.tex0.tbp0 << ',' << static_cast<unsigned>(event.tex0.tbw) << ','
                << static_cast<unsigned>(event.tex0.psm) << ',' << event.tex0.cbp << ','
                << static_cast<unsigned>(event.tex0.cpsm) << ',' << static_cast<unsigned>(event.tex0.csm)
                << " transfer=" << event.transferPixels << " dest=" << event.bitbltbuf.dbp
                << " transfer_size=" << event.trxreg.rrw << ',' << event.trxreg.rrh << '\n';
        }
        diagnostics.lastGsSequence = event.seq;
    }
}
void saveCapture(PS2Runtime&, void* userData) {
    const auto& diagnostics = *static_cast<const Diagnostics*>(userData);
    const auto& path = diagnostics.screenshot;
    if (path.empty()) return;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    if (diagnostics.renderer) {
        diagnostics.renderer->capture(path.string());
        return;
    }
    Image frame = LoadImageFromScreen();
    const bool saved = ExportImage(frame, path.string().c_str());
    UnloadImage(frame);
    if (!saved) std::cerr << "[ratchet] Failed to export screenshot\n";
}
void tracePadUpdate(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t pc = cpu->pc;
    const uint32_t pad = getRegU32(cpu, 4);
    const uint32_t savedS0 = getRegU32(cpu, 16);
    const uint32_t sp = getRegU32(cpu, 29);
    const uint32_t ra = getRegU32(cpu, 31);
    if (pc == 0x3a3b40u && (tracedPadUpdates++ < 24u || pad < 0x1cd0c0u || pad >= 0x1cfec0u))
        std::cout << "[ratchet:pad-update] pc=0x" << std::hex << pc << " pad=0x" << pad
                  << " s0=0x" << savedS0 << " sp=0x" << sp << " ra=0x" << ra
                  << std::dec << '\n' << std::flush;
    originalPadUpdate(ram, cpu, runtime);
    if (cpu->pc == ra && (getRegU32(cpu, 16) != savedS0 || getRegU32(cpu, 29) != sp))
        std::cout << "[ratchet:pad-registers] entry=0x" << std::hex << pc
                  << " s0_before=0x" << savedS0 << " s0_after=0x" << getRegU32(cpu, 16)
                  << " sp_before=0x" << sp << " sp_after=0x" << getRegU32(cpu, 29)
                  << std::dec << '\n' << std::flush;
}
void traceMpegDemux(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const bool trace = tracedMpegDemux++ < 8u;
    if (trace) {
        std::cout << "[ratchet:mpeg-demux]" << std::hex;
        for (uint32_t reg = 4; reg <= 8; ++reg)
            std::cout << " r" << reg << "=0x" << getRegU32(cpu, reg);
        std::cout << std::dec << '\n' << std::flush;
    }
    originalMpegDemux(ram, cpu, runtime);
    if (trace) std::cout << "[ratchet:mpeg-demux-result] consumed=" << getRegU32(cpu, 2) << '\n' << std::flush;
}
void traceMpegPicture(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t mpeg = getRegU32(cpu, 4);
    const bool trace = tracedMpegPictures++ < 8u;
    if (trace) std::cout << "[ratchet:mpeg-picture] mpeg=0x" << std::hex << mpeg
                         << " image=0x" << getRegU32(cpu, 5) << std::dec << '\n' << std::flush;
    originalMpegPicture(ram, cpu, runtime);
    if (trace) std::cout << "[ratchet:mpeg-picture-result] status=" << static_cast<int32_t>(getRegU32(cpu, 2))
                         << " size=" << runtime->memory().read32(mpeg) << ',' << runtime->memory().read32(mpeg + 4)
                         << " frame=" << runtime->memory().read32(mpeg + 8) << '\n' << std::flush;
}
void traceMpegStreamRegistration(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    std::cout << "[ratchet:mpeg-stream] type=" << getRegU32(cpu, 5)
              << " channel=" << getRegU32(cpu, 6) << " callback=0x" << std::hex << getRegU32(cpu, 7)
              << " data=0x" << getRegU32(cpu, 8) << std::dec
              << " available=" << runtime->hasFunction(getRegU32(cpu, 7)) << '\n' << std::flush;
    originalMpegStreamRegistration(ram, cpu, runtime);
}
void traceMovieAudio(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const bool trace = tracedMovieAudio++ < 8u;
    const uint32_t data = getRegU32(cpu, 5);
    if (trace) std::cout << "[ratchet:movie-audio] packet=0x" << std::hex
                         << runtime->memory().read32(data + 8) << " length=0x"
                         << runtime->memory().read32(data + 12) << std::dec << '\n' << std::flush;
    originalMovieAudio(ram, cpu, runtime);
    if (trace) std::cout << "[ratchet:movie-audio-result] status=" << getRegU32(cpu, 2) << '\n' << std::flush;
}
void traceSetDma(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t list = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[4], 0));
    const uint32_t source = runtime->memory().read32(list);
    const uint32_t destination = runtime->memory().read32(list + 4);
    const uint32_t size = runtime->memory().read32(list + 8);
    originalSetDma(ram, cpu, runtime);
    std::cout << "[ratchet:sif-dma] src=0x" << std::hex << source << " dest=0x" << destination
              << " size=0x" << size << " result=0x" << _mm_extract_epi32(cpu->r[2], 0)
              << std::dec << '\n' << std::flush;
}
void traceModuleTransfer(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    std::cout << "[ratchet:module-transfer]" << std::hex;
    for (uint32_t reg = 4; reg <= 8; ++reg)
        std::cout << " r" << reg << "=0x" << _mm_extract_epi32(cpu->r[reg], 0);
    const uint32_t source = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[4], 0));
    std::cout << " magic=0x" << runtime->memory().read32(source) << std::dec << '\n' << std::flush;
    const uint32_t argumentSize = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[7], 0));
    const uint32_t argumentAddress = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[8], 0));
    std::cout << "[ratchet:module-arguments]" << std::hex;
    for (uint32_t i = 0; i < std::min(argumentSize, 128u); ++i)
        std::cout << ' ' << static_cast<unsigned>(runtime->memory().read8(argumentAddress + i));
    std::cout << std::dec << '\n' << std::flush;
    originalModuleTransfer(ram, cpu, runtime);
}
void traceCall(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t client = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[4], 0));
    const uint32_t function = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[5], 0));
    const uint32_t send = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[7], 0));
    // EE SDK uses n32: arguments 5-8 are in t0-t3, not on the IOP o32 stack.
    const uint32_t sendSize = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[8], 0));
    const uint32_t receive = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[9], 0));
    const bool trace = tracedCalls++ < 64u;
    if (trace) {
        std::cout << "[ratchet:rpc-call] client=0x" << std::hex << client << " function=0x" << function
                  << " send=0x" << send << " size=0x" << sendSize << " recv=0x" << receive << " words=";
        for (unsigned i=0; i<4; ++i) std::cout << ' ' << runtime->memory().read32(send+i*4u);
        std::cout << std::dec << '\n' << std::flush;
    }
    originalCall(ram, cpu, runtime);
    if (trace) std::cout << "[ratchet:rpc-result] status=" << _mm_extract_epi32(cpu->r[2], 0)
                         << " first=0x" << std::hex << runtime->memory().read32(receive) << std::dec << '\n' << std::flush;
}
void traceBind(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t client = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[4], 0));
    const uint32_t sid = static_cast<uint32_t>(_mm_extract_epi32(cpu->r[5], 0));
    originalBind(ram, cpu, runtime);
    uint32_t server = runtime->memory().read32(client + 36);
    if (observedBindings.emplace(sid, server).second) {
        std::cout << "[ratchet:rpc-bind] sid=0x" << std::hex << sid
                  << " server=0x" << server << " client=0x" << client
                  << std::dec << '\n' << std::flush;
    }
}
}

int main(int argc, char** argv) {
    std::filesystem::path elf, iso, files, memoryDump, screenshot;
    int seconds = 0;
    bool fsr = true, deinterlace = true;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--no-fsr") fsr = false;
        else if (argument == "--no-deinterlace") deinterlace = false;
        else if (argument == "--elf" && i + 1 < argc) elf = argv[++i];
        else if (argument == "--iso" && i + 1 < argc) iso = argv[++i];
        else if (argument == "--files" && i + 1 < argc) files = argv[++i];
        else if (argument == "--dump-memory" && i + 1 < argc) memoryDump = argv[++i];
        else if (argument == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (argument == "--seconds" && i + 1 < argc) {
            try { seconds = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "Invalid --seconds\n"; return 2; }
            if (seconds < 1 || seconds > 3600) return 2;
        } else {
            std::cerr << "Usage: ratchet-recomp --elf boot.elf --iso game.iso [--files directory] [--seconds 10] [--dump-memory directory] [--screenshot file.png]\n";
            return 2;
        }
    }
    if (!std::filesystem::is_regular_file(elf) || !std::filesystem::is_regular_file(iso)) {
        std::cerr << "Both --elf and --iso must point to existing files\n";
        return 2;
    }
    if (files.empty()) files = iso.parent_path() / "files";
    if (!std::filesystem::is_directory(files)) {
        std::cerr << "Disc files directory missing; extract ISO files or pass --files\n";
        return 2;
    }
    try {
        Diagnostics diagnostics;
        diagnostics.screenshot = screenshot;
        diagnostics.memoryDump = memoryDump;
        PS2Runtime runtime;
        runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        runtime.setDebugUiCallbacks(initCapture, collectGsEvents, saveCapture, &diagnostics);
        runtime.setHostCallbacks({
            [&](PS2Runtime &host, const char *title) {
                auto renderer = std::make_unique<VulkanGS>(true, fsr, deinterlace);
                diagnostics.renderer = renderer.get();
                renderer->open(host, title);
                host.gs().setRasterBackend(std::move(renderer));
            },
            [&](PS2Runtime &host) { return diagnostics.renderer->frame(host); },
            [] {} // Keep GPU memory alive for diagnostics after the EE thread joins.
        });
        PS2Runtime::IoPaths paths;
        paths.cdImage = std::filesystem::absolute(iso);
        PS2Runtime::setIoPaths(paths);
        std::cout << "[ratchet] ISO=" << paths.cdImage << '\n';
        if (!runtime.initialize("Ratchet & Clank 3 - RE:COPILE Edition (experimental)")) return 1;
        runtime.gs().setDebugHistoryPaused(false);
        if (!runtime.loadELF(std::filesystem::absolute(elf).string())) return 1;
        if (runtime.cpu().pc != 0x0013ada8u) {
            std::cerr << "[ratchet] Expected recovered SCES-52456 boot ELF\n";
            return 2;
        }
        paths = PS2Runtime::getIoPaths();
        paths.cdRoot = paths.hostRoot = std::filesystem::absolute(files);
        paths.mcRoot = paths.cdRoot.parent_path() / "memory-cards";
        PS2Runtime::setIoPaths(paths);
        // Exact PAL SDK address recovered by the analyzer. Preserve its behavior.
        originalBind = runtime.lookupFunction(0x00122630u);
        if (!originalBind || !runtime.replaceFunction(0x00122630u, traceBind)) return 1;
        originalCall = runtime.lookupFunction(0x00122810u);
        if (!originalCall || !runtime.replaceFunction(0x00122810u, traceCall)) return 1;
        originalModuleTransfer = runtime.lookupFunction(0x003981d8u);
        if (!originalModuleTransfer || !runtime.replaceFunction(0x003981d8u, traceModuleTransfer)) return 1;
        originalSetDma = runtime.lookupFunction(0x0011f1e0u);
        if (!originalSetDma || !runtime.replaceFunction(0x0011f1e0u, traceSetDma)) return 1;
        originalPadUpdate = runtime.lookupFunction(0x003a3b40u);
        if (!originalPadUpdate || !runtime.replaceFunction(0x003a3b40u, tracePadUpdate)) return 1;
        originalMpegDemux = runtime.lookupFunction(0x00139a70u);
        if (!originalMpegDemux || !runtime.replaceFunction(0x00139a70u, traceMpegDemux)) return 1;
        originalMpegPicture = runtime.lookupFunction(0x00135c10u);
        if (!originalMpegPicture || !runtime.replaceFunction(0x00135c10u, traceMpegPicture)) return 1;
        originalMpegStreamRegistration = runtime.lookupFunction(0x00139e50u);
        if (!originalMpegStreamRegistration || !runtime.replaceFunction(0x00139e50u, traceMpegStreamRegistration)) return 1;
        originalMovieAudio = runtime.lookupFunction(0x003b14c0u);
        if (!originalMovieAudio || !runtime.replaceFunction(0x003b14c0u, traceMovieAudio)) return 1;
        std::cout << "[ratchet] Boot ELF loaded; starting translated EE code\n" << std::flush;
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        std::thread watchdog;
        if (seconds) {
            watchdog = std::thread([&] {
                std::unique_lock lock(mutex);
                if (!done.wait_for(lock, std::chrono::seconds(seconds), [&] { return finished; })) {
                    std::cout << "[ratchet] Time limit reached; requesting stop\n" << std::flush;
                    runtime.requestStop();
                }
            });
        }
        try {
            runtime.run();
        } catch (...) {
            { std::lock_guard lock(mutex); finished = true; }
            done.notify_all();
            if (watchdog.joinable()) watchdog.join();
            throw;
        }
        { std::lock_guard lock(mutex); finished = true; }
        done.notify_all();
        if (watchdog.joinable()) watchdog.join();
        if (!screenshot.empty()) {
            if (!std::filesystem::is_regular_file(screenshot)) throw std::runtime_error("Failed to save screenshot");
            std::cout << "[ratchet] Screenshot=" << screenshot << '\n';
        }
        if (!memoryDump.empty()) {
            std::filesystem::create_directories(memoryDump);
            std::ofstream ee(memoryDump / "ee.bin", std::ios::binary);
            ee.write(reinterpret_cast<const char*>(runtime.memory().getRDRAM()), 32u * 1024u * 1024u);
            std::vector<uint8_t> iopBytes(2u * 1024u * 1024u);
            if (!runtime.readIopMemory(0u, iopBytes.data(), iopBytes.size()))
                throw std::runtime_error("Failed to snapshot IOP RAM");
            std::ofstream iopFile(memoryDump / "iop.bin", std::ios::binary);
            iopFile.write(reinterpret_cast<const char*>(iopBytes.data()), iopBytes.size());
            std::ofstream gsFile(memoryDump / "gs-vram.bin", std::ios::binary);
            std::vector<uint8_t> gsBytes;
            diagnostics.renderer->SnapshotVram(gsBytes);
            gsFile.write(reinterpret_cast<const char*>(gsBytes.data()), gsBytes.size());
            if (!ee || !iopFile || !gsFile) throw std::runtime_error("Failed to write memory snapshots");
            std::cout << "[ratchet] Memory snapshots=" << memoryDump << '\n';
        }
        const auto iop = runtime.iopDebugSnapshot();
        std::cout << "[ratchet:iop] modules=" << iop.emulatorLoadedModules
                  << " threads=" << iop.emulatorThreads << " rpc_servers=" << iop.emulatorRpcServers
                  << " instructions=" << iop.emulatorInstructions << '\n';
        for (const auto& message : iop.diagnostics)
            std::cout << "[ratchet:iop-diagnostic] " << message << '\n';
        for (const auto& service : iop.services) {
            std::cout << "[ratchet:iop-service] " << service.name << " active=" << service.active << " sids=";
            for (const auto sid : service.sids) std::cout << "0x" << std::hex << sid << ' ';
            std::cout << std::dec << '\n';
        }
        const auto& cpu = runtime.cpu();
        collectGsEvents(runtime, &diagnostics);
        std::cout << "[ratchet:gs-history] tags=" << diagnostics.observedGsEvents[0]
                  << " registers=" << diagnostics.observedGsEvents[1]
                  << " draws=" << diagnostics.observedGsEvents[2]
                  << " image_transfers=" << diagnostics.observedGsEvents[3] << '\n';
        const auto gs = runtime.gs().getDebugSnapshot();
        const auto& privileged = runtime.memory().gs();
        std::cout << "[ratchet:gs-state] vsync=" << privileged.vsyncTick.load()
                  << " pmode=0x" << std::hex << privileged.pmode
                  << " smode2=0x" << privileged.smode2
                  << " dispfb1=0x" << privileged.dispfb1 << " display1=0x" << privileged.display1
                  << " dispfb2=0x" << privileged.dispfb2 << " display2=0x" << privileged.display2
                  << std::dec << " presented=" << gs.hostPresentationWidth << ',' << gs.hostPresentationHeight
                  << " display_fbp=" << gs.hostPresentationDisplayFbp
                  << " source_fbp=" << gs.hostPresentationSourceFbp << '\n';
        for (int index = 0; index < 2; ++index) {
            const auto& context = gs.ctx[index];
            std::cout << "[ratchet:gs-context] index=" << index << " frame=" << context.frame.fbp
                      << " fbw=" << context.frame.fbw << " psm=" << static_cast<unsigned>(context.frame.psm)
                      << " scissor=" << context.scissor.x0 << ',' << context.scissor.x1 << ','
                      << context.scissor.y0 << ',' << context.scissor.y1
                      << " offset=" << context.xyoffset.ofx << ',' << context.xyoffset.ofy << '\n';
        }
        const auto scheduler = runtime.eeScheduler().snapshot();
        std::cout << "[ratchet:ee] cycles=" << scheduler.eeCycle << " running=" << scheduler.runningThreadId << '\n';
        std::cout << "[ratchet:registers]" << std::hex;
        for (uint32_t reg = 2; reg < 32; ++reg)
            std::cout << " r" << std::dec << reg << "=0x" << std::hex
                      << static_cast<uint64_t>(_mm_extract_epi64(cpu.r[reg], 0));
        std::cout << std::dec << " pad_updates=" << tracedPadUpdates << '\n';
        for (const auto& thread : scheduler.threads)
            std::cout << "[ratchet:ee-thread] id=" << thread.id << " priority=" << thread.currentPriority
                      << " status=" << static_cast<unsigned>(thread.status)
                      << " wait=" << static_cast<unsigned>(thread.waitReason) << " invocations=" << thread.invocationDepth
                      << " pc=0x" << std::hex << thread.pc << " ra=0x" << thread.ra << " sp=0x" << thread.sp
                      << std::dec << '\n';
        std::cout << "[ratchet:disc-state]" << std::hex;
        for (uint32_t offset = 0; offset < 32u; offset += 4u)
            std::cout << " 0x" << (0x1ccfd0u + offset) << "=0x" << runtime.memory().read32(0x1ccfd0u + offset);
        for (const uint32_t address : {0x1d4dacu, 0x1d4db0u, 0x1d4db4u, 0x1d4db8u, 0x1d4de0u, 0x1d4980u})
            std::cout << " 0x" << address << "=0x" << runtime.memory().read32(address);
        std::cout << std::dec << '\n';
        std::cout << "[ratchet:state] pc=0x" << std::hex << cpu.pc
                  << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(cpu.r[31], 0))
                  << " sp=0x" << static_cast<uint32_t>(_mm_extract_epi32(cpu.r[29], 0))
                  << " gp=0x" << static_cast<uint32_t>(_mm_extract_epi32(cpu.r[28], 0))
                  << std::dec << " dma=" << runtime.memory().dmaStartCount()
                  << " gif=" << runtime.memory().gifCopyCount()
                  << " gs=" << runtime.memory().gsWriteCount()
                  << " vif=" << runtime.memory().vifWriteCount() << '\n';
        std::cout << "[ratchet] Runtime returned (this does not establish a successful game boot)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ratchet] Fatal: " << error.what() << '\n';
        return 1;
    }
}
