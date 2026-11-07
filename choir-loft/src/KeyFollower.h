#pragma once
// Follow Key: reads the shared key file the Smooth Key plug-in writes. Background thread only, never the audio thread.
// Missing, broken, stale or low confidence data is ignored. The last good key is held, and if there has never been one the
// plug-in keeps using its own Key and Scale controls.
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

namespace smoothharmony
{
struct KeyFileData
{
    int key = 0;
    bool minor = false;
    double confidence = 0.0;
    double time = 0.0;
    bool listening = false;
};

class KeyFollower
{
public:
    static constexpr double minConfidence = 0.5;
    // Smooth Key only rewrites its file when something changes, so a held song can leave it minutes old and still right.
    static constexpr double maxAgeSeconds = 600.0;

    // The real home folder from the user database, never HOME, which a host may redirect.
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

    void setPath (const char* newPath) noexcept { std::snprintf (path, sizeof path, "%s", newPath != nullptr ? newPath : ""); }

    // Finds "name" : and returns a pointer just past the colon, or null. text is zero terminated.
    static const char* findField (const char* text, const char* name) noexcept
    {
        char quoted[32];
        std::snprintf (quoted, sizeof quoted, "\"%s\"", name);
        const char* at = std::strstr (text, quoted);
        if (at == nullptr) return nullptr;
        at += std::strlen (quoted);
        while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') ++at;
        if (*at != ':') return nullptr;
        ++at;
        while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') ++at;
        return at;
    }

    // Strict: any missing or strange field fails the whole file.
    static bool parse (const char* text, KeyFileData& d) noexcept
    {
        const char* k = findField (text, "key");
        const char* s = findField (text, "scale");
        const char* c = findField (text, "confidence");
        const char* t = findField (text, "time");
        if (k == nullptr || s == nullptr || c == nullptr || t == nullptr || *k != '"' || *s != '"')
            return false;

        ++k;
        static constexpr const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        static constexpr const char* flats[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
        int key = -1;
        for (int i = 0; i < 12 && key < 0; ++i)
        {
            const size_t a = std::strlen (names[i]), b = std::strlen (flats[i]);
            if ((std::strncmp (k, names[i], a) == 0 && k[a] == '"') || (std::strncmp (k, flats[i], b) == 0 && k[b] == '"'))
                key = i;
        }
        if (key < 0)
            return false;

        ++s;
        bool minor;
        if (std::strncmp (s, "major\"", 6) == 0) minor = false;
        else if (std::strncmp (s, "minor\"", 6) == 0) minor = true;
        else return false;

        char* end = nullptr;
        const double confidence = std::strtod (c, &end);
        if (end == c || ! std::isfinite (confidence) || confidence < 0.0 || confidence > 1.0)
            return false;
        const double time = std::strtod (t, &end);
        if (end == t || ! std::isfinite (time) || time <= 0.0)
            return false;

        d.key = key;
        d.minor = minor;
        d.confidence = confidence;
        d.time = time;
        const char* l = findField (text, "listening");
        d.listening = l != nullptr && std::strncmp (l, "true", 4) == 0;
        return true;
    }

    static bool usable (const KeyFileData& d, double nowUnix) noexcept
    {
        return d.confidence >= minConfidence && nowUnix - d.time <= maxAgeSeconds && d.time - nowUnix <= 60.0;
    }

    // Reads the file once. Returns true if it held a usable key, and publishes it.
    bool poll (double nowUnix) noexcept
    {
        if (path[0] == 0)
            return false;
        const int fd = open (path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            return false;
        char text[2048];
        const ssize_t got = read (fd, text, sizeof text - 1);
        close (fd);
        if (got <= 0)
            return false;
        text[got] = 0;
        KeyFileData d;
        if (! parse (text, d) || ! usable (d, nowUnix))
            return false;
        packed.store (d.key | (d.minor ? 16 : 0) | 32, std::memory_order_relaxed);
        return true;
    }

    void forget() noexcept { packed.store (0, std::memory_order_relaxed); }

    // Safe from the audio thread.
    bool has() const noexcept { return (packed.load (std::memory_order_relaxed) & 32) != 0; }
    int key() const noexcept { return packed.load (std::memory_order_relaxed) & 15; }
    bool minor() const noexcept { return (packed.load (std::memory_order_relaxed) & 16) != 0; }

private:
    char path[PATH_MAX] {};
    std::atomic<int> packed { 0 };
};
} // namespace smoothharmony
