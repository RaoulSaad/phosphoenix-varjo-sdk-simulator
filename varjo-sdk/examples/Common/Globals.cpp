
#include "Globals.hpp"

namespace
{
constexpr VarjoExamples::LogLevel c_defaultLogLevel = VarjoExamples::LogLevel::Info;
    // Log level
VarjoExamples::LogLevel g_logLevel = c_defaultLogLevel;

    // Optional logging function to allow e.g. UI logging.
VarjoExamples::LogFunc g_logFunc = nullptr;
}

void VarjoExamples::initLog(LogFunc logFunc, LogLevel logLevel)
{
    g_logFunc = logFunc;
    g_logLevel = logLevel;
}

void VarjoExamples::deinitLog()
{
    g_logFunc = nullptr;
    g_logLevel = c_defaultLogLevel;
}

void VarjoExamples::writeLog(LogLevel level, const std::string& line)
{
    if (g_logLevel < level) {
        return;
    }

    // Always write to stdout
    {
        // auto stream = stderr;
        auto stream = stdout;
        fprintf(stream, line.c_str());
        fprintf(stream, "\n");
        fflush(stream);
    }

    // If log function defined, call it as well
    if (g_logFunc) {
        g_logFunc(level, line);
    }

    if (level == LogLevel::Critical) {
        std::terminate();
    }
}

void VarjoExamples::writeLog(LogLevel level, const char* funcName, int lineNum, const char* prefix, const char* format, ...)
{
    if (g_logLevel < level) {
        return;
    }

    constexpr size_t lineLimit = 4096;
    char lineBuf[lineLimit];
    va_list args;
    std::string formatStr;
    // formatStr  += std::string(funcName) + "():" + std::to_string(lineNum) + ": ";
    formatStr += std::string(prefix) + format;
    va_start(args, format);
    vsprintf_s(lineBuf, lineLimit, formatStr.data(), args);
    va_end(args);

    writeLog(level, std::string(lineBuf));
}

[[noreturn]] void VarjoExamples::writeCritical(const char* funcName, int lineNum, const char* prefix, const char* format, ...)
{
    constexpr size_t lineLimit = 4096;
    char lineBuf[lineLimit];
    va_list args;
    const std::string formatStr = std::string(prefix) + format;
    va_start(args, format);
    vsprintf_s(lineBuf, lineLimit, formatStr.data(), args);
    va_end(args);

    // calls std::terminate()
    writeLog(LogLevel::Critical, std::string(lineBuf));
}
