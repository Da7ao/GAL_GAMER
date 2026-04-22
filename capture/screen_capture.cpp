#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <thread>

struct WindowCandidate {
    HWND hwnd;
    int score;
};
 
std::vector<WindowCandidate> candidates;
 
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

// 捕获指定窗口的截图并保存为BMP文件
bool CaptureWindow(HWND hWnd, const std::string& outputFileName) {
    // 获取窗口区域
    RECT rect;
    // 改用 GetClientRect（永远不会返回 0 尺寸）
    if (!GetClientRect(hWnd, &rect)) {
        std::cerr << "Failed to get window rect! Error: " << GetLastError() << std::endl;
        return false;
    }

    // 转换为屏幕真实坐标（解决窗口偏移问题）
    POINT tl = { rect.left, rect.top };
    POINT br = { rect.right, rect.bottom };
    ClientToScreen(hWnd, &tl);
    ClientToScreen(hWnd, &br);

    // 计算真实宽高
    int width = br.x - tl.x;
    int height = br.y - tl.y;

    if (width <= 0 || height <= 0) {
        std::cerr << "Invalid window dimensions!" << std::endl;
        return false;
    }

    // 创建设备上下文
    HDC hDC = GetDC(hWnd);
    if (!hDC) {
        std::cerr << "Failed to get device context! Error: " << GetLastError() << std::endl;
        return false;
    }

    // 创建兼容的内存设备上下文
    HDC hMemDC = CreateCompatibleDC(hDC);
    if (!hMemDC) {
        std::cerr << "Failed to create compatible DC! Error: " << GetLastError() << std::endl;
        ReleaseDC(hWnd, hDC);
        return false;
    }

    // 创建兼容位图
    HBITMAP hBitmap = CreateCompatibleBitmap(hDC, width, height);
    if (!hBitmap) {
        std::cerr << "Failed to create compatible bitmap! Error: " << GetLastError() << std::endl;
        DeleteDC(hMemDC);
        ReleaseDC(hWnd, hDC);
        return false;
    }

    // 选择位图到内存DC
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);

    // 复制窗口内容到位图
    if (!BitBlt(hMemDC, 0, 0, width, height, hDC, 0, 0, SRCCOPY)) {
        std::cerr << "Failed to copy window content! Error: " << GetLastError() << std::endl;
        SelectObject(hMemDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(hWnd, hDC);
        return false;
    }

    // 选择回原来的位图
    SelectObject(hMemDC, hOldBitmap);

    // 保存为BMP文件
    BITMAPINFOHEADER bi;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = height;  // 正高度表示从上到下扫描
    bi.biPlanes = 1;
    bi.biBitCount = 24;     // 24位真彩色
    bi.biCompression = BI_RGB;
    bi.biSizeImage = 0;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    // 计算每行字节数（必须是4的倍数）
    int bytesPerLine = ((width * 3 + 3) / 4) * 4;

    // 创建文件
    std::ofstream outFile(outputFileName, std::ios::binary);
    if (!outFile) {
        std::cerr << "Failed to create output file!" << std::endl;
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(hWnd, hDC);
        return false;
    }

    // 写入BMP文件头
    outFile.write("BM", 2);  // 标识符
    int fileSize = 54 + bytesPerLine * height;
    outFile.write(reinterpret_cast<const char*>(&fileSize), 4);  // 文件大小
    outFile.write("\0\0\0\0", 4);  // 保留
    outFile.write(reinterpret_cast<const char*>(&bi.biSize), 4);  // 偏移量
    outFile.write(reinterpret_cast<const char*>(&bi.biSize), 4);  // 信息头大小
    outFile.write(reinterpret_cast<const char*>(&width), 4);    // 宽度
    outFile.write(reinterpret_cast<const char*>(&height), 4);   // 高度
    outFile.write(reinterpret_cast<const char*>(&bi.biPlanes), 2);  // 平面数
    outFile.write(reinterpret_cast<const char*>(&bi.biBitCount), 2); // 位深度
    outFile.write(reinterpret_cast<const char*>(&bi.biCompression), 4); // 压缩方式
    outFile.write(reinterpret_cast<const char*>(&bi.biSizeImage), 4); // 图像大小
    outFile.write(reinterpret_cast<const char*>(&bi.biXPelsPerMeter), 4); // 水平分辨率
    outFile.write(reinterpret_cast<const char*>(&bi.biYPelsPerMeter), 4); // 垂直分辨率
    outFile.write(reinterpret_cast<const char*>(&bi.biClrUsed), 4);  // 使用颜色数
    outFile.write(reinterpret_cast<const char*>(&bi.biClrImportant), 4); // 重要颜色数

    // 读取位图数据
    std::vector<BYTE> buffer(bytesPerLine * height);
    GetDIBits(hDC, hBitmap, 0, height, buffer.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    // 写入像素数据
    outFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());

    // 清理资源
    outFile.close();
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(hWnd, hDC);

    std::cout << "Screenshot saved to: " << outputFileName << std::endl;
    return true;
}

// 连续截图功能
void ContinuousCapture(HWND hWnd, const std::string& baseFileName, int intervalMs = 1000) {
    int captureCount = 0;
    std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();

    while (true) {
        // 检查窗口是否仍然存在
        if (!IsWindow(hWnd)) {
            std::cout << "Game window closed, stopping capture." << std::endl;
            break;
        }

        // 计算时间间隔
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastTime).count();

        if (elapsed >= intervalMs) {
            // 生成文件名
            char fileName[256];
            sprintf_s(fileName, sizeof(fileName), "%s_%04d.bmp", baseFileName.c_str(), captureCount++);

            // 捕获窗口
            if (CaptureWindow(hWnd, fileName)) {
                std::cout << "Captured frame " << captureCount << ": " << fileName << std::endl;
            }

            lastTime = currentTime;
        }

        // 短暂休眠以减少CPU使用
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main() {
    std::string gamePath = "E:\\software\\gal\\ef - the first tale\\ef_first_cn.exe";
    std::string arguments = "-windowed";
    std::string outputFileName = "game_screenshot";

    // 启动游戏并获取进程信息
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

    // 等待游戏窗口出现
    std::cout << "Waiting for game window to appear..." << std::endl;


    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    HWND hWnd = FindMainWindow(pi.dwProcessId);

    if (!hWnd) {
        std::cerr << "Failed to find game window!" << std::endl;
        return 1;
    }
    std::cout << "Game window found!" << std::endl;


    // 开始连续截图
    std::cout << "Starting continuous capture with 100ms interval..." << std::endl;
    ContinuousCapture(hWnd, outputFileName, 1000);

    std::cout << "Capture stopped. Cleaning up..." << std::endl;

    // 关闭进程和线程句柄
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}