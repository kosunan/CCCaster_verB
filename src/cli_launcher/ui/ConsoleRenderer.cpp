#include "cli_launcher/ui/ConsoleRenderer.hpp"
#include <iostream>
#include <iomanip>
#include <conio.h>
#include <windows.h>

namespace {
// ANSI Color Codes
constexpr const char *ANSI_COLOR_RESET = "\x1b[0m";
constexpr const char *ANSI_COLOR_CYAN_BOLD = "\x1b[1;36m";
constexpr const char *ANSI_COLOR_GREEN = "\x1b[32m";
constexpr const char *ANSI_COLOR_YELLOW = "\x1b[33m";
constexpr const char *ANSI_COLOR_RED = "\x1b[31m";
constexpr const char *ANSI_BG_CYAN = "\x1b[46;30m";

// Windows Native Clear Screen to avoid ANSI dependence for clearing
void NativeClearScreen() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE)
        return;
    COORD coordScreen = {0, 0};
    DWORD cCharsWritten;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD dwConSize;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi))
        return;
    dwConSize = csbi.dwSize.X * csbi.dwSize.Y;
    if (!FillConsoleOutputCharacterA(hConsole, ' ', dwConSize, coordScreen, &cCharsWritten))
        return;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi))
        return;
    if (!FillConsoleOutputAttribute(hConsole, csbi.wAttributes, dwConSize, coordScreen, &cCharsWritten))
        return;
    SetConsoleCursorPosition(hConsole, coordScreen);
}
} // namespace

namespace cccaster::main_app::ui {

void ConsoleRenderer::EnableVirtualTerminalProcessing() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE)
        return;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode))
        return;
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);

    // Prevent console resize from user dragging edge to avoid layout wrap breaks
    HWND consoleWindow = GetConsoleWindow();
    if (consoleWindow) {
        SetWindowLongPtr(consoleWindow, GWL_STYLE,
                         GetWindowLongPtr(consoleWindow, GWL_STYLE) & ~WS_MAXIMIZEBOX & ~WS_SIZEBOX);
    }
}

void ConsoleRenderer::ClearScreen() {
    NativeClearScreen();
}

void ConsoleRenderer::PrintHeader() {
    std::cout << ANSI_COLOR_CYAN_BOLD << "╔══════════════════════════════════════════════════════╗\n"
              << "║            CCCaster verB 1.2 - Rollback Engine        ║\n"
              << "╚══════════════════════════════════════════════════════╝\n"
              << ANSI_COLOR_RESET;
}

std::optional<std::string> ConsoleRenderer::GetTextInputWithCancel(const std::string &prompt,
                                                                   const std::string &prefill,
                                                                   bool allowCancel) {
    std::string input = prefill;
    while (true) {
        ClearScreen();
        PrintHeader();

        std::cout << "\n  " << prompt << "\n";
        std::cout << "  (Type letters/numbers | ENTER to confirm" << (allowCancel ? " | ESC to cancel" : "")
                  << ")\n\n";
        std::cout << "  > " << input;

        int key = _getch();
        if (key == 0 || key == 224) {
            _getch(); // ignore special keys
            continue;
        }

        if (key == '\r' || key == '\n') {
            return input; // 空Enterの場合は空文字列""を返す（nulloptではない）
        } else if (key == 27 && allowCancel) {
            return std::nullopt;  // ESCキャンセル
        } else if (key == '\b') { // Backspace
            if (!input.empty()) {
                input.pop_back();
            }
        } else if (key >= 32 && key <= 126) { // Printable chars
            input += static_cast<char>(key);
        }
    }
}

int ConsoleRenderer::DrawMenuAndGetSelection(const std::string &title,
                                             const std::vector<std::string> &options, bool allowCancel) {
    if (options.empty())
        return -1;

    int selectedIndex = 0;
    while (true) {
        ClearScreen();
        PrintHeader();

        std::cout << "\n  " << title << "\n";
        std::cout << "  (UP/DOWN arrows to navigate | ENTER to select"
                  << (allowCancel ? " | ESC to go back" : "") << ")\n\n";

        for (size_t i = 0; i < options.size(); ++i) {
            if (static_cast<int>(i) == selectedIndex) {
                std::cout << "  " << ANSI_BG_CYAN << "  " << options[i] << "  " << ANSI_COLOR_RESET << "\n";
            } else {
                std::cout << "    " << options[i] << "\n";
            }
        }
        std::cout << "\n";

        int key = _getch();
        if (key == 0 || key == 224) {
            key = _getch();
            if (key == 72) {
                selectedIndex = (selectedIndex - 1 + options.size()) % options.size();
            } else if (key == 80) {
                selectedIndex = (selectedIndex + 1) % options.size();
            }
        } else {
            if (key == '\r' || key == '\n') {
                return selectedIndex;
            } else if (key == 27 && allowCancel) {
                return -1;
            } else if (key >= '1' && key < '1' + static_cast<int>(options.size())) {
                return key - '1';
            }
        }
    }
}

void ConsoleRenderer::PrintNetworkStatus(bool isConnected, uint32_t packetsReceived, uint32_t packetsSent,
                                         double currentPingMs, double currentJitterMs, double lossRate) {
    std::cout << "\x1b[2K\r"; // Clear current line before redrawing status
    if (isConnected) {
        const char *pingColor = ANSI_COLOR_GREEN;
        if (currentPingMs >= 50.0)
            pingColor = ANSI_COLOR_RED;
        else if (currentPingMs >= 30.0)
            pingColor = ANSI_COLOR_YELLOW;

        std::cout << "[NETWORK] Connected | Rx:" << std::setw(3) << packetsReceived << " Tx:" << std::setw(3)
                  << packetsSent << " pps | " << pingColor << "Ping: " << std::fixed << std::setprecision(1)
                  << currentPingMs << "ms " << ANSI_COLOR_RESET << "| Jitter: " << std::setw(4) << std::fixed
                  << std::setprecision(1) << currentJitterMs << "ms "
                  << "| Loss: " << std::setw(4) << std::fixed << std::setprecision(1) << lossRate << "%"
                  << std::flush;
    }
}

} // namespace cccaster::main_app::ui
