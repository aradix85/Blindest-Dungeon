// core/subtitles.cpp -- THE GAME'S OWN SUBTITLES, READ ALOUD.

#include <cstdio>
#include <cstring>
#include "internal.h"
#include "game/offsets.h"

static const uintptr_t SUB_TEXT_OFF    = 0x193c;
static const uintptr_t SUB_TEXT_MAX    = 0x200;   // the game's own strncpy_s bound
static const uintptr_t SUB_VISIBLE_OFF = 0x1bd8;

static char s_subLast[MAILBOX_SZ];

static bool s_subLogged = false;

// ---- THE LAST NARRATION: the chunks of one passage, kept so comma can read it again ----
static const int   SUB_PASSAGE_MAX    = 2048;
static const DWORD SUB_PASSAGE_GAP_MS = 6000;   // a longer silence starts a new passage
static char  s_passage[SUB_PASSAGE_MAX];
static DWORD s_passageTick = 0;

static void subRemember(const char* line) {
    DWORD now = GetTickCount();
    if (now - s_passageTick > SUB_PASSAGE_GAP_MS) s_passage[0] = 0;
    s_passageTick = now;
    size_t n = strlen(s_passage);
    _snprintf(s_passage + n, sizeof s_passage - n, "%s%s", n ? " " : "", line);
    s_passage[sizeof s_passage - 1] = 0;
}

bool subLastNarration(char* out, int outsz) {
    if (!out || outsz <= 0) return false;
    _snprintf(out, outsz, "%s", s_passage);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

static bool subIsMissSentinel(const char* s) {
    return (s[0] == '[' && s[1] == '<') || strstr(s, "error_not_localized") != nullptr;
}

static const uintptr_t SUB_MOVIE_OFF = 0x1cf0;
static const uintptr_t MOVIE_RANGES_BEGIN_OFF = 0x00;   // track+0x00/+0x08: vector<{uint start, uint end}>
static const uintptr_t MOVIE_RANGES_END_OFF   = 0x08;
static const uintptr_t MOVIE_NAME_OFF         = 0x64;   // inline track name (log only)
static const uintptr_t MOVIE_RANGE_BYTES      = 8;
static const uintptr_t MOVIE_RANGES_MAX       = 4096;   // a 57 s cutscene has ~40 lines
bool subMovieTrackUp(uintptr_t base) {
    if (!base) return false;
    uintptr_t movie = 0;
    if (!safeReadPtr(base + DARKEST_APP_RVA + SUB_MOVIE_OFF, &movie) || movie <= 0x10000) return false;
    uintptr_t rb = 0, re = 0;
    if (!safeReadPtr(movie + MOVIE_RANGES_BEGIN_OFF, &rb) ||
        !safeReadPtr(movie + MOVIE_RANGES_END_OFF, &re)) return false;
    if (re < rb || ((re - rb) % MOVIE_RANGE_BYTES) != 0 ||
        (re - rb) / MOVIE_RANGE_BYTES > MOVIE_RANGES_MAX) return false;
    return true;
}

// ---- THE CUTSCENE KEY GUARD ----
static bool   s_mvUp       = false;   // the last state axIsMovie observed (edge log)
static const char* const kOpeningMovie = "house_of_ruin";   // video\house_of_ruin.ogv, played at launch
// The player ignores a skip key in the first moments of the cinematic, so press Enter after a
// second and again every second while the track is still up.
static const DWORD INTRO_SKIP_RETRY_MS  = 1000;
static const int   INTRO_SKIP_MAX_TRIES = 8;
static bool  s_introSkipPending = false;
static int   s_introSkipTries   = 0;
static DWORD s_introSkipNextTry = 0;

static void serviceIntroSkip() {
    if (!s_introSkipPending || (long)(GetTickCount() - s_introSkipNextTry) < 0) return;
    if (s_introSkipTries >= INTRO_SKIP_MAX_TRIES) {
        s_introSkipPending = false;
        logLine("movie: the opening cinematic did not take the skip key -- giving up");
        return;
    }
    uint32_t scan = klScancodeForKey(SDLK_RETURN);
    enqueueSynthKeyAt(SDL_EVT_KEYDOWN, scan, SDLK_RETURN, 0, 0);
    enqueueSynthKeyAt(SDL_EVT_KEYUP,   scan, SDLK_RETURN, 0, 1);
    s_introSkipTries++;
    s_introSkipNextTry = GetTickCount() + INTRO_SKIP_RETRY_MS;
    logLine("movie: skip key %d of %d for the opening cinematic", s_introSkipTries, INTRO_SKIP_MAX_TRIES);
}
static int    s_mvFnSettings = -2;

bool axIsMovie() {
    const bool up = subMovieTrackUp(g_base);
    if (up != s_mvUp) {
        s_mvUp = up;
        if (up) {
            uintptr_t movie = 0, rb = 0, re = 0;
            char name[64] = "?";
            safeReadPtr(g_base + DARKEST_APP_RVA + SUB_MOVIE_OFF, &movie);
            safeReadPtr(movie + MOVIE_RANGES_BEGIN_OFF, &rb);
            safeReadPtr(movie + MOVIE_RANGES_END_OFF, &re);
            if (!safeReadCStr(movie + MOVIE_NAME_OFF, name, sizeof name)) strcpy(name, "?");
            logLine("movie: track UP -- %p, %u subtitle range(s), name \"%.40s\" -- plain keys but "
                    "Enter are held", (void*)movie, (unsigned)((re - rb) / MOVIE_RANGE_BYTES), name);
            // The opening cinematic plays at every launch: arm the skip once per session, when
            // the player asked for it (F10 > Announcements). serviceIntroSkip presses Enter.
            static bool s_introSeen = false;
            if (!s_introSeen && strcmp(name, kOpeningMovie) == 0) {
                s_introSeen = true;
                if (axSkipIntro()) {
                    s_introSkipPending = true;
                    s_introSkipTries   = 0;
                    s_introSkipNextTry = GetTickCount() + INTRO_SKIP_RETRY_MS;
                    logLine("movie: the opening cinematic will be skipped (skip_intro=1)");
                }
            }
        } else {
            logLine("movie: track down -- keys released to the game");
            s_introSkipPending = false;
        }
    }
    return up;
}

bool routeMovieKey(uintptr_t base, uint32_t sym, uint16_t mod, uint8_t repeat) {
    (void)base;
    if (mod & (KMOD_LALT | KMOD_RALT | KMOD_LCTRL | KMOD_RCTRL)) return false;   // Alt+F4 etc.
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) return false;                // the skip key
    if (s_mvFnSettings == -2) s_mvFnSettings = kmFind("general.settings");
    if (s_mvFnSettings >= 0 ? kmIsCurrentKey(s_mvFnSettings, sym, mod)
                            : (sym == SDLK_F10 && !(mod & (KMOD_LSHIFT | KMOD_RSHIFT)))) return false;
    if (repeat) return true;
    logLine("movie: swallowed key 0x%x (mod 0x%x) during the cutscene", sym, mod);
    return true;
}

void serviceSubtitles(uintptr_t base) {
    if (!base) return;
    serviceIntroSkip();                                 // per frame, subtitles on or off
    if (!axReadSubtitles()) { s_subLast[0] = 0; return; }

    uintptr_t app = base + DARKEST_APP_RVA;

    uint8_t visible = 0;
    if (!safeReadU8(app + SUB_VISIBLE_OFF, &visible)) return;
    if (!visible) { s_subLast[0] = 0; return; }      // gone -> re-arm; the buffer keeps stale text

    char raw[SUB_TEXT_MAX + 1];
    if (!safeReadCStr(app + SUB_TEXT_OFF, raw, (int)sizeof raw)) return;
    if (!raw[0] || subIsMissSentinel(raw)) return;

    char line[MAILBOX_SZ];
    stripMarkup(raw, line, sizeof line);
    if (!line[0]) return;
    if (strcmp(line, s_subLast) == 0) return;        // same chunk still up: nothing new to say

    strncpy(s_subLast, line, sizeof s_subLast - 1);
    s_subLast[sizeof s_subLast - 1] = 0;

    if (!s_subLogged) {
        s_subLogged = true;
        uintptr_t movie = 0;
        safeReadPtr(app + SUB_MOVIE_OFF, &movie);    // diagnostic only: narration vs cinematic
        logLine("subtitles: first line read (movie track %s): \"%s\"",
                movie ? "present" : "none", line);
    }

    subRemember(line);
    if (g_raidDisplay) clogAdd(line);                 // in a raid the "." log keeps the narrator too
    postSpeech(line, false, SPK_EVENT);
}
