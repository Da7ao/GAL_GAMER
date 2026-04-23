#include "screen_capture.h"
#include "audio_capture.h"
#include <atomic>
#include <mutex>
#include <thread>

struct WindowCandidate { HWND hwnd; int score; };
std::vector<WindowCandidate> candidates;

class CaptureManager {
public:
    CaptureManager() : captureLock(false) {}

    void StartCapture() {
        std::lock_guard<std::mutex> lock(lockMutex);
        captureLock.store(true);
    }

    void StopCapture() {
        std::lock_guard<std::mutex> lock(lockMutex);
        captureLock.store(false);
    }

    bool IsCapturing() const {
        return captureLock.load();
    }

private:
    std::atomic<bool> captureLock;
    std::mutex lockMutex;
};

CaptureManager captureManager;

void ScreenCaptureLoop(HWND hWnd, const std::string& baseFileName, int intervalMs) {
    int captureCount = 0;
    std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();

    while (captureManager.IsCapturing()) {
        if (!IsWindow(hWnd)) {
            std::cout << "Game window closed, stopping capture." << std::endl;
            break;
        }

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastTime).count();

        if (elapsed >= intervalMs) {
            char fileName[256];
            sprintf_s(fileName, sizeof(fileName), "%s_%04d.bmp", baseFileName.c_str(), captureCount++);

            if (CaptureWindow(hWnd, fileName)) {
                std::cout << "Captured frame " << captureCount << ": " << fileName << std::endl;
            }

            lastTime = currentTime;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void AudioCaptureLoop(DWORD processId, const std::string& baseFileName) {
    AudioCaptureContext context{};

    if (!InitializeAudioCapture(context, baseFileName, processId)) {
        std::cerr << "Failed to initialize audio capture!" << std::endl;
        return;
    }

    HRESULT hr = context.pAudioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "Failed to start audio capture! HRESULT: 0x" << std::hex << hr << std::endl;
        return;
    }

    int captureCount = 0;
    int loopCount = 0;
    size_t totalCaptured = 0;

    while (captureManager.IsCapturing()) {
        loopCount++;
        if (loopCount % 100 == 0) {
            std::cout << "Audio capture thread running, loop " << loopCount
                      << ", total captured: " << totalCaptured << " bytes" << std::endl;
        }

        UINT32 packetLength = 0;
        HRESULT hr = context.pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            std::cerr << "GetNextPacketSize failed! HRESULT: 0x" << std::hex << hr << std::endl;
            break;
        }

        // 循环读取所有可用的音频包
        while (packetLength > 0) {
            BYTE* pData = NULL;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            // 使用 CaptureClient::GetBuffer 读取音频数据
            hr = context.pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) {
                std::cerr << "CaptureClient::GetBuffer failed! HRESULT: 0x" << std::hex << hr << std::endl;
                break;
            }

            if (numFramesAvailable > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // 静音包：填充零数据
                    size_t silentSize = numFramesAvailable * context.pWaveFormat->nBlockAlign;
                    context.audioBuffer.insert(context.audioBuffer.end(), silentSize, 0);
                    totalCaptured += silentSize;
                } else {
                    // 实际音频数据
                    size_t dataSize = numFramesAvailable * context.pWaveFormat->nBlockAlign;
                    context.audioBuffer.insert(context.audioBuffer.end(),
                                               pData, pData + dataSize);
                    totalCaptured += dataSize;
                }
            }

            // 必须释放 buffer
            hr = context.pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) {
                std::cerr << "ReleaseBuffer failed! HRESULT: 0x" << std::hex << hr << std::endl;
                break;
            }

            // 定期保存（超过 4MB 时）
            if (context.audioBuffer.size() > 4 * 1024 * 1024) {
                std::string tempFileName = context.outputFileName + "_part.wav";
                if (SaveAudioToFile(context.audioBuffer, tempFileName, context.pWaveFormat)) {
                    context.audioBuffer.clear();
                }
            }

            hr = context.pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }

        // 10ms 休眠，与音频引擎周期同步
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    context.pAudioClient->Stop();
    std::cout << "Audio capture stopped" << std::endl;

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

BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    DWORD targetPid = static_cast<DWORD>(lParam);
 
    if (pid != targetPid) return TRUE;
 
    if (GetParent(hwnd) != nullptr) return TRUE; // 非顶级窗口
    if (!IsWindowVisible(hwnd)) return TRUE;     // 不可见
 
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != nullptr) return TRUE;           // 被其他窗口拥有
 
    char className[256];
    GetClassNameA(hwnd, className, sizeof(className));
    if (strcmp(className, "Shell_TrayWnd") == 0 ||
        strcmp(className, "WorkerW") == 0) {
        return TRUE; // 排除系统托盘类
    }
 
    char title[512];
    GetWindowTextA(hwnd, title, sizeof(title));
    int titleLen = strlen(title);
 
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
    EnumWindows(EnumWindowProc, processId);
 
    if (candidates.empty()) return nullptr;
 
    auto best = std::max_element(candidates.begin(), candidates.end(),
        [](const WindowCandidate& a, const WindowCandidate& b) {
            return a.score < b.score;
        });
 
    return best->hwnd;
}

int main() {
    std::string gamePath = "E:\\software\\gal\\ef - the first tale\\ef_first_cn.exe";
    std::string arguments = "-windowed";
    std::string base_dir = "./";
    std::string outputFileName = "capture_output";

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL result = CreateProcessA(
        gamePath.c_str(),
        const_cast<LPSTR>(arguments.c_str()),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );
    if (!result) {
        std::cerr << "Failed to start game! Error code: " << GetLastError() << std::endl;
        return 1;
    }
    std::cout << "Game started successfully! ProcessID: " << pi.dwProcessId << std::endl;

    std::cout << "Waiting for game window to appear..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    HWND hWnd = FindMainWindow(pi.dwProcessId);

    if (!hWnd) {
        std::cerr << "Failed to find game window!" << std::endl;
        return 1;
    }
    std::cout << "Game window found!" << std::endl;

    // 启动捕获管理器
    captureManager.StartCapture();
    std::cout << "Capture started. Press Ctrl+C to stop..." << std::endl;

    // 创建线程
    std::thread screenThread([&]() {
        ScreenCaptureLoop(hWnd, outputFileName, 500);
    });

    std::thread audioThread([&]() {
        AudioCaptureLoop(pi.dwProcessId, outputFileName);
    });

    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 停止捕获
    captureManager.StopCapture();
    std::cout << "Stopping capture..." << std::endl;

    // 等待线程完成
    screenThread.join();
    audioThread.join();

    std::cout << "Capture stopped. Cleaning up..." << std::endl;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}



// g++ capture.cpp screen_capture.cpp audio_capture.cpp -o capture.exe -lole32 -luuid -lavrt -lpsapi -luser32 -lwinmm -lgdi32 -std=c++17