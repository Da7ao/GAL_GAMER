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

bool CaptureWindow(HWND hWnd, const std::string& outputFileName);