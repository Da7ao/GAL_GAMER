#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
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
#include <comdef.h>  // 添加 _bstr_t 支持

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#define HAS_AUDIOCLIENTACTIVATIONPARAMS_HEADER 1
#endif

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#ifndef HAS_AUDIOCLIENTACTIVATIONPARAMS_HEADER
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;
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

class AudioActivationCompletionHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    AudioActivationCompletionHandler() : refCount_(1), completedEvent_(CreateEvent(nullptr, FALSE, FALSE, nullptr)),
                                         activateResult_(E_FAIL), audioClient_(nullptr) {}

    ~AudioActivationCompletionHandler() override {
        if (completedEvent_) CloseHandle(completedEvent_);
        if (audioClient_) audioClient_->Release();
    }

    HRESULT WaitForCompletion(DWORD timeoutMs, IAudioClient** outAudioClient) {
        if (!completedEvent_) return E_FAIL;
        DWORD waitResult = WaitForSingleObject(completedEvent_, timeoutMs);
        if (waitResult != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(waitResult == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError());
        if (FAILED(activateResult_)) return activateResult_;
        if (!audioClient_) return E_FAIL;
        *outAudioClient = audioClient_;
        (*outAudioClient)->AddRef();
        return S_OK;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount_); }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&refCount_);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        if (!operation) return E_POINTER;
        IUnknown* activatedInterface = nullptr;
        HRESULT hr = operation->GetActivateResult(&activateResult_, &activatedInterface);
        if (SUCCEEDED(hr) && SUCCEEDED(activateResult_) && activatedInterface) {
            activatedInterface->QueryInterface(IID_PPV_ARGS(&audioClient_));
        }
        if (activatedInterface) activatedInterface->Release();
        SetEvent(completedEvent_);
        return S_OK;
    }

private:
    LONG refCount_;
    HANDLE completedEvent_;
    HRESULT activateResult_;
    IAudioClient* audioClient_;
};

bool SaveAudioToFile(const std::vector<BYTE>& audioData, const std::string& fileName, const WAVEFORMATEX* pWaveFormat) {
    struct WAVHeader {
        char riff[4];
        uint32_t fileSize;
        char wave[4];
        char fmt[4];
        uint32_t fmtSize;
        uint16_t audioFormat;
        uint16_t numChannels;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample;
        char data[4];
        uint32_t dataSize;
    };

    WAVHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.riff, "RIFF", 4);
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmt, "fmt ", 4);
    memcpy(header.data, "data", 4);

    // 注意：loopback 捕获到的是 IEEE_FLOAT 格式（32位浮点），
    // 这里直接写原始格式，audioFormat 用 3 (WAVE_FORMAT_IEEE_FLOAT)
    // 如果 pWaveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE，需要检查 SubFormat
    header.fmtSize = 16;
    header.audioFormat = (pWaveFormat->wBitsPerSample == 32) ? 3 : 1; // 3=IEEE_FLOAT, 1=PCM
    header.numChannels = pWaveFormat->nChannels;
    header.sampleRate = pWaveFormat->nSamplesPerSec;
    header.byteRate = pWaveFormat->nAvgBytesPerSec;
    header.blockAlign = pWaveFormat->nBlockAlign;
    header.bitsPerSample = pWaveFormat->wBitsPerSample;
    header.dataSize = (uint32_t)audioData.size();
    header.fileSize = 36 + header.dataSize;

    std::ofstream outFile(fileName, std::ios::binary);
    if (!outFile) {
        std::cerr << "Failed to create audio output file!" << std::endl;
        return false;
    }
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    outFile.write(reinterpret_cast<const char*>(audioData.data()), audioData.size());
    outFile.close();
    std::cout << "Saved WAV file: " << fileName << " (" << audioData.size() << " bytes)" << std::endl;
    return true;
}

// 关键修复：使用 Process Loopback，只捕获目标进程（及其子进程）音频
bool InitializeAudioCapture(AudioCaptureContext& context, const std::string& outputFileName, DWORD targetProcessId) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        std::cerr << "COM is already initialized with a different threading model." << std::endl;
        return false;
    }
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM! HRESULT: 0x" << std::hex << hr << std::endl;
        return false;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = targetProcessId;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateVariant;
    PropVariantInit(&activateVariant);
    activateVariant.vt = VT_BLOB;
    activateVariant.blob.cbSize = sizeof(activationParams);
    activateVariant.blob.pBlobData = reinterpret_cast<BYTE*>(CoTaskMemAlloc(sizeof(activationParams)));
    if (!activateVariant.blob.pBlobData) {
        std::cerr << "Failed to allocate activation params blob!" << std::endl;
        CoUninitialize();
        return false;
    }
    memcpy(activateVariant.blob.pBlobData, &activationParams, sizeof(activationParams));

    auto* completionHandler = new AudioActivationCompletionHandler();
    if (!completionHandler) {
        PropVariantClear(&activateVariant);
        CoUninitialize();
        return false;
    }

    IActivateAudioInterfaceAsyncOperation* asyncOperation = nullptr;
    hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateVariant,
        completionHandler,
        &asyncOperation);
    PropVariantClear(&activateVariant);
    if (FAILED(hr)) {
        std::cerr << "ActivateAudioInterfaceAsync failed! HRESULT: 0x" << std::hex << hr << std::endl;
        completionHandler->Release();
        CoUninitialize();
        return false;
    }

    hr = completionHandler->WaitForCompletion(5000, &context.pAudioClient);
    if (asyncOperation) asyncOperation->Release();
    completionHandler->Release();
    if (FAILED(hr)) {
        std::cerr << "Process-loopback activation failed! HRESULT: 0x" << std::hex << hr << std::endl;
        CoUninitialize();
        return false;
    }

    hr = context.pAudioClient->GetMixFormat(&context.pWaveFormat);
    if (FAILED(hr)) {
        std::cerr << "Failed to get mix format! HRESULT: 0x" << std::hex << hr << std::endl;
        context.pAudioClient->Release();
        CoUninitialize();
        return false;
    }

    std::cout << "Audio format: " << std::dec << context.pWaveFormat->nChannels << " channels, "
              << context.pWaveFormat->nSamplesPerSec << " Hz, "
              << context.pWaveFormat->wBitsPerSample << " bits" << std::endl;

    hr = context.pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        10000000,  // 缓冲区时长：1秒（单位100纳秒）
        0,
        context.pWaveFormat,
        NULL);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize audio client with loopback! HRESULT: 0x" << std::hex << hr << std::endl;
        CoTaskMemFree(context.pWaveFormat);
        context.pAudioClient->Release();
        CoUninitialize();
        return false;
    }

    hr = context.pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&context.pCaptureClient);
    if (FAILED(hr)) {
        std::cerr << "Failed to get capture client! HRESULT: 0x" << std::hex << hr << std::endl;
        CoTaskMemFree(context.pWaveFormat);
        context.pAudioClient->Release();
        CoUninitialize();
        return false;
    }

    context.outputFileName = outputFileName;
    context.targetProcessId = targetProcessId;
    context.isCapturing = false;
    context.audioBuffer.clear();

    std::cout << "Audio capture initialized successfully (Process Loopback mode)!" << std::endl;
    return true;
}

bool StartAudioCapture(AudioCaptureContext& context) {
    HRESULT hr = context.pAudioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "Failed to start audio capture! HRESULT: 0x" << std::hex << hr << std::endl;
        return false;
    }
    context.isCapturing = true;
    std::cout << "Audio capture started" << std::endl;
    return true;
}

void StopAudioCapture(AudioCaptureContext& context) {
    if (context.isCapturing) {
        context.pAudioClient->Stop();
        context.isCapturing = false;
    }
}

// ✅ 修复后的捕获线程：使用 IAudioCaptureClient 正确读取数据
DWORD WINAPI AudioCaptureThread(LPVOID lpParam) {
    AudioCaptureContext* pContext = reinterpret_cast<AudioCaptureContext*>(lpParam);

    std::cout << "Audio capture thread started" << std::endl;
    std::cout << "Initial isCapturing state: " << pContext->isCapturing << std::endl;

    int loopCount = 0;
    size_t totalCaptured = 0;

    while (pContext->isCapturing) {
        loopCount++;
        if (loopCount % 100 == 0) {
            std::cout << "Audio capture thread running, loop " << loopCount
                      << ", total captured: " << totalCaptured << " bytes" << std::endl;
        }

        UINT32 packetLength = 0;
        HRESULT hr = pContext->pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            std::cerr << "GetNextPacketSize failed! HRESULT: 0x" << std::hex << hr << std::endl;
            break;
        }

        // 循环读取所有可用的音频包
        while (packetLength > 0) {
            BYTE* pData = NULL;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            // ✅ 使用 CaptureClient::GetBuffer 读取音频数据
            hr = pContext->pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) {
                std::cerr << "CaptureClient::GetBuffer failed! HRESULT: 0x" << std::hex << hr << std::endl;
                break;
            }

            if (numFramesAvailable > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // 静音包：填充零数据
                    size_t silentSize = numFramesAvailable * pContext->pWaveFormat->nBlockAlign;
                    pContext->audioBuffer.insert(pContext->audioBuffer.end(), silentSize, 0);
                    totalCaptured += silentSize;
                } else {
                    // 实际音频数据
                    size_t dataSize = numFramesAvailable * pContext->pWaveFormat->nBlockAlign;
                    pContext->audioBuffer.insert(pContext->audioBuffer.end(),
                                                 pData, pData + dataSize);
                    totalCaptured += dataSize;
                }
            }

            // ✅ 必须释放 buffer
            hr = pContext->pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) {
                std::cerr << "ReleaseBuffer failed! HRESULT: 0x" << std::hex << hr << std::endl;
                break;
            }

            // 定期保存（超过 4MB 时）
            if (pContext->audioBuffer.size() > 4 * 1024 * 1024) {
                std::string tempFileName = pContext->outputFileName + "_part.wav";
                if (SaveAudioToFile(pContext->audioBuffer, tempFileName, pContext->pWaveFormat)) {
                    pContext->audioBuffer.clear();
                }
            }

            hr = pContext->pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }

        // 10ms 休眠，与音频引擎周期同步
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << std::dec << "Audio capture thread stopping after " << loopCount
              << " loops, total captured: " << totalCaptured << " bytes" << std::endl;
    return 0;
}

// ---- 窗口查找（保持不变）----

struct WindowCandidate { HWND hwnd; int score; };
std::vector<WindowCandidate> candidates;

BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != static_cast<DWORD>(lParam)) return TRUE;
    if (GetParent(hwnd) != nullptr) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    char className[256];
    GetClassNameA(hwnd, className, sizeof(className));
    if (strcmp(className, "Shell_TrayWnd") == 0 || strcmp(className, "WorkerW") == 0) return TRUE;
    char title[512];
    GetWindowTextA(hwnd, title, sizeof(title));
    int titleLen = (int)strlen(title);
    int score = 0;
    if (titleLen > 0) score += 3;
    if (titleLen > 10) score += 2;
    if (IsZoomed(hwnd)) score += 2;
    if (GetForegroundWindow() == hwnd) score += 3;
    candidates.push_back({hwnd, score});
    return TRUE;
}

HWND FindMainWindow(DWORD processId) {
    candidates.clear();
    EnumWindows(EnumWindowProc, (LPARAM)processId);
    if (candidates.empty()) return nullptr;
    auto best = std::max_element(candidates.begin(), candidates.end(),
        [](const WindowCandidate& a, const WindowCandidate& b) { return a.score < b.score; });
    return best->hwnd;
}

void ContinuousAudioCapture(const std::string& baseFileName, DWORD processId, int durationMs = 10000) {
    AudioCaptureContext context;
    memset(&context, 0, sizeof(context));  // 确保指针初始化为 NULL

    if (!InitializeAudioCapture(context, baseFileName, processId)) {
        std::cerr << "Failed to initialize audio capture!" << std::endl;
        return;
    }

    if (!StartAudioCapture(context)) {
        std::cerr << "Failed to start audio capture!" << std::endl;
        return;
    }

    std::cout << "Starting audio capture for " << durationMs << " milliseconds" << std::endl;

    HANDLE hThread = CreateThread(NULL, 0, AudioCaptureThread, &context, 0, NULL);
    if (!hThread) {
        std::cerr << "Failed to create capture thread!" << std::endl;
        StopAudioCapture(context);
        return;
    }

    std::cout << "Capture thread created successfully" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));

    StopAudioCapture(context);
    std::cout << "Audio capture stopped" << std::endl;

    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    if (!context.audioBuffer.empty()) {
        std::string finalFileName = baseFileName + ".wav";
        SaveAudioToFile(context.audioBuffer, finalFileName, context.pWaveFormat);
    } else {
        std::cout << "No audio data captured (game may not have been playing any audio)" << std::endl;
    }

    // 清理资源
    if (context.pCaptureClient) context.pCaptureClient->Release();
    if (context.pAudioClient)   context.pAudioClient->Release();
    if (context.pWaveFormat)    CoTaskMemFree(context.pWaveFormat);
    CoUninitialize();
}

int main() {
    std::string gamePath = "E:\\software\\gal\\ef - the first tale\\ef_first_cn.exe";
    std::string arguments = "-windowed";
    std::string outputFileName = "game_audio";

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL result = CreateProcessA(
        gamePath.c_str(),
        const_cast<LPSTR>(arguments.c_str()),
        NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

    if (!result) {
        std::cerr << "Failed to start game! Error code: " << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "Game started successfully! ProcessID: " << pi.dwProcessId << std::endl;

    std::cout << "Waiting for game window to appear..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));  // 等待久一些让游戏初始化
    HWND hWnd = FindMainWindow(pi.dwProcessId);

    if (!hWnd) {
        std::cerr << "Failed to find game window!" << std::endl;
        return 1;
    }
    std::cout << "Game window found!" << std::endl;

    std::cout << "Starting continuous audio capture..." << std::endl;
    ContinuousAudioCapture(outputFileName, pi.dwProcessId, 10000);

    std::cout << "Audio capture finished. Cleaning up..." << std::endl;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}
