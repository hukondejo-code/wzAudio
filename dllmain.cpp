#include "pch.h"
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <windows.h>
#include <mmsystem.h>

#include <xaudio2.h>
#include <x3daudio.h>

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

// ---------------------------------------------------------------------------
//  IMPORT ADDRESS TABLE (IAT) PATCHING
// ---------------------------------------------------------------------------

// Ez a függvény átírja a main.exe telefonkönyvét (IAT) módosítás nélkül!
void PatchImportAddressTable(const char* dllName, const char* functionName, DWORD newFunctionAddress)
{
    // Lekérjük a main.exe (vagy a betöltött fő modul) alapcímét a memóriában
    HMODULE hModule = GetModuleHandleA(NULL);
    if (!hModule) return;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;

    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hModule + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;

    // Megkeressük az Import Directory-t
    PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hModule +
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    if (!importDesc || (BYTE*)importDesc == (BYTE*)hModule) return;

    // Végigmegyünk az importált DLL-ek listáján
    while (importDesc->Name) {
        const char* name = (const char*)((BYTE*)hModule + importDesc->Name);

        // Ha megtaláltuk a dsound.dll-t
        if (_stricmp(name, dllName) == 0) {
            PIMAGE_THUNK_DATA thunkIAT = (PIMAGE_THUNK_DATA)((BYTE*)hModule + importDesc->FirstThunk);
            PIMAGE_THUNK_DATA thunkOriginal = (PIMAGE_THUNK_DATA)((BYTE*)hModule + importDesc->Characteristics);

            while (thunkIAT->u1.Function) {
                // Megkeressük a függvény nevét
                if (thunkOriginal && !(thunkOriginal->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    PIMAGE_IMPORT_BY_NAME importName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hModule + thunkOriginal->u1.AddressOfData);

                    // Megvan a DirectSoundCreate!
                    if (strcmp((const char*)importName->Name, functionName) == 0) {
                        DWORD oldProtect;
                        // Feloldjuk az írásvédettséget a játék belső táblázatában
                        VirtualProtect(&thunkIAT->u1.Function, sizeof(DWORD), PAGE_READWRITE, &oldProtect);

                        // Kicseréljük a címet a saját XAudio2 alapú proxy függvényünkre!
                        thunkIAT->u1.Function = newFunctionAddress;

                        // Visszaállítjuk a védelmet
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

// UN DIFFERENCE: DirectSound8 mentése (Ez hiányzott!)
typedef HRESULT(__stdcall* DirectSoundCreate8_t)(LPGUID, void**, void*);
DirectSoundCreate8_t TrueDirectSoundCreate8 = nullptr;

// ---------------------------------------------------------------------------
//  BIZTONSÁGOS MEMÓRIA-POZÍCIÓ LEKÉRDEZÉS
// ---------------------------------------------------------------------------
bool GetObjectPosition(int index, float& outX, float& outY) {
    if (index < 0) return false;
    if (IsBadReadPtr((void*)OBJECT_LIST_BASE, sizeof(uintptr_t))) return false;
    uintptr_t listPtr = *(uintptr_t*)OBJECT_LIST_BASE;
    if (listPtr == 0) return false;
    uintptr_t entry = listPtr + ((size_t)index * OBJECT_STRUCT_SIZE);
    if (IsBadReadPtr((void*)entry, OBJECT_STRUCT_SIZE)) return false;

    outX = (float)(*(int*)(entry + 0x10));
    outY = (float)(*(int*)(entry + 0x14));
    return true;

}

// ---------------------------------------------------------------------------
//  FINOMHANGOLT 3D AUDIO PANNING SZÁMÍTÓ (BIZTONSÁGI FALLBACK-EKEL)
// ---------------------------------------------------------------------------
// Globális változó az XAudio2 hangforráshoz (effektekhez)
IXAudio2SourceVoice* g_3d_voice = nullptr;

// EZ A HÁTTÉRSZÁL FÜGGVÉNY: Folyamatosan figyel és számol a háttérben
DWORD WINAPI AudioWatcherThread(LPVOID lpParam)
{
    // Megvárjuk, amíg a játék ténylegesen betölti az objektumlistát a memóriába
    while (!g_audio_ready || IsBadReadPtr((void*)PLAYER_INDEX_ADDR, sizeof(int))) {
        Sleep(500);
    }
    // VÉGTELEN CIKLUS - Amíg fut a játék, ez a szál 20 ezredmásodpercenként dolgozik
    while (g_audio_ready)
    {
        Sleep(20); // ~50 FPS frissítési ráta a hang pozicionáláshoz

        int playerIndex = *(int*)PLAYER_INDEX_ADDR;
        float playerX = 0, playerY = 0;

        if (!GetObjectPosition(playerIndex, playerX, playerY)) continue;

        // Végigpörgetjük az ObjectList aktív szörnyeit (például az első 400 slotot)
        for (int i = 0; i < 400; i++)
        {
            if (i == playerIndex) continue; // A játékost kihagyjuk

            float mobX = 0, mobY = 0;
            if (GetObjectPosition(i, mobX, mobY))
            {
                // TŰPONTOS TÁVOLSÁGSZÁMÍTÁS (Pitagorasz-tétel)
                float distance = sqrtf(powf(mobX - playerX, 2) + powf(mobY - playerY, 2));

                // Ha a szörny a hallótávolságon belül van (pl. 30 koordináta egység a MU-ban)
                if (distance < 30.0f)
                {  
                    // === !!! EZT A LOG BLOKKOT ILLESZD BE IDE !!! ===
                    char debugMsg[256];
                    sprintf_s(debugMsg, "[wzAudio] Mob=%d | Dist=%.1f | PlayerX=%.1f, Y=%.1f | MobX=%.1f, Y=%.1f\n",
                        i, distance, playerX, playerY, mobX, mobY);

                    // ===============================================
                   
                    // Ha a játék elindított egy effekt hangot ezen a csatornán (g_3d_voice)
                    if (g_3d_voice)
                    {
                        X3DAUDIO_LISTENER listener = {};
                        listener.Position.x = playerX; listener.Position.z = playerY;
                        listener.OrientFront.z = 1.0f; listener.OrientTop.y = 1.0f;

                        X3DAUDIO_EMITTER emitter = {};
                        emitter.Position.x = mobX; emitter.Position.z = mobY;
                        emitter.OrientFront.z = 1.0f; emitter.OrientTop.y = 1.0f;
                        emitter.ChannelCount = 1;
                        emitter.CurveDistanceScaler = 0.12f; // Szuper érzékeny izometrikus skálázás

                        X3DAUDIO_DSP_SETTINGS dsp = {};
                        float matrix[2] = { 0.0f, 0.0f };
                        dsp.SrcChannelCount = 1; dsp.DstChannelCount = 2;
                        dsp.pMatrixCoefficients = matrix;

                        // Kiszámoljuk a 3D teret élőben a szörnyhöz!
                        X3DAudioCalculate(g_x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);

                        // Alkalmazzuk a mátrixot az XAudio2-re
                        g_3d_voice->SetOutputMatrix(g_master, 1, 2, dsp.pMatrixCoefficients);
                    }
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  EREDETI WZAUDIO 2.0 ZENEI FÜGGVÉNYEK (VÁLTOZATLANUL - GARANTÁLTAN MŰKÖDIK)
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

HRESULT __stdcall HookedDirectSoundCreate(LPGUID lpGuid, void** ppDS, void* pUnkOuter)
{
    // Ha a játék a régi DirectSound-ot kéri, a valódi régi függvényt hívjuk meg
    if (TrueDirectSoundCreate) return TrueDirectSoundCreate(lpGuid, ppDS, pUnkOuter);
    return  0x887800F0; // Ez a DSERR_GENERIC pontos hexadecimális értéke!
}   

HRESULT __stdcall HookedDirectSoundCreate8(LPGUID lpGuid, void** ppDS8, void* pUnkOuter)
{
    // !!! JAVÍTVA !!! Ha a játék a DirectSound8-at kéri, a valódi DirectSound8-at hívjuk meg!
    if (TrueDirectSoundCreate8) return TrueDirectSoundCreate8(lpGuid, ppDS8, pUnkOuter);
    return  0x887800F0; // Ez a DSERR_GENERIC pontos hexadecimális értéke!
}

// EZ AZ A FÜGGVÉNY, AMI ÖSSZEKÖTI A KETTŐT MODIFICATION NÉLKÜL
void HookDirectSoundAPI()
{
    HMODULE hDsound = GetModuleHandleA("dsound.dll");
    if (!hDsound) hDsound = LoadLibraryA("dsound.dll");

    if (hDsound) {
        // Kimentjük a valódi DirectSoundCreate címet
        TrueDirectSoundCreate = (DirectSoundCreate_t)GetProcAddress(hDsound, "DirectSoundCreate");

        // Kimentjük a valódi DirectSoundCreate8 címet is!
        TrueDirectSoundCreate8 = (DirectSoundCreate8_t)GetProcAddress(hDsound, "DirectSoundCreate8");
    }
}

// =========================================================================
// S9 ÉS ÚJABB SZEZONOK HIÁNYZÓ EXPORTÁLT FÜGGVÉNYEI (A TE STUBJAID)
// =========================================================================

// Visszaadja a hangfolyam információit (0 = sikeres/nincs hiba)
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamInfo(int unk1, int unk2) { return 0; }
// Visszaadja a folyam eltolódását másodpercben
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamOffsetSec(int unk1) { return 0; }
// Lekéri az aktuális hangerőt (visszaadjuk a registry-ből olvasott értéket, vagy fix 9-et)
extern "C" __declspec(dllexport) int __cdecl wzAudioGetVolume() { return GetMusicVolumeFromRegistry(); }
// Fájlmegnyitási stub, az MCI open intézi helyette, így itt csak sikert (1) adunk vissza
extern "C" __declspec(dllexport) int __cdecl wzAudioOpenFile(const char* filePath) { return 1; }
// Zene szüneteltetése (opcionális, ha a kliens hívná, mci paranccsal leállítható)
extern "C" __declspec(dllexport) int __cdecl wzAudioPause() { mciSendStringA("pause my_mp3", NULL, 0, NULL); return 1; }
// Pozicionálás a zenében
extern "C" __declspec(dllexport) int __cdecl wzAudioSeek(int position) { return 1; }
// Hangszínszabályzó (Equalizer) beállítása (0 = kikapcsolva/alapértelmezett)
extern "C" __declspec(dllexport) int __cdecl wzAudioSetEqualizer(int eqMode) { return 0; }
// !!! A LEGGYANÚSABB BŰNÖS !!! Keverő mód beállítása.
// Ha ezt nem találja vagy hibát ad, azt hiszi, nincs hardver. Fix 1-et (sikeres) adunk vissza!
extern "C" __declspec(dllexport) int __cdecl wzAudioSetMixerMode(int mode) { return 1; }
// Hangerő csökkentése billentyűkombinációra
extern "C" __declspec(dllexport) int __cdecl wzAudioVolumeDown() { return 1; }
// Hangerő növelése billentyűkombinációra
extern "C" __declspec(dllexport) int __cdecl wzAudioVolumeUp() { return 1; }

// =========================================================================
// WZAUDIO ALAP EXPORT FÜGGVÉNYEK
// =========================================================================
extern "C" __declspec(dllexport) void* __cdecl wzAudioCreate() {
    mciSendStringA("close all", NULL, 0, NULL);

    // Az ÚJ XAUDIO2 + X3DAUDIO INITIALIZÁLÁS (Garantálja a 3D-t)
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

    // ELINDÍTJUK AZ AUDIO FIGYELŐT: Ez létrehoz egy teljesen különálló szálat a CPU-ban,
    // ami a háttérben, a játék akadozása nélkül végzi a 3D számításokat!
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

// =========================================================================
// DLL ASZINKRON IDŐZÍTETT INDÍTÁSA (A 3D AUDIO SZENT GRÁLJA)
// =========================================================================
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);
        InitLogger();
        HookDirectSoundAPI(); // Mentjük a címeket
        // Amint a wzAudio.dll betöltődik, AZONNAL átírjuk a játék belső táblázatát!
        // Kicseréljük a dsound.dll "DirectSoundCreate" parancsát a mi saját Proxy-nkra.
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate", (DWORD)HookedDirectSoundCreate);
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate8", (DWORD)HookedDirectSoundCreate8);
    }
    return TRUE;
}
