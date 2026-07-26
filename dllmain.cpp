#include "pch.h"
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <thread>
#include <atomic>
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace fs = std::filesystem;

// GLOBÁLIS VÁLTOZÓK
std::string g_CurrentTrack = "";       // A konkrét fájl, ami épp szól (pl. "Data\\Music\\Lorencia\\01.mp3")
std::string g_RequestedTrack = "";     // Az eredeti név, amit a kliens kért (pl. "Data\\Music\\Lorencia.mp3")
std::atomic<bool> g_ThreadRunning = false;
std::thread g_MusicMonitorThread;

// REGISTRY OLVASÓ: Lekéri a Launcher által mentett egyedi zenei hangerőt
int GetMusicVolumeFromRegistry() {
    HKEY hKey;
    DWORD musicVolume = 9; // MuOnline standard 9 (high)
    DWORD dataSize = sizeof(musicVolume);

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "VolumeLevel", NULL, NULL, (LPBYTE)&musicVolume, &dataSize);
        RegCloseKey(hKey);
    }

    int mciVolume = musicVolume * 111;
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

// INTELLIGENS VÉLETLENSZERŰSÍTŐ: Kezeli a mappákat és kiszűri a duplikációt
std::string GetRandomMp3FromFolderIfNeeded(const std::string& originalPath, const std::string& lastPlayedTrack = "") {
    std::string folder_name = originalPath;
    size_t last_dot = folder_name.find_last_of(".");
    if (last_dot != std::string::npos) {
        folder_name = folder_name.substr(0, last_dot);
    }

    if (fs::exists(folder_name) && fs::is_directory(folder_name)) {
        std::vector<std::string> mp3_list;

        for (const auto& entry : fs::directory_iterator(folder_name)) {
            if (entry.path().extension() == ".mp3") {
                mp3_list.push_back(entry.path().string());
            }
        }

        if (!mp3_list.empty()) {
            // Ha csak 1 fájl van a mappában, nincs mit variálni
            if (mp3_list.size() == 1) {
                return mp3_list[0];
            }

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distr(0, mp3_list.size() - 1);

            // Addig sorsolunk újra, amíg az előző daltól el nem térünk
            std::string selected;
            do {
                selected = mp3_list[distr(gen)];
            } while (selected == lastPlayedTrack);

            return selected;
        }
    }

    return originalPath;
}

// HÁTTÉRSZÁL ROUTINE: Figyeli a szám végét és duplikáció-mentesen újat indít
void MusicMonitorRoutine() {
    char statusMode[32];

    while (g_ThreadRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // fél másodpercenként ellenőriz

        if (g_CurrentTrack.empty()) continue;

        // Lekérjük az MCI aktuális állapotát
        ZeroMemory(statusMode, sizeof(statusMode));
        mciSendStringA("status my_mp3 mode", statusMode, sizeof(statusMode) - 1, NULL);

        // Ha a státusz "stopped", vagyis organikusan véget ért a zeneszám
        if (strcmp(statusMode, "stopped") == 0) {
            mciSendStringA("close my_mp3", NULL, 0, NULL);

            // Kérünk egy ÚJ random számot, átadva a g_CurrentTrack-et, hogy azt SKIP-elje
            std::string nextTrack = GetRandomMp3FromFolderIfNeeded(g_RequestedTrack, g_CurrentTrack);
            g_CurrentTrack = nextTrack;

            std::string openCmd = "open \"" + g_CurrentTrack + "\" type mpegvideo alias my_mp3";
            if (mciSendStringA(openCmd.c_str(), NULL, 0, NULL) == 0) {
                mciSendStringA("play my_mp3", NULL, 0, NULL); // Nincs repeat kapcsoló, a szál léptet
                ApplyMCIVolume();
            }
        }
    }
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

#ifndef DS_OK
#define DS_OK 0
#endif

class CWzAudioImpl : public IWzAudio {
public:
    int __thiscall GetStreamOffsetRange(int unk1, int unk2) override {
        return DS_OK;
    }

    void __thiscall Play(const char* filePath, int volume, int unknown) override {
        if (!filePath) return;

        // Ellenőrizzük, hogy a zene egyáltalán be van-e kapcsolva a Registry-ben
        HKEY hKey;
        DWORD musicOnOff = 1;
        DWORD dataSize = sizeof(musicOnOff);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "MusicOn", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize) != ERROR_SUCCESS) {
                RegQueryValueExA(hKey, "MusicOnOff", NULL, NULL, (LPBYTE)&musicOnOff, &dataSize);
            }
            RegCloseKey(hKey);
        }

        if (musicOnOff == 0) {
            Stop();
            return;
        }

        // Ha a kliens ugyanazt a zónát (placeholder fájlt) kéri, ami már fut, nem szakítjuk meg
        if (g_RequestedTrack == filePath) {
            ApplyMCIVolume();
            return;
        }

        // Új zónára váltás (teleportálás) esetén takarítunk
        mciSendStringA("close my_mp3", NULL, 0, NULL);

        g_RequestedTrack = filePath;
        g_CurrentTrack = GetRandomMp3FromFolderIfNeeded(g_RequestedTrack, ""); // Első lejátszáskor bármi jöhet

        std::string openCmd = "open \"" + g_CurrentTrack + "\" type mpegvideo alias my_mp3";
        if (mciSendStringA(openCmd.c_str(), NULL, 0, NULL) == 0) {
            mciSendStringA("play my_mp3", NULL, 0, NULL);
            ApplyMCIVolume();
        }
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

// EXPORTÁLT FÜGGVÉNYEK
extern "C" __declspec(dllexport) void* __cdecl wzAudioCreate() {
    mciSendStringA("close all", NULL, 0, NULL);

    // Biztonságos háttérszál indítás inicializáláskor
    if (!g_ThreadRunning) {
        g_ThreadRunning = true;
        g_MusicMonitorThread = std::thread(MusicMonitorRoutine);
    }

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

extern "C" __declspec(dllexport) int __cdecl wzAudioDestroy() {
    // Háttérszál leállítása a DLL megsemmisülésekor
    if (g_ThreadRunning) {
        g_ThreadRunning = false;
        if (g_MusicMonitorThread.joinable()) {
            g_MusicMonitorThread.join();
        }
    }
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl wzAudioOption(int option, int value) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSetVolume(int volume) {
    g_AudioInstance.SetVolume(volume);
    return 1;
}
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamOffsetRange(int unk1, int unk2) { return 1; }


// =========================================================================
// AZ ÚJONNAN FELFEDEZETT HIÁNYZÓ EXPORTÁLT FÜGGVÉNYEK STUBJAI
// =========================================================================

// Visszaadja a hangfolyam információit (0 = sikeres/nincs hiba)
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamInfo(int unk1, int unk2) {
    return 0;
}

// Visszaadja a folyam eltolódását másodpercben
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamOffsetSec(int unk1) {
    return 0;
}

// Lekéri az aktuális hangerőt (visszaadjuk a registry-ből olvasott értéket, vagy fix 9-et)
extern "C" __declspec(dllexport) int __cdecl wzAudioGetVolume() {
    return GetMusicVolumeFromRegistry();
}

// Fájlmegnyitási stub, az MCI open intézi helyette, így itt csak sikert (1) adunk vissza
extern "C" __declspec(dllexport) int __cdecl wzAudioOpenFile(const char* filePath) {
    return 1;
}

// Zene szüneteltetése (opcionális, ha a kliens hívná, mci paranccsal leállítható)
extern "C" __declspec(dllexport) int __cdecl wzAudioPause() {
    mciSendStringA("pause my_mp3", NULL, 0, NULL);
    return 1;
}

// Pozicionálás a zenében
extern "C" __declspec(dllexport) int __cdecl wzAudioSeek(int position) {
    return 1;
}

// Hangszínszabályzó (Equalizer) beállítása (0 = kikapcsolva/alapértelmezett)
extern "C" __declspec(dllexport) int __cdecl wzAudioSetEqualizer(int eqMode) {
    return 0;
}

// !!! A LEGGYANÚSABB BŰNÖS !!! Keverő mód beállítása.
// Ha ezt nem találja vagy hibát ad, azt hiszi, nincs hardver. Fix 1-et (sikeres) adunk vissza!
extern "C" __declspec(dllexport) int __cdecl wzAudioSetMixerMode(int mode) {
    return 1;
}

// Hangerő csökkentése billentyűkombinációra
extern "C" __declspec(dllexport) int __cdecl wzAudioVolumeDown() {
    return 1;
}

// Hangerő növelése billentyűkombinációra
extern "C" __declspec(dllexport) int __cdecl wzAudioVolumeUp() {
    return 1;
}


