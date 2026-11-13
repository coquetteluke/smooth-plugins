#pragma once
// Writes the shared key.json. Used only from the background thread.
// Plain POSIX with fixed buffers: no allocation and no locale, so a host set to comma decimals still gets valid JSON.
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include "KeyAnalyser.h"
#include "KeyPublish.h"

namespace smoothkey
{
struct KeyState
{
    int key = 0;              // 0 to 11 from C
    bool minor = false;
    double confidence = 0.0;
    bool listening = false;
    // Since 1.7.0, written after the fields above.
    int altKey = -1;          // the runner up, 0 to 11, or -1 for none
    bool altMinor = false;
    double altConfidence = 0.0;
    double margin = 1.0;      // 0 a tie, 1 no contest
    bool unsure = false;      // two sets of notes fit about equally, and the confidence is under 0.5
    bool locked = false;      // the key is held by Lock
    int source = 0;           // 0 heard, 1 manual, 2 locked
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

    // For tests only. When the environment variable SMOOTH_KEY_JSON holds an absolute path, the file goes there instead, so a
    // test run beside a live session cannot change the file that session's plug-ins follow. Not set, empty or not absolute:
    // false, and the default path is used exactly as before. Read once as the plug-in is made, never on the audio thread.
    static bool overridePath (char* out, size_t size) noexcept
    {
        const char* e = std::getenv ("SMOOTH_KEY_JSON");
        if (e == nullptr || e[0] != '/')
            return false;
        const int n = std::snprintf (out, size, "%s", e);
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

    // Room for the longest line there can be, with some to spare. A test checks the longest one fits.
    static constexpr size_t lineSize = 384;
    // A held key never changes, and a follower ignores a file older than ten minutes. So while a key is held the file is
    // written again this often, and a plug-in added later still finds a fresh one. Since 1.7.2 the same goes for every key that is
    // fixed on purpose: set by hand, or frozen with Listening off or Bypass.
    static constexpr double lockedRefreshSeconds = 240.0;

    // Formats one line. The first five fields are exactly as other plug-ins have always read them. The rest were added in
    // 1.7.0 and always come after: the runner up (null when there is none), how clearly the key leads it, whether the two
    // are too close to call, whether the key is held by Lock, and where the key came from. Returns its length.
    static int format (char* out, size_t size, const KeyState& s, double unixSeconds) noexcept
    {
        const int hundredths = toHundredths (s.confidence);
        const long long tenths = std::llround (unixSeconds * 10.0);
        const bool hasAlt = s.altKey >= 0;
        const int altHundredths = hasAlt ? toHundredths (s.altConfidence) : 0;
        const int marginHundredths = hasAlt ? toHundredths (s.margin) : 100;
        char alt[48];
        if (hasAlt) std::snprintf (alt, sizeof alt, "\"altKey\":\"%s\",\"altScale\":\"%s\"", keyNames[s.altKey % 12], s.altMinor ? "minor" : "major");
        else std::snprintf (alt, sizeof alt, "\"altKey\":null,\"altScale\":null");
        return std::snprintf (out, size, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":%d.%02d,\"time\":%lld.%lld,\"listening\":%s,"
                                         "%s,\"altConfidence\":%d.%02d,\"margin\":%d.%02d,\"unsure\":%s,\"locked\":%s,\"source\":\"%s\"}",
                              keyNames[((s.key % 12) + 12) % 12], s.minor ? "minor" : "major",
                              hundredths / 100, hundredths % 100, tenths / 10, tenths % 10, s.listening ? "true" : "false",
                              alt, altHundredths / 100, altHundredths % 100, marginHundredths / 100, marginHundredths % 100,
                              s.unsure ? "true" : "false", s.locked ? "true" : "false", sourceNames[s.source >= 0 && s.source <= 2 ? s.source : 0]);
    }

    // Call often. Writes only when something changed and enough time has passed. monotonic is any steady clock in seconds.
    // atOnce skips the wait between writes (not the wait after a failure), for a bounce that runs faster than real time.
    void update (const KeyState& s, double monotonic, double unixSeconds, bool atOnce = false) noexcept
    {
        const bool sameAlt = (s.altKey < 0 && written.altKey < 0) || (s.altKey == written.altKey && s.altMinor == written.altMinor);
        const bool changed = ! hasWritten || s.key != written.key || s.minor != written.minor || s.listening != written.listening
                             || std::abs (s.confidence - written.confidence) >= 0.02
                             || (s.confidence == 0.0 && written.confidence != 0.0)
                             || ! sameAlt || s.unsure != written.unsure || s.locked != written.locked || s.source != written.source
                             || (s.altKey >= 0 && (std::abs (s.altConfidence - written.altConfidence) >= 0.05 || std::abs (s.margin - written.margin) >= 0.05));
        const bool fixed = s.locked || s.source == 1 || ! s.listening;
        const bool refresh = hasWritten && fixed && monotonic - lastWrite >= lockedRefreshSeconds;
        // After a failure it tries again even if the state went back to the line on disk, so the failure does not stay up for good.
        if (! changed && ! refresh && ! failed)
            return;
        const double wait = failed ? std::fmax (minInterval, 5.0) : (atOnce ? 0.0 : minInterval);
        if (attempted && monotonic - lastAttempt < wait)
            return;
        attempted = true;
        lastAttempt = monotonic;
        if (writeNow (s, unixSeconds))
        {
            written = s;
            hasWritten = true;
            failed = false;
            lastWrite = monotonic;
            ++writes;
        }
        else
        {
            failed = true;
            ++failures;
        }
    }

    // Writes regardless of the timer.
    void flush (const KeyState& s, double unixSeconds) noexcept
    {
        if (writeNow (s, unixSeconds)) { written = s; hasWritten = true; failed = false; ++writes; }
        else { failed = true; ++failures; }
    }

    // The last word as the plug-in closes: the line it last wrote, now saying it no longer listens, with the time that line had.
    // Only while key.json still holds that line, so a Key that closes never writes over a newer line from another Key, and never
    // makes an old key look new to a follower. Since 1.7.2; before, it wrote its last line again stamped with the time of closing.
    void close() noexcept
    {
        if (! hasWritten || written.listening == false || ! stillHolds())
            return;
        KeyState s = written;
        s.listening = false;
        flush (s, writtenUnix);
    }

    // Whether key.json still holds exactly the line this one last wrote. Reads the file.
    bool stillHolds() const noexcept
    {
        if (! hasWritten || lastLineLength <= 0)
            return false;
        const int fd = open (path, O_RDONLY);
        if (fd < 0)
            return false;
        char text[lineSize + 1];
        const ssize_t n = read (fd, text, sizeof text);
        ::close (fd);
        return n == (ssize_t) lastLineLength && std::memcmp (text, lastLine, (size_t) lastLineLength) == 0;
    }

    // The Unix time of the line last written, as it is in the file.
    double lastWrittenTime() const noexcept { return writtenUnix; }

    bool hasFailed() const noexcept { return failed; }
    bool everWritten() const noexcept { return hasWritten; }
    const KeyState& lastWritten() const noexcept { return written; }
    long writeCount() const noexcept { return writes; }
    long failureCount() const noexcept { return failures; }

private:
    static int toHundredths (double v) noexcept { return (int) std::lround (std::fmin (1.0, std::fmax (0.0, v)) * 100.0); }

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

    bool writeNow (const KeyState& s, double unixSeconds) noexcept
    {
        if (path[0] == 0 || ! makeFolders())
            return false;
        char line[lineSize];
        const int length = format (line, sizeof line, s, unixSeconds);
        if (length <= 0 || (size_t) length >= sizeof line)
            return false;
        // Whole file to a temporary name in the same folder, then rename: a reader sees the old file or the new one, never part.
        const int fd = open (tempPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return false;
        const bool wroteAll = write (fd, line, (size_t) length) == (ssize_t) length;
        const bool closed = ::close (fd) == 0;
        if (! wroteAll || ! closed || rename (tempPath, path) != 0)
        {
            unlink (tempPath);
            return false;
        }
        std::memcpy (lastLine, line, (size_t) length);
        lastLineLength = length;
        writtenUnix = (double) std::llround (unixSeconds * 10.0) / 10.0;   // as the line says it
        return true;
    }

    char path[PATH_MAX] {};
    char tempPath[PATH_MAX + 64] {};
    double minInterval = 0.5, lastAttempt = 0.0, lastWrite = 0.0;
    bool attempted = false, hasWritten = false, failed = false;
    KeyState written;
    long writes = 0, failures = 0;
    char lastLine[lineSize] {};
    int lastLineLength = 0;
    double writtenUnix = 0.0;
};
} // namespace smoothkey
