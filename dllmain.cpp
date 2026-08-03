#include "pch.h"
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <fstream>
#include <mutex>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "xaudio2.lib")

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  GLOBÁLIS AUDIO STRUKTÚRÁK
// ---------------------------------------------------------------------------
IXAudio2* g_xaudio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
bool g_audio_ready = false;

std::string g_CurrentTrack = "";
std::string g_RequestedTrack = "";
HWND g_HwndMCI = NULL;

typedef HRESULT(__stdcall* DirectSoundCreate_t)(LPGUID, void**, void*);
DirectSoundCreate_t TrueDirectSoundCreate = nullptr;
typedef HRESULT(__stdcall* DirectSoundCreate8_t)(LPGUID, void**, void*);
DirectSoundCreate8_t TrueDirectSoundCreate8 = nullptr;

struct DSBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    WAVEFORMATEX* lpwfxFormat;
    GUID guid3DAlgorithm;
};

// ---------------------------------------------------------------------------
//  ZSENIÁLIS DIRECTSOUND BUFFER WRAPPER (A PANNING KIKÉNYSZERÍTÉSE)
// ---------------------------------------------------------------------------
class MyDirectSoundBuffer : public IUnknown {
public:
    IUnknown* m_pRealBuffer;
    IXAudio2SourceVoice* m_pXAudioVoice;
    WAVEFORMATEX m_wfx;
    std::vector<BYTE> m_audioData;
    float m_currentVolume;
    float m_currentPan; // -1.0f (Bal) és +1.0f (Jobb) között

    MyDirectSoundBuffer(IUnknown* pReal, const DSBUFFERDESC* pDesc) {
        m_pRealBuffer = pReal;
        m_pXAudioVoice = nullptr;
        m_currentVolume = 1.0f;
        m_currentPan = 0.0f; // Középen
        if (pDesc && pDesc->lpwfxFormat) m_wfx = *(pDesc->lpwfxFormat);
        else ZeroMemory(&m_wfx, sizeof(WAVEFORMATEX));
    }

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override { return m_pRealBuffer->QueryInterface(riid, ppvObj); }
    ULONG __stdcall AddRef() override { return m_pRealBuffer->AddRef(); }
    ULONG __stdcall Release() override {
        ULONG res = m_pRealBuffer->Release();
        if (res == 0) {
            if (m_pXAudioVoice) { m_pXAudioVoice->DestroyVoice(); m_pXAudioVoice = nullptr; }
            delete this; return 0;
        }
        return res;
    }

    // DirectSound kötelező VTable eltolások a crash ellen
    virtual HRESULT __stdcall GetCaps(void* p1) { return ((HRESULT(__stdcall*)(void*, void*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetCurrentPosition(DWORD* p1, DWORD* p2) { return ((HRESULT(__stdcall*)(void*, DWORD*, DWORD*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1, p2); }
    virtual HRESULT __stdcall GetFormat(void* p1, DWORD p2, DWORD* p3) { return ((HRESULT(__stdcall*)(void*, void*, DWORD, DWORD*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1, p2, p3); }
    virtual HRESULT __stdcall GetVolume(LONG* p1) { return ((HRESULT(__stdcall*)(void*, LONG*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetPan(LONG* p1) { return ((HRESULT(__stdcall*)(void*, LONG*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetFrequency(DWORD* p1) { return ((HRESULT(__stdcall*)(void*, DWORD*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetStatus(DWORD* p1) { return ((HRESULT(__stdcall*)(void*, DWORD*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall Initialize(void* p1, const void* p2) { return ((HRESULT(__stdcall*)(void*, void*, const void*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1, p2); }

    virtual HRESULT __stdcall Lock(DWORD dwWriteCursor, DWORD dwWriteBytes, void** ppvAudioPtr1, DWORD* pdwAudioBytes1, void** ppvAudioPtr2, DWORD* pdwAudioBytes2, DWORD dwFlags) {
        return ((HRESULT(__stdcall*)(void*, DWORD, DWORD, void**, DWORD*, void**, DWORD*, DWORD))(*(void***)m_pRealBuffer))(m_pRealBuffer, dwWriteCursor, dwWriteBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags);
    }

    virtual HRESULT __stdcall SetCurrentPosition(DWORD p1) { return ((HRESULT(__stdcall*)(void*, DWORD))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall SetFormat(const void* p1) { return ((HRESULT(__stdcall*)(void*, const void*))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall SetFrequency(DWORD p1) { return ((HRESULT(__stdcall*)(void*, DWORD))(*(void***)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall Restore() { return ((HRESULT(__stdcall*)(void*))(*(void***)m_pRealBuffer))(m_pRealBuffer); }

    // --- KRITIKUS HANGERŐ ÉS BALANSZ HOOKOK ---
    // A játék ide küldi be a szörnyek pozíciója alapján kiszámított hangerőt (-10000 és 0 között)
    virtual HRESULT __stdcall SetVolume(LONG dwVolume) {
        float dsVol = (float)dwVolume;
        // Átszámoljuk XAudio2 lebegőpontos (0.0f - 1.0f) skálára
        m_currentVolume = powf(10.0f, dsVol / 2000.0f);
        UpdateXAudioMatrix();
        return ((HRESULT(__stdcall*)(void*, LONG))(*(void***)m_pRealBuffer))(m_pRealBuffer, dwVolume);
    }

    // A játék ide küldi be a szörnyek pozíciója alapján kiszámított sztereó irányt (-10000 = Bal, +10000 = Jobb)
    virtual HRESULT __stdcall SetPan(LONG dwPan) {
        m_currentPan = (float)dwPan / 10000.0f; // Átszámoljuk -1.0f (Bal) és +1.0f (Jobb) közé
        UpdateXAudioMatrix();
        return ((HRESULT(__stdcall*)(void*, LONG))(*(void***)m_pRealBuffer))(m_pRealBuffer, dwPan);
    }

    // Segédfüggvény: Azonnal rákényszeríti a kiszámított sztereó együtthatókat az XAudio2-re
    void UpdateXAudioMatrix() {
        if (!m_pXAudioVoice || !g_master) return;

        // Kiszámoljuk a Bal és Jobb oldal arányát a Pan alapján
        float leftVolume = m_currentVolume * (1.0f - m_currentPan);
        float rightVolume = m_currentVolume * (1.0f + m_currentPan);

        // Korlátozzuk a határokat (0.0f - 1.0f közé)
        if (leftVolume > 1.0f) leftVolume = 1.0f;   if (leftVolume < 0.0f) leftVolume = 0.0f;
        if (rightVolume > 1.0f) rightVolume = 1.0f; if (rightVolume < 0.0f) rightVolume = 0.0f;

        float matrix[2] = { leftVolume, rightVolume };

        // KŐKEMÉNYEN RÁKÉNYSZERÍTJÜK A HARDVERRE A SZTEREÓ SZÉTVÁLASZTÁST!
        m_pXAudioVoice->SetOutputMatrix(g_master, 1, 2, matrix);
    }

    virtual HRESULT __stdcall Play(DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) {
        if (!m_audioData.empty() && g_audio_ready) {
            if (m_pXAudioVoice) { m_pXAudioVoice->DestroyVoice(); m_pXAudioVoice = nullptr; }

            m_wfx.nChannels = 1; // Mono kényszerítés (ffmpeg minták)
            m_wfx.nBlockAlign = m_wfx.wBitsPerSample / 8;
            m_wfx.nAvgBytesPerSec = m_wfx.nSamplesPerSec * m_wfx.nBlockAlign;

            if (SUCCEEDED(g_xaudio->CreateSourceVoice(&m_pXAudioVoice, &m_wfx))) {
                XAUDIO2_VOICE_SENDS sendList = { 0 };
                XAUDIO2_SEND_DESCRIPTOR sendDesc = { 0 };
                sendDesc.pOutputVoice = g_master;
                sendList.SendCount = 1;
                sendList.pSends = &sendDesc;
                m_pXAudioVoice->SetOutputVoices(&sendList);

                UpdateXAudioMatrix(); // Alkalmazzuk a panorámát

                XAUDIO2_BUFFER buffer = { 0 };
                buffer.AudioBytes = m_audioData.size();
                buffer.pAudioData = m_audioData.data();
                buffer.Flags = XAUDIO2_END_OF_STREAM;

                m_pXAudioVoice->SubmitSourceBuffer(&buffer);
                m_pXAudioVoice->Start(0);
                return 0; // DS_OK
            }
        }
        typedef HRESULT(__stdcall* Play_t)(void*, DWORD, DWORD, DWORD);
        return ((Play_t)(*(void***)m_pRealBuffer))(m_pRealBuffer, dwReserved1, dwPriority, dwFlags);
    }

    virtual HRESULT __stdcall Stop() {
        if (m_pXAudioVoice) m_pXAudioVoice->Stop(0);
        return ((HRESULT(__stdcall*)(void*))(*(void***)m_pRealBuffer))(m_pRealBuffer);
    }

    virtual HRESULT __stdcall Unlock(void* pvAudioPtr1, DWORD dwAudioBytes1, void* pvAudioPtr2, DWORD dwAudioBytes2) {
        if (pvAudioPtr1 && dwAudioBytes1 > 0) {
            m_audioData.resize(dwAudioBytes1);
            memcpy(m_audioData.data(), pvAudioPtr1, dwAudioBytes1);
        }
        return ((HRESULT(__stdcall*)(void*, void*, DWORD, void*, DWORD))(*(void***)m_pRealBuffer))(m_pRealBuffer, pvAudioPtr1, dwAudioBytes1, pvAudioPtr2, dwAudioBytes2);
    }
};

// ---------------------------------------------------------------------------
//  IDIRECTSOUND DEVICE WRAPPER
// ---------------------------------------------------------------------------
class MyDirectSound : public IUnknown {
public:
    IUnknown* m_pRealDS;
    MyDirectSound(IUnknown* pReal) { m_pRealDS = pReal; }

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override { return m_pRealDS->QueryInterface(riid, ppvObj); }
    ULONG __stdcall AddRef() override { return m_pRealDS->AddRef(); }
    ULONG __stdcall Release() override {
        ULONG res = m_pRealDS->Release();
        if (res == 0) { delete this; return 0; }
        return res;
    }

    virtual HRESULT __stdcall CreateSoundBuffer(const DSBUFFERDESC* pcDSBufferDesc, void** ppDSBuffer, IUnknown* pUnkOuter) {
        void* pRealBuffer = nullptr;
        HRESULT hr = ((HRESULT(__stdcall*)(void*, const DSBUFFERDESC*, void**, IUnknown*))(*(void***)m_pRealDS))(m_pRealDS, pcDSBufferDesc, &pRealBuffer, pUnkOuter);
        if (SUCCEEDED(hr) && pRealBuffer) {
            MyDirectSoundBuffer* myBuffer = new MyDirectSoundBuffer((IUnknown*)pRealBuffer, pcDSBufferDesc);
            *ppDSBuffer = (void*)myBuffer;
            return hr;
        }
        return hr;
    }
    virtual HRESULT __stdcall GetCaps(void* p1) { return ((HRESULT(__stdcall*)(void*, void*))((void**)m_pRealDS))(m_pRealDS, p1); }
    virtual HRESULT __stdcall SetCooperativeLevel(HWND p1, DWORD p2) { return ((HRESULT(__stdcall*)(void*, HWND, DWORD))((void**)m_pRealDS))(m_pRealDS, p1, p2); }
    virtual HRESULT __stdcall SpeakerConfig(DWORD p1) { return ((HRESULT(__stdcall*)(void*, DWORD))((void**)m_pRealDS))(m_pRealDS, p1); }
    virtual HRESULT __stdcall GetSpeakerConfig(DWORD* p1) { return ((HRESULT(__stdcall*)(void*, DWORD*))((void**)m_pRealDS))(m_pRealDS, p1); }
    virtual HRESULT __stdcall Initialize(const GUID* p1) { return ((HRESULT(__stdcall*)(void*, const GUID*))((void**)m_pRealDS))(m_pRealDS, p1); }
};

// ---------------------------------------------------------------------------
//  DIRECTSOUND API HOOKOK (IAT PATCHING)
// ---------------------------------------------------------------------------
HRESULT __stdcall HookedDirectSoundCreate(LPGUID lpGuid, void** ppDS, void* pUnkOuter) {
    if (!TrueDirectSoundCreate) return 0x887800F0;
    void* pRealDS = nullptr;
    HRESULT hr = TrueDirectSoundCreate(lpGuid, &pRealDS, pUnkOuter);
    if (SUCCEEDED(hr) && pRealDS) {
        MyDirectSound* myDS = new MyDirectSound((IUnknown*)pRealDS);
        *ppDS = (void*)myDS;
        return hr;
    }
    return hr;
}
HRESULT __stdcall HookedDirectSoundCreate8(LPGUID lpGuid, void** ppDS8, void* pUnkOuter) {
    if (!TrueDirectSoundCreate8) return 0x887800F0;
    void* pRealDS8 = nullptr;
    HRESULT hr = TrueDirectSoundCreate8(lpGuid, &pRealDS8, pUnkOuter);
    if (SUCCEEDED(hr) && pRealDS8) {
        MyDirectSound* myDS8 = new MyDirectSound((IUnknown*)pRealDS8);
        *ppDS8 = (void*)myDS8;
        return hr;
    }
    return hr;
}
void PatchImportAddressTable(const char* dllName, const char* functionName, DWORD newFunctionAddress) {
    HMODULE hModule = GetModuleHandleA(NULL);
    if (!hModule) return;
    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hModule;
    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hModule + dosHeader->e_lfanew);
    PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hModule + ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
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
//  MCI HÁTTÉRZENE MOTOR (VÁLTOZATLAN, STABIL)
// ---------------------------------------------------------------------------
int GetMusicVolumeFromRegistry() {HKEY hKey; 
DWORD musicVolume = 9; 
DWORD dataSize = sizeof(musicVolume);
if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    RegQueryValueExA(hKey, "VolumeLevel", NULL, NULL, (LPBYTE)&musicVolume, &dataSize); 
    RegCloseKey(hKey);}
int mciVolume = musicVolume * 111; 
if (mciVolume > 1000) mciVolume = 1000; 
if (mciVolume < 0) mciVolume = 0;
return mciVolume;}
void ApplyMCIVolume() {
    if (g_CurrentTrack == "") return;
    int volume = GetMusicVolumeFromRegistry();std::string volCmd = "setaudio my_mp3 volume to " + std::to_string(volume);
    mciSendStringA(volCmd.c_str(), NULL, 0, NULL);}
std::string GetRandomMp3FromFolderIfNeeded(const std::string& originalPath, const std::string& lastPlayedTrack = "") {
    std::string folder_name = originalPath; 
    size_t last_dot = folder_name.find_last_of(".");
    if (last_dot != std::string::npos) folder_name = folder_name.substr(0, last_dot);
    if (fs::exists(folder_name) && fs::is_directory(folder_name)) {
        std::vector<std::string> mp3_list;
        for (const auto& entry : fs::directory_iterator(folder_name)) { 
if (entry.path().extension() == ".mp3") mp3_list.push_back(entry.path().string()); }
if (!mp3_list.empty()) {
if (mp3_list.size() == 1) 
return mp3_list[0];
std::random_device rd; 
std::mt19937 gen(rd()); 
std::uniform_int_distribution<> distr(0, mp3_list.size() - 1);
std::string selected; 
do { selected = mp3_list[distr(gen)]; } 
while (selected == lastPlayedTrack);
return selected;}}
return originalPath;}
void PlayNextRotatedTrack() {mciSendStringA("close my_mp3", NULL, 0, NULL);
g_CurrentTrack = GetRandomMp3FromFolderIfNeeded(g_RequestedTrack, g_CurrentTrack);
std::string openCmd = "open \"" + g_CurrentTrack + "\" type mpegvideo alias my_mp3";
if (mciSendStringA(openCmd.c_str(), NULL, 0, NULL) == 0) {
std::string playCmd = "play my_mp3 notify"; 
mciSendStringA(playCmd.c_str(), NULL, 0, (HWND)g_HwndMCI); ApplyMCIVolume();}}
LRESULT CALLBACK MCIWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
if (uMsg == MM_MCINOTIFY && wParam == MCI_NOTIFY_SUCCESSFUL) { 
PlayNextRotatedTrack(); 
return 0; }
return DefWindowProc(hwnd, uMsg, wParam, lParam);}
void CreateMCIHelperWindow() {
if (g_HwndMCI != NULL) return;
WNDCLASSEXA wc = { 0 }; 
wc.cbSize = sizeof(WNDCLASSEXA); 
wc.lpfnWndProc = MCIWindowProc; 
wc.hInstance = GetModuleHandle(NULL); 
wc.lpszClassName = "MuWzAudioMCIHelper"; 
RegisterClassExA(&wc);
g_HwndMCI = CreateWindowExA(0, wc.lpszClassName, "MCI Helper", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);}
class IWzAudio {
public:
    virtual int  __thiscall GetStreamOffsetRange(int unk1, int unk2) = 0;
    virtual void __thiscall Play(const char* filePath, int volume, int unknown) = 0;
    virtual void __thiscall Stop() = 0;virtual void __thiscall SetVolume(int volume) = 0;
    virtual void __thiscall Option(int option, int value) = 0;virtual void __thiscall Destroy() = 0;};
class CWzAudioImpl : public IWzAudio {
public:int __thiscall GetStreamOffsetRange(int unk1, int unk2) override { return 0; }
      void __thiscall Play(const char* filePath, int volume, int unknown) override {
          if (!filePath) return;HKEY hKey; 
          DWORD musicOnOff = 1; 
          DWORD dataSize = sizeof(musicOnOff);
          if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
              if (RegQueryValueExA(hKey, "MusicOn", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize) != ERROR_SUCCESS) 
                  RegQueryValueExA(hKey, "MusicOnOff", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize);RegCloseKey(hKey);}
          if (musicOnOff == 0) { Stop(); return; }
          if (g_RequestedTrack == filePath) { 
              ApplyMCIVolume(); return; }g_RequestedTrack = filePath; 
          g_CurrentTrack = ""; PlayNextRotatedTrack();}
      void __thiscall Stop() override { mciSendStringA("close my_mp3", NULL, 0, NULL); g_CurrentTrack = ""; g_RequestedTrack = ""; }
      void __thiscall SetVolume(int volume) override { ApplyMCIVolume(); }
      void __thiscall Option(int option, int value) override {}void __thiscall Destroy() override {}
};

CWzAudioImpl g_AudioInstance;

// ---------------------------------------------------------------------------
//  EXPORT FUNKCIÓK AZ EX501 KLIENSNEK
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
extern "C" __declspec(dllexport) void* __cdecl wzAudioCreate() {mciSendStringA("close all", NULL, 0, NULL);if (!g_audio_ready) {
    if (SUCCEEDED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        // Expliciten kényszerítjük a 2 hardvercsatornát az XAudio2-nek
        if (SUCCEEDED(g_xaudio->CreateMasteringVoice(&g_master, 2, 44100, 0, NULL, NULL, AudioCategory_GameEffects))) {g_audio_ready = true;}}}
CreateMCIHelperWindow();
return &g_AudioInstance;
}
extern "C" __declspec(dllexport) int __cdecl wzAudioPlay(const char* filePath, int volume, int unknown) { g_AudioInstance.Play(filePath, volume, unknown); return 1; }
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
//  DLLMAIN – TISZTA HOOK RENDSZER (NINCS INLINE PATCH, JÖHET A ZENE!)
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID){
    if (reason == DLL_PROCESS_ATTACH){
        DisableThreadLibraryCalls(hInst);
        HMODULE hDsound = GetModuleHandleA("dsound.dll");
        if (!hDsound) hDsound = LoadLibraryA("dsound.dll");
        if (hDsound) {
            TrueDirectSoundCreate = (DirectSoundCreate_t)GetProcAddress(hDsound, "DirectSoundCreate");
            TrueDirectSoundCreate8 = (DirectSoundCreate8_t)GetProcAddress(hDsound, "DirectSoundCreate8");
        }
        // Becsatoljuk a tiszta API wrappereket
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate", (DWORD)HookedDirectSoundCreate);
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate8", (DWORD)HookedDirectSoundCreate8);
    }
    return TRUE;
}