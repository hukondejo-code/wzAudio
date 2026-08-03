#include "pch.h"
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <x3daudio.h>
#include <fstream>
#include <mutex>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "xaudio2.lib")

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  GLOBÁLIS AUDIO ÉS BEÁLLÍTÁS VÁLTOZÓK
// ---------------------------------------------------------------------------
std::string g_CurrentTrack = "";
std::string g_RequestedTrack = "";
HWND g_HwndMCI = NULL;

IXAudio2* g_xaudio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
X3DAUDIO_HANDLE g_x3d = { 0 };
bool g_audio_ready = false;
DWORD g_masterChannels = 2;

uintptr_t OBJECT_LIST_BASE = 0x07B5258C;
const size_t OBJECT_STRUCT_SIZE = 0x538;

const int MAX_VOICE_POOL = 64;
struct Active3DSound {
    IXAudio2SourceVoice* pSourceVoice;
    int targetMobIndex;
    bool bInUse;
};
Active3DSound g_VoicePool[MAX_VOICE_POOL] = { 0 };
std::mutex g_VoicePoolMutex;

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
//  LOGOLÁS RENDSZER
// ---------------------------------------------------------------------------
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
    sprintf_s(line, "[%02d:%02d:%02d.%03d] %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    LogDebug(line);
}

// ---------------------------------------------------------------------------
//  BIZTONSÁGOS MEMÓRIA-ELLENŐRZÉS ÉS REFERENCIAPONT KÖVETÉS
// ---------------------------------------------------------------------------
bool IsReadable(void* ptr, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (end <= regionEnd);
}

bool GetReferencePosition(short& outX, short& outY)
{
    if (!IsReadable((void*)OBJECT_LIST_BASE, sizeof(uintptr_t))) return false;
    uintptr_t listPtr = *(uintptr_t*)OBJECT_LIST_BASE;
    if (!IsReadable((void*)listPtr, OBJECT_STRUCT_SIZE)) return false;
    uintptr_t entry = listPtr + (OBJECT_STRUCT_SIZE); // idx=1 (Player saját karakter)
    if (!IsReadable((void*)(entry + 0xAC), sizeof(short)) || !IsReadable((void*)(entry + 0xB0), sizeof(short))) return false;
    outX = *(short*)(entry + 0xAC);
    outY = *(short*)(entry + 0xB0);
    return true;
}

// ---------------------------------------------------------------------------
//  WAV HANGFAJL BETÖLTŐ I/O RENDSZER
// ---------------------------------------------------------------------------
bool LoadWavFile(const std::string& filePath, std::vector<BYTE>& audioData, WAVEFORMATEX& wfx) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;
    char chunkId[4];
    file.read(chunkId, 4);
    if (strncmp(chunkId, "RIFF", 4) != 0) return false;
    file.seekg(4, std::ios::cur);
    file.read(chunkId, 4);
    if (strncmp(chunkId, "WAVE", 4) != 0) return false;
    while (file.read(chunkId, 4)) {
        DWORD chunkSize; file.read((char*)&chunkSize, 4);
        if (strncmp(chunkId, "fmt ", 4) == 0) {
            file.read((char*)&wfx, sizeof(WAVEFORMATEX));
            if (chunkSize > sizeof(WAVEFORMATEX)) file.seekg(chunkSize - sizeof(WAVEFORMATEX), std::ios::cur);
        }
        else if (strncmp(chunkId, "data", 4) == 0) {
            audioData.resize(chunkSize);
            file.read((char*)audioData.data(), chunkSize);
            break;
        }
        else {
            file.seekg(chunkSize, std::ios::cur);
        }
    }
    return !audioData.empty();
}

// ---------------------------------------------------------------------------
//  XAUDIO2 DYNAMIC VOICE EFFECT POOL INDÍTÓ
// ---------------------------------------------------------------------------
void Trigger3DEffectByFileName(const char* waveFileName, int mobIndex)
{
    if (!g_audio_ready || !waveFileName) return;
    std::string fullPath = "Data\\Sound\\" + std::string(waveFileName);
    WAVEFORMATEX wfx = { 0 };
    std::vector<BYTE> audioData;
    if (!LoadWavFile(fullPath, audioData, wfx)) return;

    std::lock_guard<std::mutex> lock(g_VoicePoolMutex);
    int freeSlot = -1;
    for (int i = 0; i < MAX_VOICE_POOL; i++) {
        if (!g_VoicePool[i].bInUse) { freeSlot = i; break; }
        else {
            XAUDIO2_VOICE_STATE state; g_VoicePool[i].pSourceVoice->GetState(&state);
            if (state.SamplesPlayed == 0 && state.BuffersQueued == 0) {
                g_VoicePool[i].pSourceVoice->DestroyVoice();
                g_VoicePool[i].bInUse = false;
                freeSlot = i; break;
            }
        }
    }
    if (freeSlot == -1) return;

    wfx.nChannels = 1; // ffmpeg mono kényszerítés
    wfx.nBlockAlign = wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    IXAudio2SourceVoice* pSourceVoice = nullptr;
    if (SUCCEEDED(g_xaudio->CreateSourceVoice(&pSourceVoice, &wfx))) {
        XAUDIO2_VOICE_SENDS sendList = { 0 };
        XAUDIO2_SEND_DESCRIPTOR sendDesc = { 0 };
        sendDesc.pOutputVoice = g_master;
        sendList.SendCount = 1;
        sendList.pSends = &sendDesc;
        pSourceVoice->SetOutputVoices(&sendList);

        XAUDIO2_BUFFER buffer = { 0 };
        buffer.AudioBytes = audioData.size();
        buffer.pAudioData = audioData.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        if (SUCCEEDED(pSourceVoice->SubmitSourceBuffer(&buffer))) {
            pSourceVoice->Start(0);
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
//  X3DAUDIO VALÓS IDEJŰ POZÍCIÓKÖVETŐ SZÁL (WATCHER)
// ---------------------------------------------------------------------------
DWORD WINAPI AudioWatcherThread(LPVOID lpParam)
{
    while (true) {
        short tx = 0, ty = 0;
        if (g_audio_ready && GetReferencePosition(tx, ty)) { if (tx != 0 && ty != 0) break; }
        Sleep(200);
    }
    while (g_audio_ready) {
        Sleep(16);
        short refX = 0, refY = 0;
        if (!GetReferencePosition(refX, refY) || (refX == 0 && refY == 0)) continue;
        uintptr_t listPtr = *(uintptr_t*)OBJECT_LIST_BASE;
        if (!IsReadable((void*)listPtr, OBJECT_STRUCT_SIZE * 400)) continue;

        std::lock_guard<std::mutex> lock(g_VoicePoolMutex);
        for (int i = 0; i < MAX_VOICE_POOL; i++) {
            if (!g_VoicePool[i].bInUse) continue;
            XAUDIO2_VOICE_STATE state; g_VoicePool[i].pSourceVoice->GetState(&state);
            if (state.SamplesPlayed == 0 && state.BuffersQueued == 0) {
                g_VoicePool[i].pSourceVoice->DestroyVoice(); g_VoicePool[i].bInUse = false; continue;
            }
            int mobIdx = g_VoicePool[i].targetMobIndex;
            uintptr_t entry = listPtr + (mobIdx * OBJECT_STRUCT_SIZE);
            if (IsReadable((void*)entry, OBJECT_STRUCT_SIZE)) {
                short mobX = *(short*)(entry + 0xAC); short mobY = *(short*)(entry + 0xB0);
                if (mobX == 0 && mobY == 0) { g_VoicePool[i].pSourceVoice->SetVolume(0.0f); continue; }

                X3DAUDIO_LISTENER listener = {};
                listener.Position.x = (float)refX * 0.1f; listener.Position.z = (float)refY * 0.1f;
                listener.OrientFront.z = 1.0f; listener.OrientTop.y = 1.0f;

                X3DAUDIO_EMITTER emitter = {};
                emitter.Position.x = (float)mobX * 0.1f; emitter.Position.z = (float)mobY * 0.1f;
                emitter.OrientFront.z = 1.0f; emitter.OrientTop.y = 1.0f;
                emitter.ChannelCount = 1; emitter.CurveDistanceScaler = 1.0f;

                X3DAUDIO_DSP_SETTINGS dsp = {};
                float matrix[2] = { 0.0f, 0.0f };
                dsp.SrcChannelCount = 1; dsp.DstChannelCount = 2; dsp.pMatrixCoefficients = matrix;

                X3DAudioCalculate(g_x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
                g_VoicePool[i].pSourceVoice->SetOutputMatrix(g_master, 1, 2, dsp.pMatrixCoefficients);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  IDIRECTSOUND HOOK ÉS PROXY BUFFER WRAPPER OSZTÁLYOK
// ---------------------------------------------------------------------------
class MyDirectSoundBuffer : public IUnknown {
public:
    IUnknown* m_pRealBuffer;
    WAVEFORMATEX m_wfx;

    MyDirectSoundBuffer(IUnknown* pReal, const DSBUFFERDESC* pDesc) {
        m_pRealBuffer = pReal;
        if (pDesc && pDesc->lpwfxFormat) m_wfx = *(pDesc->lpwfxFormat);
        else ZeroMemory(&m_wfx, sizeof(WAVEFORMATEX));
    }
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override { return m_pRealBuffer->QueryInterface(riid, ppvObj); }
    ULONG __stdcall AddRef() override { return m_pRealBuffer->AddRef(); }
    ULONG __stdcall Release() override {
        ULONG res = m_pRealBuffer->Release(); if (res == 0) { delete this; return 0; }
        return res;
    }
    virtual HRESULT __stdcall GetCaps(void* p1) { return ((HRESULT(__stdcall*)(void*, void*))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetCurrentPosition(DWORD* p1, DWORD* p2) { return ((HRESULT(__stdcall*)(void*, DWORD*, DWORD*))((void**)m_pRealBuffer))(m_pRealBuffer, p1, p2); }
    virtual HRESULT __stdcall GetFormat(void* p1, DWORD p2, DWORD* p3) { return ((HRESULT(__stdcall*)(void*, void*, DWORD, DWORD*))((void**)m_pRealBuffer))(m_pRealBuffer, p1, p2, p3); }
    virtual HRESULT __stdcall GetVolume(LONG* p1) { return ((HRESULT(__stdcall*)(void*, LONG*))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetPan(LONG* p1) { return ((HRESULT(__stdcall*)(void*, LONG*))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetFrequency(DWORD* p1) { return ((HRESULT(__stdcall*)(void*, DWORD*))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall GetStatus(DWORD* p1) { return ((HRESULT(__stdcall*)(void*, DWORD*))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall Initialize(void* p1, const void* p2) { return ((HRESULT(__stdcall*)(void*, void*, const void*))((void**)m_pRealBuffer))(m_pRealBuffer, p1, p2); }
    virtual HRESULT __stdcall Lock(DWORD dwWriteCursor, DWORD dwWriteBytes, void** ppvAudioPtr1, DWORD* pdwAudioBytes1, void** ppvAudioPtr2, DWORD* pdwAudioBytes2, DWORD dwFlags) { return ((HRESULT(__stdcall*)(void*, DWORD, DWORD, void**, DWORD*, void**, DWORD*, DWORD))((void**)m_pRealBuffer))(m_pRealBuffer, dwWriteCursor, dwWriteBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags); }
    virtual HRESULT __stdcall SetCurrentPosition(DWORD p1) { return ((HRESULT(__stdcall*)(void*, DWORD))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall SetFormat(const void* p1) { return ((HRESULT(__stdcall*)(void*, const void*))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall SetVolume(LONG p1) { return ((HRESULT(__stdcall*)(void*, LONG))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall SetPan(LONG p1) { return ((HRESULT(__stdcall*)(void*, LONG))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall SetFrequency(DWORD p1) { return ((HRESULT(__stdcall*)(void*, DWORD))((void**)m_pRealBuffer))(m_pRealBuffer, p1); }
    virtual HRESULT __stdcall Stop() { return ((HRESULT(__stdcall*)(void*))((void**)m_pRealBuffer))(m_pRealBuffer); }
    virtual HRESULT __stdcall Unlock(void* p1, DWORD p2, void* p3, DWORD p4) { return ((HRESULT(__stdcall*)(void*, void*, DWORD, void*, DWORD))((void**)m_pRealBuffer))(m_pRealBuffer, p1, p2, p3, p4); }
    virtual HRESULT __stdcall Restore() { return ((HRESULT(__stdcall*)(void*))((void**)m_pRealBuffer))(m_pRealBuffer); }

    // HIBRID FAJLLOPÓS LEJÁTSZÓ METÓDUS (Ezzel váltjuk ki a makacs monó keverőt!)
    virtual HRESULT __stdcall Play(DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) {
        char* pWavString = (char*)0x0095C918; // Statikus Webzen fájlnév puffer
        if (g_audio_ready && pWavString && pWavString[0] != '\0') {
            std::string fileName(pWavString);
            if (fileName.find("Data\\Sound\\") != std::string::npos) {
                short refX = 0, refY = 0;
                int targetMobIdx = 0;
                if (GetReferencePosition(refX, refY)) {
                    uintptr_t listPtr = (uintptr_t)OBJECT_LIST_BASE;
                    if (IsReadable((void*)listPtr, OBJECT_STRUCT_SIZE * 400)) {
                        float minDistance = 999.0f;
                        for (int i = 0; i < 400; i++) {
                            if (i == 1) continue;
                            uintptr_t entry = listPtr + (i * OBJECT_STRUCT_SIZE);
                            if (IsReadable((void*)entry, OBJECT_STRUCT_SIZE)) {
                                short mX = (short)(entry + 0xAC);
                                short mY = (short)(entry + 0xB0);
                                if (mX == 0 && mY == 0) continue;
                                float dist = sqrtf(powf((float)mX - (float)refX, 2.0f) + powf((float)mY - (float)refY, 2.0f));
                                if (dist < minDistance) {
                                    minDistance = dist;
                                    targetMobIdx = i;
                                }
                            }
                        }
                    }
                }
                // Lopjuk a hangot közvetlenül az XAudio2 3D térbe!
                Trigger3DEffectByFileName(pWavString, targetMobIdx); return 0; // DS_OK - Elnémítjuk a gyári szoftveres monó keverést
            }
        }
        typedef HRESULT(__stdcall* Play_t)(void*, DWORD, DWORD, DWORD);
        return ((Play_t)((void**)m_pRealBuffer))(m_pRealBuffer, dwReserved1, dwPriority, dwFlags);
    }
};

class MyDirectSound : public IUnknown {
public:
    IUnknown* m_pRealDS;
    MyDirectSound(IUnknown* pReal) { m_pRealDS = pReal; }
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override {
        return
            m_pRealDS->QueryInterface(riid, ppvObj);
    }
    ULONG __stdcall AddRef() override { return m_pRealDS->AddRef(); }
    ULONG __stdcall Release() override {
        ULONG res = m_pRealDS->Release();
        if (res == 0) { delete this; return 0; }
        return res;
    }
    virtual HRESULT __stdcall CreateSoundBuffer(const DSBUFFERDESC* pcDSBufferDesc, void** ppDSBuffer, IUnknown* pUnkOuter) {
        void* pRealBuffer = nullptr;
        HRESULT hr = ((HRESULT(__stdcall*)(void*, const DSBUFFERDESC*, void**, IUnknown*))((void**)m_pRealDS))(m_pRealDS, pcDSBufferDesc, &pRealBuffer, pUnkOuter);
        if (SUCCEEDED(hr) && pRealBuffer) {
            MyDirectSoundBuffer* myBuffer = new MyDirectSoundBuffer((IUnknown*)pRealBuffer, pcDSBufferDesc);
            *ppDSBuffer = (void*)myBuffer; return hr;
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
//  IMPORT ADDRESS TABLE (IAT) HOOKS
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
    if (!TrueDirectSoundCreate8) return 0x887800F0; void* pRealDS8 = nullptr;
    HRESULT hr = TrueDirectSoundCreate8(lpGuid, &pRealDS8, pUnkOuter);
    if (SUCCEEDED(hr) && pRealDS8) {
        MyDirectSound* myDS8 = new MyDirectSound((IUnknown*)pRealDS8);
        *ppDS8 = (void*)myDS8;
        return hr;
    }
    return hr;
}
void PatchImportAddressTable(const char* dllName, const char* functionName, DWORD newFunctionAddress) {
    HMODULE hModule = GetModuleHandleA(NULL); if (!hModule) return;
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
//  MCI HÁTTÉRZENE LEJÁTSZÓ ALRENDSZER
// ---------------------------------------------------------------------------
int GetMusicVolumeFromRegistry() {
    HKEY hKey; DWORD musicVolume = 9; DWORD dataSize = sizeof(musicVolume);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "VolumeLevel", NULL, NULL, (LPBYTE)&musicVolume, &dataSize);
        RegCloseKey(hKey);
    }
    int mciVolume = musicVolume * 111;
    if (mciVolume > 1000) mciVolume = 1000;
    if (mciVolume < 0) mciVolume = 0; return mciVolume;
}
void ApplyMCIVolume() {
    if (g_CurrentTrack == "") return;
    int volume = GetMusicVolumeFromRegistry();
    std::string volCmd = "setaudio my_mp3 volume to " + std::to_string(volume);
    mciSendStringA(volCmd.c_str(), NULL, 0, NULL);
}
std::string GetRandomMp3FromFolderIfNeeded(
    const std::string& originalPath, const std::string& lastPlayedTrack = "") {
    std::string folder_name = originalPath; size_t last_dot = folder_name.find_last_of(".");
    if (last_dot != std::string::npos) folder_name = folder_name.substr(0, last_dot);
    if (fs::exists(folder_name) && fs::is_directory(folder_name)) {
        std::vector<std::string> mp3_list;
        for (const auto& entry : fs::directory_iterator(folder_name)) {
            if (entry.path().extension() == ".mp3") mp3_list.push_back(entry.path().string());
        }
        if (!mp3_list.empty()) {
            if (mp3_list.size() == 1)
                return mp3_list[0];
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distr(0, mp3_list.size() - 1);
            std::string selected;
            do {
                selected = mp3_list[distr(gen)];
            } while (selected == lastPlayedTrack);
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
    if (uMsg == MM_MCINOTIFY && wParam == MCI_NOTIFY_SUCCESSFUL) {
        PlayNextRotatedTrack();
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
void CreateMCIHelperWindow() {
    if (g_HwndMCI != NULL) return;
    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = MCIWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MuWzAudioMCIHelper";
    RegisterClassExA(&wc);
    g_HwndMCI = CreateWindowExA(0, wc.lpszClassName, "MCI Helper", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
}
class IWzAudio {
public:
    virtual int  __thiscall GetStreamOffsetRange(int unk1, int unk2) = 0;
    virtual void __thiscall Play(const char* filePath, int volume, int unknown) = 0;
    virtual void __thiscall Stop() = 0;
    virtual void __thiscall SetVolume(int volume) = 0;
    virtual void __thiscall Option(int option, int value) = 0;
    virtual void __thiscall Destroy() = 0;
};
class CWzAudioImpl : public IWzAudio {
public:
    int __thiscall GetStreamOffsetRange(int unk1, int unk2) override { return 0; }
    void __thiscall Play(const char* filePath, int volume, int unknown) override {
        if (!filePath) return;
        HKEY hKey;
        DWORD musicOnOff = 1;
        DWORD dataSize = sizeof(musicOnOff);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "MusicOn", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize) != ERROR_SUCCESS)
                RegQueryValueExA(hKey, "MusicOnOff", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize);
            RegCloseKey(hKey);
        }
        if (musicOnOff == 0) { Stop(); return; }
        if (g_RequestedTrack == filePath) { ApplyMCIVolume(); return; }
        g_RequestedTrack = filePath;
        g_CurrentTrack = "";
        PlayNextRotatedTrack();
    }
    void __thiscall Stop() override {
        mciSendStringA("close my_mp3", NULL, 0, NULL);
        g_CurrentTrack = "";
        g_RequestedTrack = "";
    }
    void __thiscall SetVolume(int volume) override { ApplyMCIVolume(); }
    void __thiscall Option(int option, int value) override {}
    void __thiscall Destroy() override {}
};
CWzAudioImpl g_AudioInstance;

// ---------------------------------------------------------------------------
//  GYÁRI S5 / EX501 STUB EXPORTOK ÉS INICIALIZÁLÁS
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
extern "C" __declspec(dllexport) void* __cdecl wzAudioCreate() {
    mciSendStringA("close all", NULL, 0, NULL); if (!g_audio_ready) {
        if (SUCCEEDED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            if (SUCCEEDED(g_xaudio->CreateMasteringVoice(&g_master, 2, 44100, 0, NULL, NULL, AudioCategory_GameEffects))) {
                X3DAudioInitialize(SPEAKER_STEREO, X3DAUDIO_SPEED_OF_SOUND, g_x3d);
                g_audio_ready = true;
            }
        }
    }
    CreateMCIHelperWindow();
    CreateThread(NULL, 0, AudioWatcherThread, NULL, 0, NULL);
    // Indul az intelligens háttérszál
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
//  DLLMAIN CSATLAKOZÁSI PONT
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        InitLogger();
        HMODULE hDsound = GetModuleHandleA("dsound.dll");
        if (!hDsound) hDsound = LoadLibraryA("dsound.dll");
        if (hDsound) {
            TrueDirectSoundCreate = (DirectSoundCreate_t)GetProcAddress(hDsound, "DirectSoundCreate");
            TrueDirectSoundCreate8 = (DirectSoundCreate8_t)GetProcAddress(hDsound, "DirectSoundCreate8");
        }
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate", (DWORD)HookedDirectSoundCreate);
        PatchImportAddressTable("dsound.dll", "DirectSoundCreate8", (DWORD)HookedDirectSoundCreate8);
        LogWithTimestamp("[wzAudio_Init] Minden IAT Hook tiszta es biztonsagos.");
    }
    return TRUE;
}