// Prueba del chat de voz sin el juego: abre el micrófono 3 s, cuenta paquetes
// codificados y los reproduce en bucle local (eco) para comprobar la salida.
#include "voice.h"
#include <cstdio>
#include <atomic>
#include <cmath>
#include <string>
#include "adpcm.h"

int main() {
    for (auto& d : VoiceChat::ListDevices()) wprintf(L"  micro: %ls\n", d.c_str());
    {   // Calidad del códec: seno de 440 Hz codificado/decodificado por paquetes, como en la red
        const int n = proto::kVoiceFrame * 50;
        static int16_t in[n], out[n];
        static uint8_t packed[proto::kVoiceFrame / 2];
        for (int i = 0; i < n; ++i) in[i] = (int16_t)(9000 * sin(2 * 3.14159265 * 440 * i / proto::kVoiceRate));
        adpcm::State enc;
        for (int f = 0; f < n; f += proto::kVoiceFrame) {
            adpcm::State dec = enc;
            adpcm::Encode(enc, in + f, proto::kVoiceFrame, packed);
            adpcm::Decode(dec, packed, proto::kVoiceFrame, out + f);
        }
        double sig = 0, err = 0;
        for (int i = 0; i < n; ++i) { sig += double(in[i]) * in[i]; err += double(in[i] - out[i]) * (in[i] - out[i]); }
        printf("ADPCM SNR = %.1f dB\n", 10 * log10(sig / (err + 1)));
    }
    VoiceChat v;
    std::atomic<int> sent{0};
    float maxLevel = 0;
    v.SetThreshold(0.0f);  // transmitir siempre
    if (const char* d = getenv("MIC")) { std::string s(d); v.SetDevice(std::wstring(s.begin(), s.end())); }
    v.Start([&](const proto::MsgVoice& m) { ++sent; v.OnPacket(1, m); });
    v.SetGain(1, 0.0f, 0.0f);  // en silencio (solo comprobamos que la cadena funciona)
    v.SetMicEnabled(true);
    for (int i = 0; i < 30; ++i) {
        Sleep(100);
        if (v.MicLevel() > maxLevel) maxLevel = v.MicLevel();
    }
    printf("micro=%s error=%d paquetes=%d (esperados ~150) nivel_max=%.3f\n", v.MicEnabled() ? "on" : "off",
           (int)v.micError.load(), sent.load(), maxLevel);
    v.Stop();
    return sent > 100 ? 0 : 1;
}
