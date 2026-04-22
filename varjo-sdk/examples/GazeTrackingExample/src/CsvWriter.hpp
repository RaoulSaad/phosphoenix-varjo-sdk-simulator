// Copyright 2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <chrono>
#include <fstream>
#include <string>

// Helper class for writing CSV files
class CsvWriter
{
public:
    CsvWriter(const std::filesystem::path& filename, const std::string& separator = ",")
        : m_outputStream(filename, std::ios::out)
        , m_separator(separator)
    {
    }

    template <typename... T>
    void outputLine(T... items)
    {
        output(items...);
        m_outputStream << "\n";
    }

private:
    void output(const char* item) { m_outputStream << item; }
    void output(const std::string& item) { m_outputStream << item; }
    void output(const double v[3]) { output(v[0], v[1], v[2]); }

    void output(const std::chrono::system_clock::time_point& timepoint)
    {
        const auto millisecPart = std::chrono::duration_cast<std::chrono::milliseconds>(timepoint.time_since_epoch()).count() % 1000;
        const auto timePart = std::chrono::system_clock::to_time_t(timepoint);
        std::tm tm;
        localtime_s(&tm, &timePart);

        m_outputStream << std::setfill('0') << std::setw(2) << tm.tm_hour << ":" << std::setw(2) << tm.tm_min << ":" << std::setw(2) << tm.tm_sec << "."
                       << std::setw(3) << millisecPart;
    }

    template <typename T>
    void output(T item)
    {
        m_outputStream << std::to_string(item);
    }

    template <typename T, typename... Trest>
    void output(T item, Trest... items)
    {
        output(item);
        output(m_separator);
        output(items...);
    }

    std::fstream m_outputStream;
    const std::string m_separator;
};
