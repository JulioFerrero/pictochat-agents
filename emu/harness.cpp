// Headless two-DS harness around the melonDS core (GPLv3 core, linked as libcore.a).
// Runs N DS instances (own thread each) with a real firmware, bridges their Wi-Fi through melonDS's LocalMP,
// writes every Wi-Fi frame to a pcap, and takes commands on stdin:
//   fast on|off          run unpaced / at 60 fps
//   touch I X Y [N]      hold the stylus at X,Y on instance I for N frames (default 6)
//   key I MASK N         hold keys (bit0 A,1 B,2 Select,3 Start,4 Right,5 Left,6 Up,7 Down,8 R,9 L,10 X,11 Y) for N frames
//   shot I FILE.ppm      write both screens (256x384) of instance I
//   frames               print frame counters
//   quit
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <semaphore.h>
#include <deque>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

#define private public
#include "NDS.h"
#undef private
#include "Args.h"
#include "Platform.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "SPI_Firmware.h"
#include "RTC.h"
#include "net/LocalMP.h"

using namespace melonDS;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- Platform
struct Inst;
namespace melonDS::Platform
{
struct FileHandle { FILE* f; };
struct Thread { std::thread t; };
struct Semaphore { sem_t s; };
struct Mutex { std::mutex m; };
struct AACDecoder {};
struct DynamicLibrary {};

static Clock::time_point g_t0 = Clock::now();
static LocalMP* g_mp = nullptr;
static std::mutex g_pcap_mutex;
static FILE* g_pcap = nullptr;
static std::atomic<bool> g_verbose{false};

// ---- external-host bridge (bridge mode: one emulated client, the host is a TCP peer, e.g. pictochat.py) ----
struct Bridge
{
    int lfd = -1;
    std::atomic<int> cfd{-1};
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<u8>> q;      // frames from the host, as the core expects them (12-byte header + frame)
    std::mutex wm;                      // serialises writes to the peer
};
static Bridge* g_bridge = nullptr;
static void BridgeSend(u8 kind, u16 aid, u64 ts, const u8* d, int len)
{
    int fd = g_bridge->cfd;
    if (fd < 0) return;
    std::vector<u8> msg(4 + 1 + 2 + 8 + len);
    u32 n = 1 + 2 + 8 + len;
    memcpy(&msg[0], &n, 4); msg[4] = kind; memcpy(&msg[5], &aid, 2); memcpy(&msg[7], &ts, 8);
    if (len) memcpy(&msg[15], d, len);
    std::lock_guard<std::mutex> g(g_bridge->wm);
    size_t off = 0;
    while (off < msg.size()) { ssize_t w = send(fd, msg.data() + off, msg.size() - off, MSG_NOSIGNAL); if (w <= 0) break; off += w; }
}
static u64 BridgeNow(void* u);

int InstId(void* userdata);

static void PcapWrite(int inst, const char* kind, u8* data, int len, u64 ts)
{
    if (!g_pcap || len <= 12) return;
    std::lock_guard<std::mutex> g(g_pcap_mutex);
    static const u8 rt[8] = {0, 0, 8, 0, 0, 0, 0, 0};
    u32 caplen = 8 + (len - 12);
    u32 sec = (u32)(ts / 1000000), usec = (u32)(ts % 1000000);
    u32 hdr[4] = {sec, usec, caplen, caplen};
    fwrite(hdr, 4, 4, g_pcap);
    fwrite(rt, 1, 8, g_pcap);
    fwrite(data + 12, 1, len - 12, g_pcap);
    fflush(g_pcap);
    (void)inst; (void)kind;
}

void SignalStop(StopReason, void*) {}
std::string GetLocalFilePath(const std::string& f) { return f; }
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    const char* m = (mode & FileMode::Write) ? ((mode & FileMode::Read) ? ((mode & FileMode::Preserve) ? "r+b" : "w+b") : "wb") : "rb";
    FILE* f = fopen(path.c_str(), m);
    return f ? new FileHandle{f} : nullptr;
}
FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }
bool FileExists(const std::string& n) { FILE* f = fopen(n.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }
bool LocalFileExists(const std::string& n) { return FileExists(n); }
bool CheckFileWritable(const std::string&) { return false; }
bool CheckLocalFileWritable(const std::string&) { return false; }
bool CloseFile(FileHandle* h) { if (!h) return false; fclose(h->f); delete h; return true; }
bool IsEndOfFile(FileHandle* h) { return feof(h->f); }
bool FileReadLine(char* s, int n, FileHandle* h) { return fgets(s, n, h->f) != nullptr; }
u64 FilePosition(FileHandle* h) { return ftell(h->f); }
bool FileSeek(FileHandle* h, s64 off, FileSeekOrigin o) { return fseek(h->f, off, o == FileSeekOrigin::Start ? SEEK_SET : o == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END) == 0; }
void FileRewind(FileHandle* h) { rewind(h->f); }
u64 FileRead(void* d, u64 s, u64 c, FileHandle* h) { return fread(d, s, c, h->f); }
bool FileFlush(FileHandle* h) { return fflush(h->f) == 0; }
u64 FileWrite(const void* d, u64 s, u64 c, FileHandle* h) { return fwrite(d, s, c, h->f); }
u64 FileWriteFormatted(FileHandle* h, const char* fmt, ...) { va_list a; va_start(a, fmt); int r = vfprintf(h->f, fmt, a); va_end(a); return r; }
u64 FileLength(FileHandle* h) { long p = ftell(h->f); fseek(h->f, 0, SEEK_END); long l = ftell(h->f); fseek(h->f, p, SEEK_SET); return l; }

void Log(LogLevel level, const char* fmt, ...)
{
    if (!g_verbose && level < LogLevel::Warn) return;
    va_list a; va_start(a, fmt); fprintf(stderr, "[core] "); vfprintf(stderr, fmt, a); va_end(a);
}

Thread* Thread_Create(std::function<void()> func) { return new Thread{std::thread(std::move(func))}; }
void Thread_Free(Thread* t) { if (t->t.joinable()) t->t.detach(); delete t; }
void Thread_Wait(Thread* t) { if (t->t.joinable()) t->t.join(); }
Semaphore* Semaphore_Create() { auto* s = new Semaphore; sem_init(&s->s, 0, 0); return s; }
void Semaphore_Free(Semaphore* s) { sem_destroy(&s->s); delete s; }
void Semaphore_Reset(Semaphore* s) { while (sem_trywait(&s->s) == 0) {} }
void Semaphore_Wait(Semaphore* s) { while (sem_wait(&s->s) != 0) {} }
bool Semaphore_TryWait(Semaphore* s, int timeout_ms)
{
    if (timeout_ms == 0) return sem_trywait(&s->s) == 0;
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000; ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r; while ((r = sem_timedwait(&s->s, &ts)) != 0 && errno == EINTR) {}
    return r == 0;
}
void Semaphore_Post(Semaphore* s, int count) { for (int i = 0; i < count; i++) sem_post(&s->s); }
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->m.lock(); }
void Mutex_Unlock(Mutex* m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->m.try_lock(); }
void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }
u64 GetMSCount() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_t0).count(); }
u64 GetUSCount() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_t0).count(); }

void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void* u) { if (!g_bridge) g_mp->Begin(InstId(u)); }
void MP_End(void* u) { if (!g_bridge) g_mp->End(InstId(u)); }
int MP_SendPacket(u8* d, int l, u64 ts, void* u) { PcapWrite(InstId(u), "pkt", d, l, ts); if (g_bridge) { BridgeSend(0, 0, ts, d, l); return l; } return g_mp->SendPacket(InstId(u), d, l, ts); }
// Bridge pacing: a real host polls about every 2.2 ms of (the client's) time and the client's software needs that gap
// to arm its next reply. So a poll (CMD) is only handed to the client once BRIDGE_GAP_US of emulated time has passed
// since the previous one; until then the client is told "nothing yet" and keeps running instead of blocking.
static u64 g_last_cmd_ts = 0;
static bool g_want_sent = false;
static u64 g_gap_us = 2000;
static bool IsCmd(const std::vector<u8>& p) { static const u8 m[6] = {3, 9, 0xBF, 0, 0, 0}; return p.size() >= 22 && memcmp(&p[16], m, 6) == 0; }
static int BridgeRecv(u8* d, u64* ts, void* u, int wait_ms)
{
    u64 now = BridgeNow(u);
    bool cmd_ok = now >= g_last_cmd_ts + g_gap_us;
    std::unique_lock<std::mutex> lk(g_bridge->m);
    if (!g_bridge->q.empty() && IsCmd(g_bridge->q.front()) && !cmd_ok) return 0;      // too early for the next poll
    if (g_bridge->q.empty() && wait_ms > 0)
    {
        if (!cmd_ok) return 0;                       // not yet time for a poll: let the emulator run on
        if (!g_want_sent) { g_want_sent = true; lk.unlock(); BridgeSend(9, 0, now, nullptr, 0); lk.lock(); }
        g_bridge->cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [] { return !g_bridge->q.empty(); });
    }
    if (g_bridge->q.empty()) return 0;
    auto pkt = std::move(g_bridge->q.front()); g_bridge->q.pop_front();
    if (IsCmd(pkt)) { g_last_cmd_ts = now; g_want_sent = false; }
    lk.unlock();
    memcpy(d, pkt.data(), pkt.size());
    if (ts) *ts = now;
    return (int)pkt.size();
}
int MP_RecvPacket(u8* d, u64* ts, void* u) { if (g_bridge) return BridgeRecv(d, ts, u, 0); return g_mp->RecvPacket(InstId(u), d, ts); }
int MP_SendCmd(u8* d, int l, u64 ts, void* u) { PcapWrite(InstId(u), "cmd", d, l, ts); if (g_bridge) return l; return g_mp->SendCmd(InstId(u), d, l, ts); }
int MP_SendReply(u8* d, int l, u64 ts, u16 aid, void* u) { if (d) PcapWrite(InstId(u), "reply", d, l, ts); if (g_bridge) { BridgeSend(2, aid, ts, d, d ? l : 0); return l; } return g_mp->SendReply(InstId(u), d, l, ts, aid); }
int MP_SendAck(u8* d, int l, u64 ts, void* u) { PcapWrite(InstId(u), "ack", d, l, ts); if (g_bridge) return l; return g_mp->SendAck(InstId(u), d, l, ts); }
int MP_RecvHostPacket(u8* d, u64* ts, void* u) { if (g_bridge) return BridgeRecv(d, ts, u, 25); return g_mp->RecvHostPacket(InstId(u), d, ts); }
u16 MP_RecvReplies(u8* d, u64 ts, u16 m, void* u) { if (g_bridge) return 0; return g_mp->RecvReplies(InstId(u), d, ts, m); }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }
void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}
void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16* d, int n, void*) { memset(d, 0, n * sizeof(s16)); return n; }
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }
bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0; }
DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }
}

// ---------------------------------------------------------------- instances
struct Inst
{
    int id = 0;
    std::atomic<bool> reset_req{false};
    std::mutex path_mutex;
    std::vector<std::pair<int,int>> path;
    size_t path_idx = 0;
    std::unique_ptr<NDS> nds;
    std::thread th;
    std::atomic<u64> frames{0};
    std::atomic<int> touch_x{-1}, touch_y{-1}, touch_left{0};
    std::atomic<u32> keys{0};
    std::atomic<int> keys_left{0};
    std::mutex fb_mutex;
    std::vector<u32> top = std::vector<u32>(256 * 192), bottom = std::vector<u32>(256 * 192);
};
static std::vector<std::unique_ptr<Inst>> g_inst;
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_fast{true};
static std::atomic<int> g_frame_us{16667};   // pacing: real microseconds per emulated frame

namespace melonDS::Platform
{
static u64 BridgeNow(void* u) { return ((::Inst*)u)->nds->Wifi.USTimestamp; }
int InstId(void* u) { return ((::Inst*)u)->id; }
// persist the firmware (user settings) per instance so the setup wizard only has to be done once
void WriteFirmware(const Firmware& fw, u32, u32, void* u)
{
    char name[64]; snprintf(name, sizeof name, "fw_inst%d.bin", ((::Inst*)u)->id);
    FILE* f = fopen(name, "wb");
    if (f) { fwrite(fw.Buffer(), 1, fw.Length(), f); fclose(f); }
}
}

static void BridgeServer(int port)
{
    using namespace melonDS::Platform;
    g_bridge->lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(g_bridge->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_bridge->lfd, (sockaddr*)&a, sizeof a) != 0 || listen(g_bridge->lfd, 1) != 0) { perror("bridge bind"); return; }
    for (;;)
    {
        int c = accept(g_bridge->lfd, nullptr, nullptr);
        if (c < 0) continue;
        g_bridge->cfd = c;
        fprintf(stderr, "bridge: host connected\n");
        std::vector<u8> buf;
        u8 tmp[8192];
        for (;;)
        {
            ssize_t n = recv(c, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            buf.insert(buf.end(), tmp, tmp + n);
            while (buf.size() >= 5)
            {
                u32 len; memcpy(&len, buf.data(), 4);
                if (buf.size() < 4 + len) break;
                // payload: kind(1) + 12-byte header + frame (+FCS)
                std::vector<u8> pkt(buf.begin() + 5, buf.begin() + 4 + len);
                buf.erase(buf.begin(), buf.begin() + 4 + len);
                if (pkt.size() >= 12 + 24) { PcapWrite(0, "host", pkt.data(), (int)pkt.size(), g_inst.empty() ? 0 : g_inst[0]->nds->Wifi.USTimestamp); }
                std::lock_guard<std::mutex> g(g_bridge->m);
                g_bridge->q.push_back(std::move(pkt));
                g_bridge->cv.notify_all();
            }
        }
        g_bridge->cfd = -1; close(c);
        fprintf(stderr, "bridge: host disconnected\n");
    }
}

static std::vector<u8> ReadFile(const char* p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void EmuLoop(Inst* in)
{
    auto next = Clock::now();
    while (!g_quit)
    {
        if (in->reset_req.exchange(false)) { in->nds->Reset(); in->nds->Start(); }
        bool dragging = false; int dx = 0, dy = 0;
        { std::lock_guard<std::mutex> g(in->path_mutex); if (in->path_idx < in->path.size()) { dx = in->path[in->path_idx].first; dy = in->path[in->path_idx].second; in->path_idx++; dragging = true; } }
        if (dragging) in->nds->TouchScreen(dx, dy);
        else if (in->touch_left > 0) { in->nds->TouchScreen(in->touch_x, in->touch_y); in->touch_left--; }
        else in->nds->ReleaseScreen();
        u32 pressed = 0;
        if (in->keys_left > 0) { pressed = in->keys; in->keys_left--; }
        in->nds->SetKeyMask(0xFFF & ~pressed);

        in->nds->RunFrame();
        in->frames++;
        if (in->frames % 120 == 0)
        {
            RTC::StateData st; in->nds->RTC.GetState(st);
            char name[64]; snprintf(name, sizeof name, "rtc_inst%d.bin", in->id);
            FILE* rf = fopen(name, "wb"); if (rf) { fwrite(&st, sizeof st, 1, rf); fclose(rf); }
        }

        void *t = nullptr, *b = nullptr;
        if (in->nds->GPU.GetFramebuffers(&t, &b) && t && b)
        {
            std::lock_guard<std::mutex> g(in->fb_mutex);
            memcpy(in->top.data(), t, 256 * 192 * 4);
            memcpy(in->bottom.data(), b, 256 * 192 * 4);
        }
        if (!g_fast)
        {
            next += std::chrono::microseconds(g_frame_us.load());
            if (next < Clock::now() - std::chrono::milliseconds(50)) next = Clock::now();   // no catch-up bursts
            std::this_thread::sleep_until(next);
        }
        else next = Clock::now();
    }
}

static bool SaveShot(Inst* in, const char* file)
{
    std::lock_guard<std::mutex> g(in->fb_mutex);
    std::string part = std::string(file) + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n256 384\n255\n");
    for (int half = 0; half < 2; half++)
    {
        const auto& v = half == 0 ? in->top : in->bottom;
        for (u32 px : v)
        {
            u8 rgb[3] = {(u8)(px >> 16), (u8)(px >> 8), (u8)px};   // 0xAARRGGBB
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return rename(part.c_str(), file) == 0;
}

int main(int argc, char** argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s bios7 bios9 firmware.bin out.pcap [instances=2]\n", argv[0]); return 1; }
    int n = argc > 5 ? atoi(argv[5]) : 2;
    auto b7 = ReadFile(argv[1]), b9 = ReadFile(argv[2]), fw = ReadFile(argv[3]);
    if (b7.size() != ARM7BIOSSize || b9.size() != ARM9BIOSSize || fw.size() < 0x20000) { fprintf(stderr, "bad BIOS/firmware sizes %zu %zu %zu\n", b7.size(), b9.size(), fw.size()); return 1; }
    Platform::g_pcap = fopen(argv[4], "wb");
    if (Platform::g_pcap) { u32 gh[6] = {0xA1B2C3D4, 0x00040002, 0, 0, 65535, 127}; u16 ver[2] = {2, 4}; fwrite(&gh[0], 4, 1, Platform::g_pcap); fwrite(ver, 2, 2, Platform::g_pcap); u32 z[4] = {0, 0, 65535, 127}; fwrite(z, 4, 4, Platform::g_pcap); }
    int bridge_port = argc > 6 ? atoi(argv[6]) : 0;
    if (bridge_port) { n = 1; Platform::g_bridge = new Platform::Bridge(); }
    if (getenv("BRIDGE_GAP_US")) Platform::g_gap_us = strtoull(getenv("BRIDGE_GAP_US"), nullptr, 10);
    else Platform::g_mp = new LocalMP();
    const char* names[] = {"Aaa", "Bbb", "Ccc", "Ddd"};

    for (int i = 0; i < n; i++)
    {
        auto in = std::make_unique<Inst>();
        in->id = i;
        char saved[64]; snprintf(saved, sizeof saved, "fw_inst%d.bin", i);
        auto fwdata = ReadFile(saved);
        if (fwdata.size() < 0x20000) fwdata = fw;
        Firmware firmware(fwdata.data(), (u32)fwdata.size());
        auto& hdr = firmware.GetHeader();
        hdr.MacAddr = {0x00, 0x09, 0xBF, 0x11, 0x22, (u8)(0x33 + i)};
        firmware.UpdateChecksums();

        NDSArgs args;
        memcpy(args.ARM7BIOS->data(), b7.data(), b7.size());
        memcpy(args.ARM9BIOS->data(), b9.data(), b9.size());
        args.Firmware = std::move(firmware);
        if (getenv("NOJIT")) args.JIT = std::nullopt; else args.JIT = JITArgs();
        in->nds = std::make_unique<NDS>(std::move(args), in.get());
        in->nds->GPU.SetRenderer(std::make_unique<SoftRenderer>(*in->nds));
        {
            char rn[64]; snprintf(rn, sizeof rn, "rtc_inst%d.bin", i);
            RTC::StateData st; FILE* rf = fopen(rn, "rb");
            if (rf && fread(&st, sizeof st, 1, rf) == 1) in->nds->RTC.SetState(st);
            else in->nds->RTC.SetDateTime(2026, 11, 7, 12, 0, 0);
            if (rf) fclose(rf);
        }
        in->nds->Reset();
        in->nds->Start();
        g_inst.push_back(std::move(in));
    }
    if (bridge_port) std::thread(BridgeServer, bridge_port).detach();
    for (auto& in : g_inst) in->th = std::thread(EmuLoop, in.get());
    fprintf(stderr, "harness: %d instances running, commands on stdin\n", n);

    std::string line;
    while (std::getline(std::cin, line))
    {
        std::istringstream ss(line);
        std::string c; ss >> c;
        {   // bounds-check the instance index of commands that take one
            std::istringstream probe(line); std::string c2; int idx = 0;
            probe >> c2;
            if ((c2 == "touch" || c2 == "key" || c2 == "shot" || c2 == "reset" || c2 == "drag") && (probe >> idx) && (idx < 0 || idx >= (int)g_inst.size())) { fprintf(stderr, "bad instance %d\n", idx); continue; }
        }
        if (c == "quit") break;
        else if (c == "fast") { std::string v; ss >> v; g_fast = (v == "on"); }
        else if (c == "fps") { double f; ss >> f; if (f > 0.5) { g_frame_us = (int)(1e6 / f); g_fast = false; } }
        else if (c == "touch") { int i, x, y, f = 6; ss >> i >> x >> y; if (!(ss >> f)) f = 6; g_inst[i]->touch_x = x; g_inst[i]->touch_y = y; g_inst[i]->touch_left = f; }
        else if (c == "key") { int i; u32 m; int f; ss >> i >> m >> f; g_inst[i]->keys = m; g_inst[i]->keys_left = f; }
        else if (c == "shot") { int i; std::string file; ss >> i >> file; printf("%s\n", SaveShot(g_inst[i].get(), file.c_str()) ? "ok" : "fail"); fflush(stdout); }
        else if (c == "frames") { for (auto& in : g_inst) printf("inst %d frames %llu\n", in->id, (unsigned long long)in->frames.load()); fflush(stdout); }
        else if (c == "drag") {   // drag I x1 y1 x2 y2 frames : stylus moves linearly, then lifts
            int i, x1, y1, x2, y2, f; ss >> i >> x1 >> y1 >> x2 >> y2 >> f; if (f < 2) f = 2;
            std::lock_guard<std::mutex> g(g_inst[i]->path_mutex);
            g_inst[i]->path.clear(); g_inst[i]->path_idx = 0;
            for (int k = 0; k < f; k++) g_inst[i]->path.push_back({x1 + (x2 - x1) * k / (f - 1), y1 + (y2 - y1) * k / (f - 1)});
        }
        else if (c == "regs") {   // regs I : ARM9 and ARM7 registers
            int i; ss >> i; auto& n = *g_inst[i]->nds;
            printf("ARM9 PC=%08X CPSR=%08X\n", n.ARM9.R[15], n.ARM9.CPSR);
            for (int k = 0; k < 15; k++) printf("  R%-2d=%08X%s", k, n.ARM9.R[k], (k % 4 == 3) ? "\n" : "");
            printf("\nARM7 PC=%08X CPSR=%08X\n", n.ARM7.R[15], n.ARM7.CPSR);
            for (int k = 0; k < 15; k++) printf("  R%-2d=%08X%s", k, n.ARM7.R[k], (k % 4 == 3) ? "\n" : "");
            printf("\n"); fflush(stdout);
        }
        else if (c == "peek9") {   // peek9 I ADDR N : read N words from ARM9 memory
            int i; std::string a; int cnt; ss >> i >> a >> cnt; u32 ad = strtoul(a.c_str(), nullptr, 16);
            for (int k = 0; k < cnt; k++) printf("%08X: %08X\n", ad + 4 * k, g_inst[i]->nds->ARM9Read32(ad + 4 * k));
            fflush(stdout);
        }
        else if (c == "reset") { int i; ss >> i; g_inst[i]->reset_req = true; }
        else if (c == "verbose") { std::string v; ss >> v; Platform::g_verbose = (v == "on"); }
        else if (c == "sleep") { int ms; ss >> ms; std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    }
    g_quit = true;
    for (auto& in : g_inst) if (in->th.joinable()) in->th.join();
    return 0;
}
