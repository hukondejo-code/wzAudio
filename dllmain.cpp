#include "pch.h"
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <x3daudio.h>
#include <map>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "xaudio2.lib")

namespace fs = std::filesystem;



// ---------------------------------------------------------------------------
//  LOGOLÁS (DEBUG) SEGÉDFÜGGVÉNYEK
// ---------------------------------------------------------------------------

#include <fstream>
#include <mutex>

std::mutex g_LogMutex;
std::ofstream g_LogFile;

void InitLogger()
{
    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);

    std::string filePath = std::string(tempPath) + "wzAudio.log";
    g_LogFile.open(filePath, std::ios::out | std::ios::app);
}

void LogDebug(const char* msg)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogFile.is_open())
    {
        g_LogFile << msg << "\n";
        g_LogFile.flush();
    }
}

void LogWithTimestamp(const char* msg)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[512];
    sprintf_s(line,
        "[%02d:%02d:%02d.%03d] %s",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg);

    LogDebug(line);
}

// ---------------------------------------------------------------------------
//  IMPORT ADDRESS TABLE (IAT) PATCHING
// ---------------------------------------------------------------------------

void PatchImportAddressTable(const char* dllName, const char* functionName, DWORD newFunctionAddress)
{
    HMODULE hModule = GetModuleHandleA(NULL);
    if (!hModule) return;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;

    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hModule + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;

    PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hModule +
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    if (!importDesc || (BYTE*)importDesc == (BYTE*)hModule) return;

    while (importDesc->Name) {
        const char* name = (const char*)((BYTE*)hModule + importDesc->Name);

        if (_stricmp(name, dllName) == 0) {
            PIMAGE_THUNK_DATA thunkIAT = (PIMAGE_THUNK_DATA)((BYTE*)hModule + importDesc->FirstThunk);
            PIMAGE_THUNK_DATA thunkOriginal = (PIMAGE_THUNK_DATA)((BYTE*)hModule + importDesc->Characteristics);

            while (thunkIAT->u1.Function) {
                if (thunkOriginal && !(thunkOriginal->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    PIMAGE_IMPORT_BY_NAME importName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hModule + thunkOriginal->u1.AddressOfData);

                    if (strcmp((const char*)importName->Name, functionName) == 0) {
                        DWORD oldProtect;
                        VirtualProtect(&thunkIAT->u1.Function, sizeof(DWORD), PAGE_READWRITE, &oldProtect);
                        thunkIAT->u1.Function = newFunctionAddress;
                        VirtualProtect(&thunkIAT->u1.Function, sizeof(DWORD), oldProtect, &oldProtect);
                        return;
                    }
                }
                thunkIAT++;
                if (thunkOriginal) thunkOriginal++;
            }
        }
        importDesc++;
    }
}

// ---------------------------------------------------------------------------
//  GLOBÁLIS VÁLTOZÓK (MCI ZENE + XAUDIO EFFECT)
// ---------------------------------------------------------------------------

std::string g_CurrentTrack = "";
std::string g_RequestedTrack = "";
HWND g_HwndMCI = NULL;

IXAudio2* g_xaudio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
X3DAUDIO_HANDLE g_x3d = { 0 };
bool g_audio_ready = false;

uintptr_t OBJECT_LIST_BASE = 0x07B5258C;
const size_t OBJECT_STRUCT_SIZE = 0x538;
uintptr_t PLAYER_INDEX_ADDR = 0x0091F830;

// Eredeti DirectSoundCreate függvény mentése a hookhoz
typedef HRESULT(__stdcall* DirectSoundCreate_t)(LPGUID, void**, void*);
DirectSoundCreate_t TrueDirectSoundCreate = nullptr;

// DirectSoundCreate8 mentése
typedef HRESULT(__stdcall* DirectSoundCreate8_t)(LPGUID, void**, void*);
DirectSoundCreate8_t TrueDirectSoundCreate8 = nullptr;

// ---------------------------------------------------------------------------
//  BIZTONSÁGOS MEMÓRIA-ELLENŐRZÉS (VirtualQuery ALAPÚ)
// ---------------------------------------------------------------------------

bool IsReadable(void* ptr, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    if (mbi.Protect & PAGE_NOACCESS)
        return false;

    if (mbi.Protect & PAGE_GUARD)
        return false;

    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

    if (end > regionEnd)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
//  STABIL REFERENCIAPONT (PLAYER KÖZELI MOB) LEKÉRDEZÉSE
// ---------------------------------------------------------------------------

bool GetReferencePosition(short& outX, short& outY)
{
    if (!IsReadable((void*)OBJECT_LIST_BASE, sizeof(uintptr_t)))
        return false;

    uintptr_t listPtr = *(uintptr_t*)OBJECT_LIST_BASE;
    if (!IsReadable((void*)listPtr, OBJECT_STRUCT_SIZE))
        return false;

    // idx=1 – az első érvényes mob / NPC, jellemzően a player közelében
    uintptr_t entry = listPtr + (1 * OBJECT_STRUCT_SIZE);

    if (!IsReadable((void*)(entry + 0xAC), sizeof(short)))
        return false;

    if (!IsReadable((void*)(entry + 0xB0), sizeof(short)))
        return false;

    outX = *(short*)(entry + 0xAC);
    outY = *(short*)(entry + 0xB0);

    char dbg[128];
    sprintf_s(dbg, "[PLAYERREF] X=%d Y=%d", outX, outY);
    LogWithTimestamp(dbg);

    return true;
}

// Maximális egyidejű hangeffektek száma
const int MAX_VOICE_POOL = 64;

struct Active3DSound {
    IXAudio2SourceVoice* pSourceVoice;
    int targetMobIndex;  // Melyik slotban ül a szörny (0-400)
    bool bInUse;
};

Active3DSound g_VoicePool[MAX_VOICE_POOL] = { 0 };
std::mutex g_VoicePoolMutex;

// ---------------------------------------------------------------------------
//  ÚJ FUNKCIÓ: 3D HANG PROFI INDÍTÁSA AZ XAUDIO2 MOTORBAN
// ---------------------------------------------------------------------------
void PlayXAudio3DEffect(const BYTE* pWaveData, DWORD dataSize, WAVEFORMATEX* pWfx, int mobIndex)
{
    if (!g_audio_ready || !pWaveData || !pWfx) return;

    std::lock_guard<std::mutex> lock(g_VoicePoolMutex);

    // Keresünk egy szabad slotot a pool-ban
    int freeSlot = -1;
    for (int i = 0; i < MAX_VOICE_POOL; i++) {
        if (!g_VoicePool[i].bInUse) {
            freeSlot = i;
            break;
        }
        else {
            // Ha már fut, ellenőrizzük, hogy lejárt-e
            XAUDIO2_VOICE_STATE state;
            g_VoicePool[i].pSourceVoice->GetState(&state);
            if (state.SamplesPlayed == 0 && state.BuffersQueued == 0) {
                g_VoicePool[i].pSourceVoice->DestroyVoice();
                g_VoicePool[i].bInUse = false;
                freeSlot = i;
                break;
            }
        }
    }

    // Ha nincs szabad csatorna, eldobjuk a hangot (túlcsordulás védelem)
    if (freeSlot == -1) return;

    // Létrehozzuk a forrás hangcsatornát (az ffmpeg miatt ez fixen 1 csatornás MONO)
    IXAudio2SourceVoice* pSourceVoice = nullptr;
    if (SUCCEEDED(g_xaudio->CreateSourceVoice(&pSourceVoice, pWfx))) {
        XAUDIO2_BUFFER buffer = { 0 };
        buffer.AudioBytes = dataSize;
        buffer.pAudioData = pWaveData;
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        if (SUCCEEDED(pSourceVoice->SubmitSourceBuffer(&buffer))) {
            pSourceVoice->Start();

            g_VoicePool[freeSlot].pSourceVoice = pSourceVoice;
            g_VoicePool[freeSlot].targetMobIndex = mobIndex;
            g_VoicePool[freeSlot].bInUse = true;
        }
        else {
            pSourceVoice->DestroyVoice();
        }
    }
}

// ---------------------------------------------------------------------------
//  FINOMHANGOLT VATCHER THREAD (v3.1.6 - TELJES PANNING JAVÍTÁS)
// ---------------------------------------------------------------------------
DWORD WINAPI AudioWatcherThread(LPVOID lpParam)
{
    // Bevárjuk a stabil betöltést (kihagyva a 0,0-ás kezdeti állapotot)
    while (true) {
        short tx = 0, ty = 0;
        if (g_audio_ready && GetReferencePosition(tx, ty)) {
            if (tx != 0 && ty != 0) break;
        }
        Sleep(100);
    }

    while (g_audio_ready)
    {
        Sleep(16); // ~60 FPS frissítés a sima hangátmenetekért

        short refX = 0, refY = 0;
        if (!GetReferencePosition(refX, refY)) continue;
        if (refX == 0 && refY == 0) continue; // Biztonsági fék térképváltáskor

        uintptr_t listPtr = *(uintptr_t*)OBJECT_LIST_BASE;
        if (!IsReadable((void*)listPtr, OBJECT_STRUCT_SIZE * 400)) continue;

        std::lock_guard<std::mutex> lock(g_VoicePoolMutex);

        for (int i = 0; i < MAX_VOICE_POOL; i++)
        {
            if (!g_VoicePool[i].bInUse) continue;

            XAUDIO2_VOICE_STATE state;
            g_VoicePool[i].pSourceVoice->GetState(&state);

            // Ha a hang megállt, felszabadítjuk a slotot
            if (state.SamplesPlayed == 0 && state.BuffersQueued == 0) {
                g_VoicePool[i].pSourceVoice->DestroyVoice();
                g_VoicePool[i].bInUse = false;
                continue;
            }

            int mobIdx = g_VoicePool[i].targetMobIndex;
            uintptr_t entry = listPtr + (mobIdx * OBJECT_STRUCT_SIZE);

            if (IsReadable((void*)entry, OBJECT_STRUCT_SIZE))
            {
                short mobX = *(short*)(entry + 0xAC);
                short mobY = *(short*)(entry + 0xB0);

                // Ha a szörny időközben meghalt vagy eltűnt (0,0 lett), elnémítjuk
                if (mobX == 0 && mobY == 0) {
                    g_VoicePool[i].pSourceVoice->SetVolume(0.0f);
                    continue;
                }

                // --- MATEMATIKAI TÉRMODELLEZÉS ---
                X3DAUDIO_LISTENER listener = {};
                listener.Position.x = (float)refX;
                listener.Position.y = 0.0f;
                listener.Position.z = (float)refY;
                listener.OrientFront.z = 1.0f; // Szigorúan normalizált vektorok
                listener.OrientTop.y = 1.0f;

                X3DAUDIO_EMITTER emitter = {};
                emitter.Position.x = (float)mobX;
                emitter.Position.y = 0.0f;
                emitter.Position.z = (float)mobY;
                emitter.OrientFront.z = 1.0f;
                emitter.OrientTop.y = 1.0f;
                emitter.ChannelCount = 1;         // MONO forrás az ffmpeg konverzió miatt!
                emitter.CurveDistanceScaler = 0.05f; // Érzékenységi együttható a MU-hoz

                X3DAUDIO_DSP_SETTINGS dsp = {};
                float matrixCoefficients[2] = { 1.0f, 1.0f }; // Bal és Jobb puffer
                dsp.SrcChannelCount = 1; // Mono bemenet
                dsp.DstChannelCount = 2; // Sztereó kimenet
                dsp.pMatrixCoefficients = matrixCoefficients;

                // Kikényszerítjük a mátrix (Pan) és a hangerő (Volume) kalkulációt
                X3DAudioCalculate(g_x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);

                // Élesítjük a hardveren a csatorna mátrixot
                g_VoicePool[i].pSourceVoice->SetOutputMatrix(g_master, 1, 2, dsp.pMatrixCoefficients);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  EREDETI WZAUDIO 2.0 ZENEI FÜGGVÉNYEK
// ---------------------------------------------------------------------------

int GetMusicVolumeFromRegistry() {
    HKEY hKey; DWORD musicVolume = 9; DWORD dataSize = sizeof(musicVolume);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "VolumeLevel", NULL, NULL, (LPBYTE)&musicVolume, &dataSize); RegCloseKey(hKey);
    }
    int mciVolume = musicVolume * 111;
    if (mciVolume > 1000) mciVolume = 1000; if (mciVolume < 0) mciVolume = 0;
    return mciVolume;
}

void ApplyMCIVolume() {
    if (g_CurrentTrack == "") return;
    int volume = GetMusicVolumeFromRegistry();
    std::string volCmd = "setaudio my_mp3 volume to " + std::to_string(volume);
    mciSendStringA(volCmd.c_str(), NULL, 0, NULL);
}

std::string GetRandomMp3FromFolderIfNeeded(const std::string& originalPath, const std::string& lastPlayedTrack = "") {
    std::string folder_name = originalPath;
    size_t last_dot = folder_name.find_last_of(".");
    if (last_dot != std::string::npos) folder_name = folder_name.substr(0, last_dot);
    if (fs::exists(folder_name) && fs::is_directory(folder_name)) {
        std::vector<std::string> mp3_list;
        for (const auto& entry : fs::directory_iterator(folder_name)) {
            if (entry.path().extension() == ".mp3") mp3_list.push_back(entry.path().string());
        }
        if (!mp3_list.empty()) {
            if (mp3_list.size() == 1) return mp3_list[0];
            std::random_device rd; std::mt19937 gen(rd()); std::uniform_int_distribution<> distr(0, mp3_list.size() - 1);
            std::string selected;
            do { selected = mp3_list[distr(gen)]; } while (selected == lastPlayedTrack);
            return selected;
        }
    }
    return originalPath;
}

void PlayNextRotatedTrack() {
    mciSendStringA("close my_mp3", NULL, 0, NULL);
    g_CurrentTrack = GetRandomMp3FromFolderIfNeeded(g_RequestedTrack, g_CurrentTrack);
    std::string openCmd = "open \"" + g_CurrentTrack + "\" type mpegvideo alias my_mp3";
    if (mciSendStringA(openCmd.c_str(), NULL, 0, NULL) == 0) {
        std::string playCmd = "play my_mp3 notify";
        mciSendStringA(playCmd.c_str(), NULL, 0, (HWND)g_HwndMCI);
        ApplyMCIVolume();
    }
}

LRESULT CALLBACK MCIWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == MM_MCINOTIFY && wParam == MCI_NOTIFY_SUCCESSFUL) { PlayNextRotatedTrack(); return 0; }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void CreateMCIHelperWindow() {
    if (g_HwndMCI != NULL) return;
    WNDCLASSEXA wc = { 0 }; wc.cbSize = sizeof(WNDCLASSEXA); wc.lpfnWndProc = MCIWindowProc;
    wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "MuWzAudioMCIHelper";
    RegisterClassExA(&wc);
    g_HwndMCI = CreateWindowExA(0, wc.lpszClassName, "MCI Helper", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
}

// ---------------------------------------------------------------------------
//  WEBZEN VTABLE INTERFÉSZ ÉS IMPLEMENTÁCIÓ
// ---------------------------------------------------------------------------

class IWzAudio {
public:
    virtual int  __thiscall GetStreamOffsetRange(int unk1, int unk2) = 0;
    virtual void __thiscall Play(const char* filePath, int volume, int unknown) = 0;
    virtual void __thiscall Stop() = 0;
    virtual void __thiscall SetVolume(int volume) = 0;
    virtual void __thiscall Option(int option, int value) = 0;
    virtual void __thiscall Destroy() = 0;
};

#ifndef DS_OK
#define DS_OK 0
#endif

class CWzAudioImpl : public IWzAudio {
public:
    int __thiscall GetStreamOffsetRange(int unk1, int unk2) override { return DS_OK; }

    void __thiscall Play(const char* filePath, int volume, int unknown) override {
        if (!filePath) return;

        HKEY hKey; DWORD musicOnOff = 1; DWORD dataSize = sizeof(musicOnOff);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "MusicOn", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize) != ERROR_SUCCESS) {
                RegQueryValueExA(hKey, "MusicOnOff", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize);
            }
            RegCloseKey(hKey);
        }

        if (musicOnOff == 0) { Stop(); return; }
        if (g_RequestedTrack == filePath) { ApplyMCIVolume(); return; }

        g_RequestedTrack = filePath; g_CurrentTrack = "";
        PlayNextRotatedTrack();
    }

    void __thiscall Stop() override {
        mciSendStringA("close my_mp3", NULL, 0, NULL);
        g_CurrentTrack = ""; g_RequestedTrack = "";
    }

    void __thiscall SetVolume(int volume) override { ApplyMCIVolume(); }
    void __thiscall Option(int option, int value) override {}
    void __thiscall Destroy() override {}
};

CWzAudioImpl g_AudioInstance;

// ---------------------------------------------------------------------------
//  DIRECTSOUND HOOKOK
// ---------------------------------------------------------------------------

HRESULT __stdcall HookedDirectSoundCreate(LPGUID lpGuid, void** ppDS, void* pUnkOuter)
{
    if (TrueDirectSoundCreate) return TrueDirectSoundCreate(lpGuid, ppDS, pUnkOuter);
    return 0x887800F0;
}

HRESULT __stdcall HookedDirectSoundCreate8(LPGUID lpGuid, void** ppDS8, void* pUnkOuter)
{
    if (TrueDirectSoundCreate8) return TrueDirectSoundCreate8(lpGuid, ppDS8, pUnkOuter);
    return 0x887800F0;
}

void HookDirectSoundAPI()
{
    HMODULE hDsound = GetModuleHandleA("dsound.dll");
    if (!hDsound) hDsound = LoadLibraryA("dsound.dll");

    if (hDsound) {
        TrueDirectSoundCreate = (DirectSoundCreate_t)GetProcAddress(hDsound, "DirectSoundCreate");
        TrueDirectSoundCreate8 = (DirectSoundCreate8_t)GetProcAddress(hDsound, "DirectSoundCreate8");
    }
}

// ---------------------------------------------------------------------------
//  S9+ STUB EXPORTOK
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamInfo(int unk1, int unk2) { return 0; }
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamOffsetSec(int unk1) { return 0; }
extern "C" __declspec(dllexport) int __cdecl wzAudioGetVolume() { return GetMusicVolumeFromRegistry(); }
extern "C" __declspec(dllexport) int __cdecl wzAudioOpenFile(const char* filePath) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioPause() { mciSendStringA("pause my_mp3", NULL, 0, NULL); return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSeek(int position) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSetEqualizer(int eqMode) { return 0; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSetMixerMode(int mode) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioVolumeDown() { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioVolumeUp() { return 1; }

// ---------------------------------------------------------------------------
//  WZAUDIO ALAP EXPORT FÜGGVÉNYEK
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) void* __cdecl wzAudioCreate() {
    mciSendStringA("close all", NULL, 0, NULL);

    if (!g_audio_ready) {
        if (SUCCEEDED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            if (SUCCEEDED(g_xaudio->CreateMasteringVoice(&g_master))) {
                DWORD channelMask = 0;
                g_master->GetChannelMask(&channelMask);
                if (channelMask == 0) channelMask = SPEAKER_STEREO;
                X3DAudioInitialize(channelMask, X3DAUDIO_SPEED_OF_SOUND, g_x3d);
                g_audio_ready = true;
            }
        }
    }

    CreateMCIHelperWindow();

    CreateThread(NULL, 0, AudioWatcherThread, NULL, 0, NULL);

    return &g_AudioInstance;
}

extern "C" __declspec(dllexport) int __cdecl wzAudioPlay(const char* filePath, int volume, int unknown) {
    g_AudioInstance.Play(filePath, volume, unknown);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl wzAudioStop() { g_AudioInstance.Stop(); return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioOption(int option, int value) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSetVolume(int volume) { g_AudioInstance.SetVolume(volume); return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamOffsetRange(int unk1, int unk2) { return 1; }

extern "C" __declspec(dllexport) int __cdecl wzAudioDestroy() {
    if (g_HwndMCI) { DestroyWindow(g_HwndMCI); g_HwndMCI = NULL; }
    if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
    if (g_xaudio) { g_xaudio->Release(); g_xaudio = nullptr; }
    return 1;
}

// ---------------------------------------------------------------------------
//  DLLMAIN – LOGGER + HOOKOK
// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);
        InitLogger();
        HookDirectSoundAPI();
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate", (DWORD)HookedDirectSoundCreate);
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate8", (DWORD)HookedDirectSoundCreate8);
    }
    return TRUE;
}
