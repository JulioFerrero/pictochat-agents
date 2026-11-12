// Headless single-DS ROM runner around the melonDS core (GPLv3 core, linked as libcore.a), for testing homebrew
// (e.g. the DS Calendar, a separate repo) without a window. Boots the .nds directly (real BIOS/firmware from fw/), optionally with
// an emulated DLDI SD card synced to a folder, then runs a deterministic script read from stdin:
//   run N                 run N frames
//   key MASK N            hold keys for N frames (bit0 A,1 B,2 Select,3 Start,4 Right,5 Left,6 Up,7 Down,8 R,9 L,10 X,11 Y)
//   touch X Y N           hold the stylus at X,Y (bottom screen) for N frames
//   shot FILE.ppm         write both screens (256x384, top above bottom)
//   quit                  (also at EOF) - flushes the SD folder
// usage: ./romrun ROM.nds [SD_FOLDER] < script     (env RTC="2026 11 8 12 0 0" sets the clock)
#include <chrono>
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
#include <time.h>

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "Platform.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "SPI_Firmware.h"
#include "RTC.h"

using namespace melonDS;
using Clock = std::chrono::steady_clock;

namespace melonDS::Platform
{
struct FileHandle { FILE* f; };
struct Thread { std::thread t; };
struct Semaphore { sem_t s; };
struct Mutex { std::mutex m; };
struct AACDecoder {};
struct DynamicLibrary {};
static Clock::time_point g_t0 = Clock::now();

void SignalStop(StopReason, void*) {}
std::string GetLocalFilePath(const std::string& f) { return f; }
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    const char* m = (mode & FileMode::Write) ? ((mode & FileMode::Read) ? ((mode & FileMode::Preserve) ? "r+b" : "w+b") : "wb") : "rb";
    FILE* f = fopen(path.c_str(), m);
    if (!f && (mode & FileMode::Preserve) && (mode & FileMode::Write)) f = fopen(path.c_str(), "w+b");
    return f ? new FileHandle{f} : nullptr;
}
FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }
bool FileExists(const std::string& n) { FILE* f = fopen(n.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }
bool LocalFileExists(const std::string& n) { return FileExists(n); }
bool CheckFileWritable(const std::string&) { return true; }
bool CheckLocalFileWritable(const std::string&) { return true; }
bool CloseFile(FileHandle* h) { if (!h) return false; fclose(h->f); delete h; return true; }
bool IsEndOfFile(FileHandle* h) { return feof(h->f); }
bool FileReadLine(char* s, int n, FileHandle* h) { return fgets(s, n, h->f) != nullptr; }
u64 FilePosition(FileHandle* h) { return ftell(h->f); }
bool FileSeek(FileHandle* h, s64 off, FileSeekOrigin o) { return fseeko(h->f, off, o == FileSeekOrigin::Start ? SEEK_SET : o == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END) == 0; }
void FileRewind(FileHandle* h) { rewind(h->f); }
u64 FileRead(void* d, u64 s, u64 c, FileHandle* h) { return fread(d, s, c, h->f); }
bool FileFlush(FileHandle* h) { return fflush(h->f) == 0; }
u64 FileWrite(const void* d, u64 s, u64 c, FileHandle* h) { return fwrite(d, s, c, h->f); }
u64 FileWriteFormatted(FileHandle* h, const char* fmt, ...) { va_list a; va_start(a, fmt); int r = vfprintf(h->f, fmt, a); va_end(a); return r; }
u64 FileLength(FileHandle* h) { long p = ftell(h->f); fseek(h->f, 0, SEEK_END); long l = ftell(h->f); fseek(h->f, p, SEEK_SET); return l; }
void Log(LogLevel level, const char* fmt, ...)
{
    if (level < LogLevel::Warn && !getenv("VERBOSE")) return;
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
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}
void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int l, u64, void*) { return l; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int l, u64, void*) { return l; }
int MP_SendReply(u8*, int l, u64, u16, void*) { return l; }
int MP_SendAck(u8*, int l, u64, void*) { return l; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }
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

static std::vector<u8> ReadFile(const char* p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool SaveShot(NDS& nds, const std::string& file)
{
    void *t = nullptr, *b = nullptr;
    if (!nds.GPU.GetFramebuffers(&t, &b) || !t || !b) return false;
    FILE* f = fopen(file.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n256 384\n255\n");
    for (const u32* src : {(const u32*)t, (const u32*)b})
        for (int i = 0; i < 256 * 192; i++)
        {
            u8 rgb[3] = {(u8)(src[i] >> 16), (u8)(src[i] >> 8), (u8)src[i]};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s ROM.nds [SD_FOLDER] < script\n", argv[0]); return 1; }
    auto b7 = ReadFile("fw/bios7.bin"), b9 = ReadFile("fw/bios9.bin"), fw = ReadFile("fw/firmware.bin");
    if (b7.size() != ARM7BIOSSize || b9.size() != ARM9BIOSSize || fw.size() < 0x20000)
    { fprintf(stderr, "need fw/bios7.bin, fw/bios9.bin, fw/firmware.bin (run ./build.sh)\n"); return 1; }
    auto rom = ReadFile(argv[1]);
    if (rom.size() < 0x200) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    NDSArgs args;
    memcpy(args.ARM7BIOS->data(), b7.data(), b7.size());
    memcpy(args.ARM9BIOS->data(), b9.data(), b9.size());
    args.Firmware = Firmware(fw.data(), (u32)fw.size());
    args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);
    nds->GPU.SetRenderer(std::make_unique<SoftRenderer>(*nds));

    NDSCart::NDSCartArgs cargs;
    if (argc > 2)
    {
        std::string img = std::string(argv[2]) + ".img";
        remove(img.c_str());    // rebuilt from the folder each run
        cargs.SDCard = FATStorageArgs{img, 64ull << 20, false, std::string(argv[2])};
    }
    auto data = std::make_unique<u8[]>(rom.size());
    memcpy(data.get(), rom.data(), rom.size());
    auto cart = NDSCart::ParseROM(std::move(data), (u32)rom.size(), nullptr, std::move(cargs));
    if (!cart) { fprintf(stderr, "ParseROM failed\n"); return 1; }
    nds->SetNDSCart(std::move(cart));

    int Y = 2026, Mo = 11, D = 8, h = 12, mi = 0, s = 0;
    if (getenv("RTC")) sscanf(getenv("RTC"), "%d %d %d %d %d %d", &Y, &Mo, &D, &h, &mi, &s);
    nds->RTC.SetDateTime(Y, Mo, D, h, mi, s);
    nds->Reset();
    nds->SetupDirectBoot(argv[1]);
    nds->Start();

    auto frames = [&](int n, u32 keys, int tx, int ty) {
        for (int i = 0; i < n; i++)
        {
            if (tx >= 0) nds->TouchScreen(tx, ty); else nds->ReleaseScreen();
            nds->SetKeyMask(0xFFF & ~keys);
            nds->RunFrame();
        }
    };
    std::string line;
    long total = 0;
    while (std::getline(std::cin, line))
    {
        std::istringstream ss(line);
        std::string c; ss >> c;
        if (c.empty() || c[0] == '#') continue;
        if (c == "run") { int n = 1; ss >> n; frames(n, 0, -1, -1); total += n; }
        else if (c == "key") { u32 m = 0; int n = 6; ss >> m >> n; frames(n, m, -1, -1); frames(2, 0, -1, -1); total += n + 2; }
        else if (c == "touch") { int x, y, n = 6; ss >> x >> y >> n; frames(n, 0, x, y); frames(2, 0, -1, -1); total += n + 2; }
        else if (c == "shot") { std::string f; ss >> f; fprintf(stderr, "frame %ld: %s %s\n", total, f.c_str(), SaveShot(*nds, f) ? "ok" : "FAILED"); }
        else if (c == "quit") break;
        else fprintf(stderr, "unknown command: %s\n", c.c_str());
    }
    nds->Stop();
    nds.reset();     // destroys the cart -> FATStorage writes the SD image back to the folder
    return 0;
}
