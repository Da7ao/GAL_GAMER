#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cstring>
#include <comdef.h>

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

struct AudioCaptureContext {
    IAudioClient* pAudioClient;
    IAudioCaptureClient* pCaptureClient;
    WAVEFORMATEX* pWaveFormat;
    std::atomic<bool> isCapturing;
    std::vector<BYTE> audioBuffer;
    std::string outputFileName;
    DWORD targetProcessId;
};

bool SaveAudioToFile(const std::vector<BYTE>& audioData, const std::string& fileName, const WAVEFORMATEX* pWaveFormat);
bool InitializeAudioCapture(AudioCaptureContext& context, const std::string& outputFileName, DWORD targetProcessId);
void ContinuousAudioCapture(const std::string& baseFileName, DWORD processId, int durationMs = 10000);