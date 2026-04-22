// Copyright 2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <iostream>
#include <stdexcept>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Extended console output support
// Uses virtual terminal sequences to implement functionality beyond standard
// console output. For more information see documentation at
// https://docs.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
class Console
{
public:
    Console()
    {
        // Get the standard output handle.
        m_hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (m_hStdOut == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to get console handle");
        }

        // Save the current output mode, to be restored on exit.
        if (!GetConsoleMode(m_hStdOut, &m_oldConsoleMode)) {
            throw std::runtime_error("Failed to get console mode");
        }

        // Enable virtual terminal processing
        const auto newMode = m_oldConsoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
        if (!SetConsoleMode(m_hStdOut, newMode)) {
            throw std::runtime_error("Failed to enable virtual terminal mode");
        }

        // Hide cursor
        hideCursor();
    }

    ~Console()
    {
        // Restore cursor
        showCursor();

        // Restore old mode
        if (m_hStdOut != INVALID_HANDLE_VALUE) {
            SetConsoleMode(m_hStdOut, m_oldConsoleMode);
        }
    }

    static void hideCursor()
    {
        constexpr char cmd[7] = {'\x1b', '[', '?', '2', '5', 'l', 0};
        std::cout << cmd;
    }

    static void showCursor()
    {
        constexpr char cmd[7] = {'\x1b', '[', '?', '2', '5', 'h', 0};
        std::cout << cmd;
    }

    static void clearScreen()
    {
        constexpr char cmd[5] = {'\x1b', '[', '2', 'J', 0};
        std::cout << cmd;
    }

    static void clearFromCursorToEndOfLine()
    {
        constexpr char cmd[5] = {'\x1b', '[', '0', 'K', 0};
        std::cout << cmd;
    }

    static void moveToLine(size_t lineNumber) { std::cout << "\x1b[" << lineNumber << "H"; }

    static void writeLine(size_t lineNumber, const std::string& text)
    {
        moveToLine(lineNumber);
        std::cout << text;
        clearFromCursorToEndOfLine();
    }

    static void writeLines(size_t lineNumber, const std::initializer_list<std::string>& texts)
    {
        for (const auto& text : texts) {
            writeLine(lineNumber++, text);
        }
    }

private:
    HANDLE m_hStdOut = INVALID_HANDLE_VALUE;
    DWORD m_oldConsoleMode;
};
