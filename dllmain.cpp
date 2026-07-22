#include "pch.h"
#include <windows.h>
#include <string>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

std::string g_CurrentTrack = "";

// REGISTRY OLVASÓ: Lekéri a Launcher által mentett egyedi zenei hangerőt
int GetMusicVolumeFromRegistry() {
    HKEY hKey;
	DWORD musicVolume = 9; // changed from 5 (mid) to MuOnline standard 9 (high)
    DWORD dataSize = sizeof(musicVolume);

    // Megnyitjuk a Registry-t
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Webzen\\Mu\\Config", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        // Kiolvassuk a Launcher egyedi zenei hangerő kulcsát
        RegQueryValueExA(hKey, "VolumeLevel", NULL, NULL, (LPBYTE)&musicVolume, &dataSize);
        RegCloseKey(hKey);
    }

    // A launcher 0-9 közötti értéket ad le. Felskálázzuk az MCI 0-1000 skálájára.
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
    // Az IGCN kliens a DirectSound init ellenőrzésekor ezt a függvényt hívja meg, 
    // hogy lekérdezze a hangfolyam puffereit. Ha ide DS_OK (0) értéket adunk vissza,
    // a kliens azt hiszi, hogy a DirectSound sikeresen elindult!
    int __thiscall GetStreamOffsetRange(int unk1, int unk2) override {
        return DS_OK; // A vak 1-es helyett fixen DS_OK (0)-t adunk vissza!
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

        // Ha a Registry szerint le van némítva (0), akkor leállítjuk a jelenlegi zenét és kilépünk
        if (musicOnOff == 0) {
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

    void __thiscall Stop() override { mciSendStringA("close my_mp3", NULL, 0, NULL); g_CurrentTrack = ""; }
    void __thiscall SetVolume(int volume) override { ApplyMCIVolume(); }

    // Ha a kliens az Option-ön keresztül próbálná konfigurálni a DirectSound-ot,
    // biztosítjuk, hogy ne kapjon hibaüzenetet a stacket illetően.
    void __thiscall Option(int option, int value) override {
        // Üresen hagyjuk, nem engedjük, hogy a gyári hibás DirectSound kód felülírja az MCI-t
    }

    void __thiscall Destroy() override {}
};

CWzAudioImpl g_AudioInstance;

// EXPORTÁLT FÜGGVÉNYEK
extern "C" __declspec(dllexport) void* __cdecl wzAudioCreate() {
    mciSendStringA("close all", NULL, 0, NULL);
    return &g_AudioInstance; // Visszaadjuk az objektum mutatóját, ez kötelező a kliensnek!
}

extern "C" __declspec(dllexport) int __cdecl wzAudioPlay(const char* filePath, int volume, int unknown) {
    g_AudioInstance.Play(filePath, volume, unknown);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl wzAudioStop() {
    g_AudioInstance.Stop();
    return 1;
}

// Az inicializációs ellenőrzések fix SIKERES (1) visszatérést kapnak!
extern "C" __declspec(dllexport) int __cdecl wzAudioDestroy() { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioOption(int option, int value) { return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSetVolume(int volume) {
    g_AudioInstance.SetVolume(volume); // Itt is kényszerítjük a saját hangerőnket!
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

// Lekéri az aktuális hangerőt (visszaadjuk a registry-ből olvasott értéket, vagy fix 5-öt)
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


