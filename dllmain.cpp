#include "pch.h"
#include <windows.h>
#include <string>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

std::string g_CurrentTrack = "";

// REGISTRY OLVASÓ: Lekéri a Launcher által mentett egyedi zenei hangerőt
int GetMusicVolumeFromRegistry() {
    HKEY hKey;
    DWORD volumeValue = 5; // Alapértelmezett érték (közepes hangerő), ha nincs mentés
    DWORD dataSize = sizeof(volumeValue);

    // Megnyitjuk a képen látható pontos Registry útvonalat
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        // Kiolvassuk a közös VolumeLevel kulcsot
        RegQueryValueExA(hKey, "VolumeLevel", NULL, NULL, (LPBYTE)&volumeValue, &dataSize);
        RegCloseKey(hKey);
    }

    // Mivel a játék skálája 0-4 vagy 0-9 között mozog (a képen a 4-es érték van maxon vagy középen),
    // biztonsági játékot játszunk: ha 4 a max, akkor 250-nel szorozzuk, ha 9 a max, akkor 111-gyel.
    // Teszteljük le úgy, hogy a kapott értéket felskálázzuk az MCI 0-1000 skálájára.

    int mciVolume = 0;
    if (volumeValue <= 4) {
        mciVolume = volumeValue * 250; // Ha 4-es skálát használ a kliens (4 * 250 = 1000)
    }
    else {
        mciVolume = volumeValue * 111; // Ha 9-es skálát használ a kliens (9 * 111 = 999)
    }

    if (mciVolume > 1000) mciVolume = 1000;
    if (mciVolume < 0) mciVolume = 0;

    return mciVolume;
}

// Alkalmazza a kiszámolt hangerőt a futó MP3-ra
void ApplyMCIVolume() {
    if (g_CurrentTrack == "") return;

    int volume = GetMusicVolumeFromRegistry();
    std::string volCmd = "setaudio my_mp3 volume to " + std::to_string(volume);
    mciSendStringA(volCmd.c_str(), NULL, 0, NULL);
}

// SZIGORÚ WEBZEN VTABLE SORREND
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
    int __thiscall GetStreamOffsetRange(int unk1, int unk2) override { return 1; }

    void __thiscall Play(const char* filePath, int volume, int unknown) override {
        if (!filePath) return;

        // Ellenőrizzük, hogy a zene egyáltalán be van-e kapcsolva a Registry-ben
        HKEY hKey;
        DWORD musicOn = 1;
        DWORD dataSize = sizeof(musicOn);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "MusicOn", NULL, NULL, (LPBYTE)&musicOn, &dataSize) != ERROR_SUCCESS) {
                RegQueryValueExA(hKey, "MusicOnOff", NULL, NULL, (LPBYTE)&musicOn, &dataSize);
            }
            RegCloseKey(hKey);
        }

        // Ha a Registry szerint le van némítva (0), akkor leállítjuk a jelenlegi zenét és kilépünk
        if (musicOn == 0) {
            mciSendStringA("close my_mp3", NULL, 0, NULL);
            g_CurrentTrack = "";
            return;
        }

        if (g_CurrentTrack == filePath) {
            ApplyMCIVolume();
            return;
        }

        mciSendStringA("close my_mp3", NULL, 0, NULL);
        g_CurrentTrack = filePath;

        std::string openCmd = "open \"" + g_CurrentTrack + "\" type mpegvideo alias my_mp3";
        if (mciSendStringA(openCmd.c_str(), NULL, 0, NULL) == 0)
        {
            mciSendStringA("play my_mp3 repeat", NULL, 0, NULL);
            ApplyMCIVolume();
        }
    }


    void __thiscall Stop() override {
        mciSendStringA("close my_mp3", NULL, 0, NULL);
        g_CurrentTrack = "";
    }

    // Ha a kliens belsőleg bántaná a hangerőt, mi felülbíráljuk a sajátunkkal
    void __thiscall SetVolume(int volume) override { ApplyMCIVolume(); }
    void __thiscall Option(int option, int value) override {}
    void __thiscall Destroy() override {}
};

CWzAudioImpl g_AudioInstance;

// EXPORTÁLT FÜGGVÉNYEK
extern "C" __declspec(dllexport) void* __cdecl wzAudioCreate() {
    mciSendStringA("close all", NULL, 0, NULL);
    return &g_AudioInstance;
}

extern "C" __declspec(dllexport) int __cdecl wzAudioPlay(const char* filePath, int volume, int unknown) {
    g_AudioInstance.Play(filePath, volume, unknown);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl wzAudioStop() {
    g_AudioInstance.Stop();
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl wzAudioDestroy() { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioOption(int option, int value) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSetVolume(int volume) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamOffsetRange(int unk1, int unk2) { return 1; }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
