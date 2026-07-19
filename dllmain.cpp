#include "pch.h"
#include <windows.h>
#include <string>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

std::string g_CurrentTrack = "";
int g_CurrentVolume = 1000; // Alapértelmezett maximális hangerő (MCI-nél 1000 a max)

// GYÁRI WEBZEN VTABLE SORREND
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
        if (g_CurrentTrack == filePath) return;

        mciSendStringA("close my_mp3", NULL, 0, NULL);
        g_CurrentTrack = filePath;

        std::string openCmd = "open \"" + g_CurrentTrack + "\" type mpegvideo alias my_mp3";
        if (mciSendStringA(openCmd.c_str(), NULL, 0, NULL) == 0)
        {
            mciSendStringA("play my_mp3 repeat", NULL, 0, NULL);

            // Ha elindult a zene, azonnal beállítjuk rá az aktuális hangerőt is
            std::string volCmd = "setaudio my_mp3 volume to " + std::to_string(g_CurrentVolume);
            mciSendStringA(volCmd.c_str(), NULL, 0, NULL);
        }
    }

    void __thiscall Stop() override {
        mciSendStringA("close my_mp3", NULL, 0, NULL);
        g_CurrentTrack = "";
    }

    // A hangerő csúszka kezelése
    void __thiscall SetVolume(int volume) override {
        // A játékból érkező értéket (általában 0-9 vagy 0-15) felskálázzuk az MCI 0-1000-es tartományára
        // Ha pl. 0-9-ig megy, a volume * 100 tökéletes (900-as hangerő).
        if (volume < 0) volume = 0;

        g_CurrentVolume = volume * 100;
        if (g_CurrentVolume > 1000) g_CurrentVolume = 1000;

        // Alkalmazzuk a hangerőt a futó zenére
        std::string volCmd = "setaudio my_mp3 volume to " + std::to_string(g_CurrentVolume);
        mciSendStringA(volCmd.c_str(), NULL, 0, NULL);
    }

    // A tálcára csukás (Option) kezelése
    void __thiscall Option(int option, int value) override {
        // Ha az opció elnémítást vagy háttérbe kényszerítést kér (value == 0 vagy hasonló)
        if (value == 0) {
            mciSendStringA("pause my_mp3", NULL, 0, NULL);
        }
        else {
            mciSendStringA("resume my_mp3", NULL, 0, NULL);
        }
    }

    void __thiscall Destroy() override {}
};

CWzAudioImpl g_AudioInstance;

// EXPORTÁLT FÜGGVÉNYEK (Visszatérési értékek javítva int-re a fagyások ellen)
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

extern "C" __declspec(dllexport) int __cdecl wzAudioDestroy() { g_AudioInstance.Destroy(); return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioOption(int option, int value) { g_AudioInstance.Option(option, value); return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioSetVolume(int volume) { g_AudioInstance.SetVolume(volume); return 1; }
extern "C" __declspec(dllexport) int __cdecl wzAudioGetStreamOffsetRange(int unk1, int unk2) { return g_AudioInstance.GetStreamOffsetRange(unk1, unk2); }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
