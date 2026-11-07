#pragma once
// Writes the shared key.json. Used only from the background thread.
// Plain POSIX with fixed buffers: no allocation and no locale, so a host set to comma decimals still gets valid JSON.
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include "KeyAnalyser.h"

namespace smoothkey
{
struct KeyState
{
    int key = 0;              // 0 to 11 from C
    bool minor = false;
    double confidence = 0.0;
    bool listening = false;
};

class KeyFile
{
public:
    // The real home folder from the user database, never HOME: a host may redirect that. False if it cannot be found.
    static bool defaultPath (char* out, size_t size) noexcept
    {
        struct passwd entry;
        struct passwd* result = nullptr;
        char buffer[4096];
        if (getpwuid_r (getuid(), &entry, buffer, sizeof buffer, &result) != 0 || result == nullptr || result->pw_dir == nullptr)
            return false;
        const int n = std::snprintf (out, size, "%s/Library/Application Support/SmoothPlugins/key.json", result->pw_dir);
        return n > 0 && (size_t) n < size;
    }

    // An empty path means the home folder could not be found: every write then counts as failed.
    void setPath (const char* newPath, double minSeconds = 0.5) noexcept
    {
        std::snprintf (path, sizeof path, "%s", newPath != nullptr ? newPath : "");
        minInterval = minSeconds;
        std::snprintf (tempPath, sizeof tempPath, "%s.tmp.%ld.%p", path, (long) getpid(), (void*) this);
    }

    const char* getPath() const noexcept { return path; }

    // Formats one line exactly as other plug-ins expect it. Returns its length.
    static int format (char* out, size_t size, const KeyState& s, double unixSeconds) noexcept
    {
        const int hundredths = (int) std::lround (std::fmin (1.0, std::fmax (0.0, s.confidence)) * 100.0);
        const long long tenths = std::llround (unixSeconds * 10.0);
        return std::snprintf (out, size, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":%d.%02d,\"time\":%lld.%lld,\"listening\":%s}",
                              keyNames[((s.key % 12) + 12) % 12], s.minor ? "minor" : "major",
                              hundredths / 100, hundredths % 100, tenths / 10, tenths % 10, s.listening ? "true" : "false");
    }

    // Call often. Writes only when something changed and enough time has passed. monotonic is any steady clock in seconds.
    void update (const KeyState& s, double monotonic, double unixSeconds) noexcept
    {
        const bool changed = ! hasWritten || s.key != written.key || s.minor != written.minor || s.listening != written.listening
                             || std::abs (s.confidence - written.confidence) >= 0.02
                             || (s.confidence == 0.0 && written.confidence != 0.0);
        if (! changed)
            return;
        const double wait = failed ? std::fmax (minInterval, 5.0) : minInterval;
        if (attempted && monotonic - lastAttempt < wait)
            return;
        attempted = true;
        lastAttempt = monotonic;
        if (writeNow (s, unixSeconds))
        {
            written = s;
            hasWritten = true;
            failed = false;
            ++writes;
        }
        else
        {
            failed = true;
            ++failures;
        }
    }

    // Writes regardless of the timer, for the last word as the plug-in closes.
    void flush (const KeyState& s, double unixSeconds) noexcept
    {
        if (writeNow (s, unixSeconds)) { written = s; hasWritten = true; failed = false; ++writes; }
        else { failed = true; ++failures; }
    }

    bool hasFailed() const noexcept { return failed; }
    bool everWritten() const noexcept { return hasWritten; }
    const KeyState& lastWritten() const noexcept { return written; }
    long writeCount() const noexcept { return writes; }
    long failureCount() const noexcept { return failures; }

private:
    bool makeFolders() const noexcept
    {
        char folder[PATH_MAX];
        std::snprintf (folder, sizeof folder, "%s", path);
        char* slash = std::strrchr (folder, '/');
        if (slash == nullptr || slash == folder) return false;
        *slash = 0;
        struct stat info;
        if (stat (folder, &info) == 0) return S_ISDIR (info.st_mode);
        for (char* p = folder + 1; *p != 0; ++p)
        {
            if (*p != '/') continue;
            *p = 0;
            if (mkdir (folder, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
        return mkdir (folder, 0755) == 0 || errno == EEXIST;
    }

    bool writeNow (const KeyState& s, double unixSeconds) const noexcept
    {
        if (path[0] == 0 || ! makeFolders())
            return false;
        char line[256];
        const int length = format (line, sizeof line, s, unixSeconds);
        if (length <= 0 || (size_t) length >= sizeof line)
            return false;
        // Whole file to a temporary name in the same folder, then rename: a reader sees the old file or the new one, never part.
        const int fd = open (tempPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return false;
        const bool wroteAll = write (fd, line, (size_t) length) == (ssize_t) length;
        const bool closed = close (fd) == 0;
        if (! wroteAll || ! closed || rename (tempPath, path) != 0)
        {
            unlink (tempPath);
            return false;
        }
        return true;
    }

    char path[PATH_MAX] {};
    char tempPath[PATH_MAX + 64] {};
    double minInterval = 0.5, lastAttempt = 0.0;
    bool attempted = false, hasWritten = false, failed = false;
    KeyState written;
    long writes = 0, failures = 0;
};
} // namespace smoothkey
