#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#ifndef VITAMANIA_NO_AVPLAYER
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/sysmodule.h>
#endif

#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <malloc.h>
#include <string>
#include <utility>
#include <vector>

extern "C" {
typedef struct stb_vorbis stb_vorbis;
typedef struct {
    char *alloc_buffer;
    int alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;
typedef struct {
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
} stb_vorbis_info;
stb_vorbis *stb_vorbis_open_filename(const char *filename, int *error, const stb_vorbis_alloc *alloc);
stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error, const stb_vorbis_alloc *alloc);
stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
int stb_vorbis_seek(stb_vorbis *f, unsigned int sample_number);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
void stb_vorbis_close(stb_vorbis *f);
}

namespace {

constexpr int SCREEN_W = 960;
constexpr int SCREEN_H = 544;
constexpr int LANE_COUNT = 4;

constexpr const char *DATA_DIR = "ux0:data/vitamania";
constexpr const char *SONGS_DIR = "ux0:data/vitamania/Songs";
constexpr const char *SCORES_PATH = "ux0:data/vitamania/scores.tsv";
constexpr const char *SKIN_DIR = "app0:skin";
constexpr int PREVIEW_START_DELAY_MS = 120;
constexpr int COUNTDOWN_MS = 3000;
constexpr int START_LEAD_IN_MS = 1800;
constexpr int END_DEADSPACE_MS = 1000;
constexpr float PI_F = 3.14159265358979323846f;

struct BindPreset {
    const char *name;
    uint32_t buttons[LANE_COUNT];
};

const BindPreset BIND_PRESETS[] = {
    {"L/DN/X/R", {SCE_CTRL_LTRIGGER, SCE_CTRL_DOWN, SCE_CTRL_CROSS, SCE_CTRL_RTRIGGER}},
    {"DPad", {SCE_CTRL_LEFT, SCE_CTRL_DOWN, SCE_CTRL_UP, SCE_CTRL_RIGHT}},
    {"Face", {SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_CROSS, SCE_CTRL_CIRCLE}},
    {"L/SQ/X/R", {SCE_CTRL_LTRIGGER, SCE_CTRL_SQUARE, SCE_CTRL_CROSS, SCE_CTRL_RTRIGGER}},
};

constexpr int BIND_PRESET_COUNT = sizeof(BIND_PRESETS) / sizeof(BIND_PRESETS[0]);

const uint32_t BINDABLE_BUTTONS[] = {
    SCE_CTRL_LTRIGGER,
    SCE_CTRL_RTRIGGER,
    SCE_CTRL_LEFT,
    SCE_CTRL_DOWN,
    SCE_CTRL_UP,
    SCE_CTRL_RIGHT,
    SCE_CTRL_SQUARE,
    SCE_CTRL_TRIANGLE,
    SCE_CTRL_CROSS,
    SCE_CTRL_CIRCLE,
};

uint64_t nowMs() {
    return sceKernelGetProcessTimeWide() / 1000ULL;
}

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

int clampi(int value, int lo, int hi) {
    return std::max(lo, std::min(value, hi));
}

std::string trim(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        start++;
    }

    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        end--;
    }

    return s.substr(start, end - start);
}

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<char>(c + ('a' - 'A'));
        }
        return static_cast<char>(c);
    });
    return s;
}

bool endsWithNoCase(const std::string &s, const char *suffix) {
    std::string lower = lowerCopy(s);
    std::string lowerSuffix = lowerCopy(suffix);
    if (lower.size() < lowerSuffix.size()) {
        return false;
    }
    return lower.compare(lower.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
}

int parseInt(const std::string &s, int fallback = 0) {
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) {
        return fallback;
    }
    return static_cast<int>(v);
}

float parseFloat(const std::string &s, float fallback = 0.0f) {
    char *end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) {
        return fallback;
    }
    return v;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> splitCsv(const std::string &s) {
    std::vector<std::string> out;
    std::string part;
    bool quoted = false;
    for (char c : s) {
        if (c == '"') {
            quoted = !quoted;
            part.push_back(c);
        } else if (c == ',' && !quoted) {
            out.push_back(trim(part));
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    out.push_back(trim(part));
    return out;
}

std::string stripQuotes(const std::string &s) {
    std::string out = trim(s);
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
        out = out.substr(1, out.size() - 2);
    }
    return out;
}

std::string normalizeBeatmapPath(std::string s) {
    s = stripQuotes(s);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string joinPath(const std::string &a, const std::string &b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    if (a[a.size() - 1] == '/') {
        return a + b;
    }
    return a + "/" + b;
}

bool isDirectory(const std::string &path) {
    SceIoStat stat{};
    if (sceIoGetstat(path.c_str(), &stat) < 0) {
        return false;
    }
    return SCE_S_ISDIR(stat.st_mode);
}

bool fileExists(const std::string &path) {
    SceIoStat stat{};
    return sceIoGetstat(path.c_str(), &stat) >= 0 && !SCE_S_ISDIR(stat.st_mode);
}

void ensureDirectories() {
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(SONGS_DIR, 0777);
}

void drawText(vita2d_pgf *font, int x, int y, uint32_t color, float scale, const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    vita2d_pgf_draw_text(font, x, y, color, scale, buffer);
}

void drawTextCentered(vita2d_pgf *font, int cx, int y, uint32_t color, float scale, const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    int width = vita2d_pgf_text_width(font, scale, buffer);
    vita2d_pgf_draw_text(font, cx - width / 2, y, color, scale, buffer);
}

std::string fitText(const std::string &s, size_t maxChars) {
    if (s.size() <= maxChars || maxChars < 4) {
        return s;
    }
    return s.substr(0, maxChars - 3) + "...";
}

const char *buttonName(uint32_t button) {
    switch (button) {
    case SCE_CTRL_LTRIGGER:
        return "L";
    case SCE_CTRL_RTRIGGER:
        return "R";
    case SCE_CTRL_UP:
        return "UP";
    case SCE_CTRL_DOWN:
        return "DN";
    case SCE_CTRL_LEFT:
        return "LT";
    case SCE_CTRL_RIGHT:
        return "RT";
    case SCE_CTRL_CROSS:
        return "X";
    case SCE_CTRL_CIRCLE:
        return "O";
    case SCE_CTRL_SQUARE:
        return "SQ";
    case SCE_CTRL_TRIANGLE:
        return "TR";
    default:
        return "?";
    }
}

uint32_t firstBindablePressed(uint32_t pressed) {
    for (uint32_t button : BINDABLE_BUTTONS) {
        if (pressed & button) {
            return button;
        }
    }
    return 0;
}

struct Settings {
    uint32_t laneButtons[LANE_COUNT] = {};
    std::string laneLabels[LANE_COUNT];
    int bindPreset = 0;
    int musicVolume = 100;
    bool backgrounds = true;

    Settings() {
        applyPreset(0);
    }

    void refreshLabels() {
        for (int i = 0; i < LANE_COUNT; i++) {
            laneLabels[i] = buttonName(laneButtons[i]);
        }
    }

    void applyPreset(int preset) {
        bindPreset = (preset + BIND_PRESET_COUNT) % BIND_PRESET_COUNT;
        for (int i = 0; i < LANE_COUNT; i++) {
            laneButtons[i] = BIND_PRESETS[bindPreset].buttons[i];
        }
        refreshLabels();
    }

    void setLaneButton(int lane, uint32_t button) {
        if (lane < 0 || lane >= LANE_COUNT || button == 0) {
            return;
        }
        laneButtons[lane] = button;
        bindPreset = -1;
        refreshLabels();
    }
};

struct TimedSample {
    int timeMs = 0;
    std::string file;
    std::string path;
    int volume = 100;
};

struct Note {
    int lane = 0;
    int startMs = 0;
    int endMs = 0;
    int hitSound = 0;
    int sampleVolume = 100;
    std::string sampleFile;
    std::string samplePath;
    bool hold = false;
    bool headHit = false;
    bool complete = false;
    bool missed = false;
    bool activeHold = false;
};

struct TimingPoint {
    int timeMs = 0;
    double beatLength = 500.0;
};

struct Beatmap {
    std::string folder;
    std::string osuPath;
    std::string audioFile;
    std::string audioPath;
    std::string backgroundFile;
    std::string backgroundPath;
    std::string title = "Unknown Title";
    std::string artist = "Unknown Artist";
    std::string creator = "Unknown Creator";
    std::string version = "Unknown Difficulty";
    int mode = -1;
    int circleSize = 0;
    int sourceMode = -1;
    int sourceCircleSize = 0;
    int audioLeadIn = 0;
    int previewTime = 0;
    float overallDifficulty = 5.0f;
    float sliderMultiplier = 1.4f;
    float scrollSpeed = 0.42f;
    bool converted = false;
    std::string conversionLabel;
    std::vector<Note> notes;
    std::vector<TimedSample> storyboardSamples;
    std::string warning;
};

struct ParseResult {
    bool ok = false;
    Beatmap map;
    std::string error;
};

std::string replaceExtension(const std::string &path, const char *extension) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return path + extension;
    }
    return path.substr(0, dot) + extension;
}

std::string filenameWithoutExtension(const std::string &path) {
    size_t slash = path.find_last_of('/');
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < start) {
        return path.substr(start);
    }
    return path.substr(start, dot - start);
}

float recommendedScrollSpeed(const std::vector<Note> &notes) {
    if (notes.size() < 2) {
        return 0.34f;
    }

    std::vector<int> gaps;
    gaps.reserve(notes.size());
    int previous = notes.front().startMs;
    for (size_t i = 1; i < notes.size(); i++) {
        int gap = notes[i].startMs - previous;
        if (gap > 0) {
            gaps.push_back(gap);
            previous = notes[i].startMs;
        }
    }
    if (gaps.empty()) {
        return 0.34f;
    }

    std::sort(gaps.begin(), gaps.end());
    int medianGap = gaps[gaps.size() / 2];
    if (medianGap <= 95) {
        return 0.30f;
    }
    if (medianGap <= 145) {
        return 0.33f;
    }
    if (medianGap <= 210) {
        return 0.36f;
    }
    return 0.40f;
}

double beatLengthAt(const std::vector<TimingPoint> &timingPoints, int timeMs) {
    double beatLength = 500.0;
    for (const TimingPoint &point : timingPoints) {
        if (point.timeMs > timeMs) {
            break;
        }
        beatLength = point.beatLength;
    }
    return beatLength;
}

int laneFromX(int x, int columns) {
    columns = std::max(1, columns);
    return clampi((x * columns) / 512, 0, columns - 1);
}

int foldLaneTo4(int sourceLane, int sourceColumns) {
    if (sourceColumns <= LANE_COUNT) {
        return clampi(sourceLane, 0, LANE_COUNT - 1);
    }
    return clampi((sourceLane * LANE_COUNT) / sourceColumns, 0, LANE_COUNT - 1);
}

int noteEndForCollision(const Note &note) {
    return note.hold ? std::max(note.startMs, note.endMs) : note.startMs;
}

bool spansOverlap(int aStart, int aEnd, int bStart, int bEnd) {
    constexpr int guardMs = 6;
    aEnd = std::max(aStart, aEnd);
    bEnd = std::max(bStart, bEnd);
    return aStart <= bEnd + guardMs && bStart <= aEnd + guardMs;
}

bool laneOccupiedDuring(const std::vector<Note> &notes, int startMs, int endMs, int lane) {
    for (const Note &note : notes) {
        int existingEnd = noteEndForCollision(note);
        if (note.lane == lane && spansOverlap(startMs, endMs, note.startMs, existingEnd)) {
            return true;
        }
    }
    return false;
}

int findAvailableLane(const std::vector<Note> &notes, int startMs, int endMs, int preferredLane) {
    preferredLane = clampi(preferredLane, 0, LANE_COUNT - 1);
    if (!laneOccupiedDuring(notes, startMs, endMs, preferredLane)) {
        return preferredLane;
    }

    for (int radius = 1; radius < LANE_COUNT; radius++) {
        int left = preferredLane - radius;
        if (left >= 0 && !laneOccupiedDuring(notes, startMs, endMs, left)) {
            return left;
        }
        int right = preferredLane + radius;
        if (right < LANE_COUNT && !laneOccupiedDuring(notes, startMs, endMs, right)) {
            return right;
        }
    }
    return -1;
}

uint32_t nextRandom(uint32_t *state) {
    *state = (*state * 1103515245U) + 12345U;
    return (*state >> 16) & 0x7FFFU;
}

struct StandardConversionState {
    int lastTime = -1000000;
    int lastLane = -1;
    int lastX = 256;
    int stairDirection = 1;
    uint32_t rng = 1;
};

int chooseStandardLane(StandardConversionState *state, const std::vector<Note> &notes, int x, int startMs, int endMs) {
    int preferred = laneFromX(x, LANE_COUNT);
    int gap = startMs - state->lastTime;
    int positionSeparation = std::abs(x - state->lastX);

    if (state->lastLane >= 0) {
        if (gap <= 95) {
            preferred = state->lastLane + state->stairDirection;
            if (preferred >= LANE_COUNT) {
                preferred = LANE_COUNT - 2;
                state->stairDirection = -1;
            } else if (preferred < 0) {
                preferred = 1;
                state->stairDirection = 1;
            }
        } else if (gap <= 125 || (gap <= 170 && positionSeparation < 20)) {
            if (preferred == state->lastLane) {
                preferred = state->lastLane + state->stairDirection;
                if (preferred >= LANE_COUNT || preferred < 0) {
                    state->stairDirection *= -1;
                    preferred = state->lastLane + state->stairDirection;
                }
            }
        } else if (gap > 260 && (nextRandom(&state->rng) % 100) < 18) {
            preferred = static_cast<int>(nextRandom(&state->rng) % LANE_COUNT);
        }
    }

    int lane = findAvailableLane(notes, startMs, endMs, preferred);
    if (lane >= 0) {
        state->lastLane = lane;
        state->lastTime = startMs;
        state->lastX = x;
    }
    return lane;
}

void addConvertedNote(Beatmap *map, Note note) {
    int lane = findAvailableLane(map->notes, note.startMs, noteEndForCollision(note), note.lane);
    if (lane < 0 && note.hold) {
        note.hold = false;
        note.endMs = note.startMs;
        lane = findAvailableLane(map->notes, note.startMs, note.startMs, note.lane);
    }
    if (lane < 0) {
        return;
    }
    note.lane = lane;
    map->notes.push_back(note);
}

int sliderEndTime(int startMs, const std::vector<std::string> &parts, const std::vector<TimingPoint> &timingPoints, float sliderMultiplier) {
    if (parts.size() < 8) {
        return startMs;
    }
    int repeats = std::max(1, parseInt(parts[6], 1));
    float pixelLength = std::max(0.0f, parseFloat(parts[7], 0.0f));
    double beatLength = beatLengthAt(timingPoints, startMs);
    double multiplier = std::max(0.1f, sliderMultiplier);
    int duration = static_cast<int>(std::floor(pixelLength * beatLength * repeats * 0.01 / multiplier));
    return std::max(startMs, startMs + duration);
}

void parseHitSample(const std::string &params, const std::string &folder, Note *note) {
    if (!note) {
        return;
    }

    std::vector<std::string> parts = split(params, ':');
    if (parts.size() < 5) {
        return;
    }

    int volume = parseInt(parts[3], 100);
    std::string file = normalizeBeatmapPath(parts[4]);
    if (file.empty()) {
        return;
    }

    note->sampleFile = file;
    note->samplePath = joinPath(folder, file);
    note->sampleVolume = clampi(volume, 0, 100);
}

ParseResult parseOsuFile(const std::string &path, const std::string &folder) {
    ParseResult result;
    result.map.osuPath = path;
    result.map.folder = folder;

    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        result.error = "Could not open .osu file";
        return result;
    }

    char raw[4096];
    std::string section;
    bool firstLine = true;
    std::vector<TimingPoint> timingPoints;
    StandardConversionState standardState;
    bool standardStateSeeded = false;

    while (std::fgets(raw, sizeof(raw), file)) {
        std::string line(raw);
        if (firstLine) {
            firstLine = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }

        line = trim(line);
        if (line.empty() || line[0] == '/' || line[0] == '#') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        if (section == "Events") {
            std::vector<std::string> parts = splitCsv(line);
            if (parts.empty()) {
                continue;
            }

            std::string eventType = lowerCopy(stripQuotes(parts[0]));
            if ((eventType == "0" || eventType == "background") && parts.size() >= 3) {
                std::string bg = normalizeBeatmapPath(parts[2]);
                if (!bg.empty()) {
                    result.map.backgroundFile = bg;
                    result.map.backgroundPath = joinPath(folder, bg);
                }
            } else if (eventType == "1" || eventType == "video") {
                continue;
            } else if (eventType == "sample" && parts.size() >= 4) {
                TimedSample sample;
                sample.timeMs = parseInt(parts[1], 0);
                sample.file = normalizeBeatmapPath(parts[3]);
                sample.path = joinPath(folder, sample.file);
                if (parts.size() >= 5) {
                    sample.volume = clampi(parseInt(parts[4], 100), 0, 100);
                }
                if (!sample.file.empty()) {
                    result.map.storyboardSamples.push_back(sample);
                }
            }
            continue;
        }

        if (section == "TimingPoints") {
            std::vector<std::string> parts = split(line, ',');
            if (parts.size() >= 2) {
                double beatLength = parseFloat(parts[1], 0.0f);
                if (beatLength > 0.0) {
                    TimingPoint point;
                    point.timeMs = parseInt(parts[0], 0);
                    point.beatLength = beatLength;
                    timingPoints.push_back(point);
                }
            }
            continue;
        }

        if (section == "HitObjects") {
            std::vector<std::string> parts = split(line, ',');
            if (parts.size() < 5) {
                continue;
            }

            int x = parseInt(parts[0], 0);
            int time = parseInt(parts[2], 0);
            int type = parseInt(parts[3], 0);
            bool isHold = (type & 128) != 0;
            bool isTap = (type & 1) != 0;
            bool isSlider = (type & 2) != 0;
            bool isSpinner = (type & 8) != 0;

            if (result.map.mode == 3) {
                if (!isHold && !isTap) {
                    continue;
                }

                int columns = result.map.circleSize > 0 ? result.map.circleSize : LANE_COUNT;
                int sourceLane = laneFromX(x, columns);

                Note note;
                note.lane = columns == LANE_COUNT ? sourceLane : foldLaneTo4(sourceLane, columns);
                note.startMs = time;
                note.endMs = time;
                note.hitSound = parseInt(parts[4], 0);
                note.hold = isHold;

                if (isHold && parts.size() >= 6) {
                    std::string objectParams = parts[5];
                    size_t colon = objectParams.find(':');
                    std::string endTime = colon == std::string::npos ? objectParams : objectParams.substr(0, colon);
                    note.endMs = std::max(time, parseInt(endTime, time));
                    if (colon != std::string::npos) {
                        parseHitSample(objectParams.substr(colon + 1), folder, &note);
                    }
                } else if (!isHold && parts.size() >= 6) {
                    parseHitSample(parts[5], folder, &note);
                }

                if (columns == LANE_COUNT) {
                    result.map.notes.push_back(note);
                } else {
                    addConvertedNote(&result.map, note);
                }
                continue;
            }

            if (result.map.mode == 0) {
                if (!standardStateSeeded) {
                    standardState.rng = static_cast<uint32_t>((static_cast<int>(std::lround(result.map.circleSize)) + 1) * 131 +
                                                              static_cast<int>(result.map.overallDifficulty * 412.0f) + 17);
                    standardStateSeeded = true;
                }

                Note note;
                note.startMs = time;
                note.endMs = time;
                note.hitSound = parseInt(parts[4], 0);

                if (isTap) {
                    int lane = chooseStandardLane(&standardState, result.map.notes, x, time, time);
                    if (lane < 0) {
                        continue;
                    }
                    note.lane = lane;
                    if (parts.size() >= 6) {
                        parseHitSample(parts[5], folder, &note);
                    }
                    result.map.notes.push_back(note);
                } else if (isSlider) {
                    note.endMs = sliderEndTime(time, parts, timingPoints, result.map.sliderMultiplier);
                    note.hold = note.endMs - note.startMs >= 90;
                    int lane = chooseStandardLane(&standardState, result.map.notes, x, time, noteEndForCollision(note));
                    if (lane < 0 && note.hold) {
                        note.hold = false;
                        note.endMs = note.startMs;
                        lane = chooseStandardLane(&standardState, result.map.notes, x, time, time);
                    }
                    if (lane < 0) {
                        continue;
                    }
                    note.lane = lane;
                    result.map.notes.push_back(note);
                } else if (isSpinner && parts.size() >= 6) {
                    int endMs = std::max(time, parseInt(parts[5], time));
                    note.hold = endMs - time >= 120;
                    note.endMs = endMs;
                    note.lane = findAvailableLane(result.map.notes, time, noteEndForCollision(note), LANE_COUNT / 2);
                    if (note.lane < 0 && note.hold) {
                        note.hold = false;
                        note.endMs = note.startMs;
                        note.lane = findAvailableLane(result.map.notes, time, time, LANE_COUNT / 2);
                    }
                    if (note.lane < 0) {
                        continue;
                    }
                    result.map.notes.push_back(note);
                    standardState.lastLane = note.lane;
                    standardState.lastTime = time;
                    standardState.lastX = x;
                }
            }
            continue;
        }

        size_t sep = line.find(':');
        if (sep == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, sep));
        std::string value = trim(line.substr(sep + 1));

        if (section == "General") {
            if (key == "AudioFilename") {
                result.map.audioFile = normalizeBeatmapPath(value);
            } else if (key == "AudioLeadIn") {
                result.map.audioLeadIn = parseInt(value, 0);
            } else if (key == "PreviewTime") {
                result.map.previewTime = std::max(0, parseInt(value, 0));
            } else if (key == "Mode") {
                result.map.mode = parseInt(value, -1);
            }
        } else if (section == "Metadata") {
            if (key == "Title") {
                if (!value.empty()) {
                    result.map.title = value;
                }
            } else if (key == "TitleUnicode") {
                if (!value.empty() && result.map.title == "Unknown Title") {
                    result.map.title = value;
                }
            } else if (key == "Artist") {
                if (!value.empty()) {
                    result.map.artist = value;
                }
            } else if (key == "ArtistUnicode") {
                if (!value.empty() && result.map.artist == "Unknown Artist") {
                    result.map.artist = value;
                }
            } else if (key == "Creator") {
                if (!value.empty()) {
                    result.map.creator = value;
                }
            } else if (key == "Version") {
                if (!value.empty()) {
                    result.map.version = value;
                }
            }
        } else if (section == "Difficulty") {
            if (key == "CircleSize") {
                result.map.circleSize = parseInt(value, 0);
            } else if (key == "OverallDifficulty") {
                result.map.overallDifficulty = parseFloat(value, 5.0f);
            } else if (key == "SliderMultiplier") {
                result.map.sliderMultiplier = std::max(0.1f, parseFloat(value, 1.4f));
            }
        }
    }

    std::fclose(file);

    result.map.sourceMode = result.map.mode;
    result.map.sourceCircleSize = result.map.circleSize;

    if (result.map.mode != 3 && result.map.mode != 0) {
        result.error = "Only osu!standard and osu!mania maps are supported";
        return result;
    }

    if (result.map.sourceMode == 3 && result.map.sourceCircleSize != LANE_COUNT) {
        result.map.converted = true;
        char label[32];
        std::snprintf(label, sizeof(label), "%dK->4K", result.map.sourceCircleSize);
        result.map.conversionLabel = label;
    } else if (result.map.sourceMode == 0) {
        result.map.converted = true;
        result.map.conversionLabel = "standard->4K";
    }
    result.map.mode = 3;
    result.map.circleSize = LANE_COUNT;

    if (result.map.notes.empty()) {
        result.error = "No playable notes found";
        return result;
    }

    std::sort(result.map.notes.begin(), result.map.notes.end(), [](const Note &a, const Note &b) {
        if (a.startMs != b.startMs) {
            return a.startMs < b.startMs;
        }
        return a.lane < b.lane;
    });
    result.map.scrollSpeed = recommendedScrollSpeed(result.map.notes);

    if (result.map.audioFile.empty()) {
        result.map.warning = "Map has no AudioFilename";
    } else {
        result.map.audioPath = joinPath(folder, result.map.audioFile);
        if (!fileExists(result.map.audioPath)) {
            result.map.warning = "Audio file not found";
        } else if (!endsWithNoCase(result.map.audioPath, ".ogg")) {
            std::string mp3Path = replaceExtension(result.map.audioPath, ".mp3");
            std::string m4aPath = replaceExtension(result.map.audioPath, ".m4a");
            std::string aacPath = replaceExtension(result.map.audioPath, ".aac");
            if (endsWithNoCase(result.map.audioPath, ".mp3") || endsWithNoCase(result.map.audioPath, ".m4a") || endsWithNoCase(result.map.audioPath, ".aac")) {
                // Native/system audio path is already usable.
            } else if (fileExists(mp3Path)) {
                result.map.audioPath = mp3Path;
                result.map.warning = "Using converted MP3 audio";
            } else if (fileExists(m4aPath)) {
                result.map.audioPath = m4aPath;
                result.map.warning = "Using converted M4A audio";
            } else if (fileExists(aacPath)) {
                result.map.audioPath = aacPath;
                result.map.warning = "Using converted AAC audio";
            }
        }
    }

    std::sort(result.map.storyboardSamples.begin(), result.map.storyboardSamples.end(), [](const TimedSample &a, const TimedSample &b) {
        return a.timeMs < b.timeMs;
    });

    result.ok = true;
    return result;
}

uint16_t readZip16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t readZip32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool readFileBytes(const std::string &path, std::vector<uint8_t> *bytes) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(file);
        return false;
    }

    bytes->resize(static_cast<size_t>(size));
    if (size > 0) {
        size_t read = std::fread(bytes->data(), 1, bytes->size(), file);
        if (read != bytes->size()) {
            std::fclose(file);
            return false;
        }
    }

    std::fclose(file);
    return true;
}

bool ensureDirectoryPath(const std::string &path) {
    if (path.empty()) {
        return true;
    }

    size_t pos = 0;
    while (true) {
        pos = path.find('/', pos + 1);
        std::string part = pos == std::string::npos ? path : path.substr(0, pos);
        if (!part.empty()) {
            sceIoMkdir(part.c_str(), 0777);
        }
        if (pos == std::string::npos) {
            break;
        }
    }
    return true;
}

bool ensureParentDirectory(const std::string &path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }
    return ensureDirectoryPath(path.substr(0, slash));
}

bool writeFileBytes(const std::string &path, const uint8_t *data, size_t size) {
    ensureParentDirectory(path);
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    if (size > 0 && std::fwrite(data, 1, size, file) != size) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    return true;
}

bool safeZipEntryPath(const std::string &rawName, std::string *safePath) {
    std::string name = rawName;
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.front() == '/') {
        name.erase(name.begin());
    }
    if (name.empty() || name.find(':') != std::string::npos) {
        return false;
    }

    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        std::string part = slash == std::string::npos ? name.substr(start) : name.substr(start, slash - start);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                return false;
            }
            parts.push_back(part);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    if (parts.empty()) {
        return false;
    }

    std::string out = parts[0];
    for (size_t i = 1; i < parts.size(); i++) {
        out = joinPath(out, parts[i]);
    }
    *safePath = out;
    return true;
}

bool shouldSkipZipEntry(const std::string &safePath) {
    std::string lower = lowerCopy(safePath);
    return lower == "__macosx" ||
           lower.compare(0, 9, "__macosx/") == 0 ||
           lower == ".ds_store" ||
           (lower.size() > 10 && lower.compare(lower.size() - 10, 10, "/.ds_store") == 0);
}

bool findZipEndOfCentralDirectory(const std::vector<uint8_t> &archive, size_t *offset) {
    if (archive.size() < 22) {
        return false;
    }

    size_t minimum = archive.size() > 65557 ? archive.size() - 65557 : 0;
    size_t pos = archive.size() - 22;
    while (true) {
        if (pos + 22 <= archive.size() &&
            archive[pos] == 0x50 && archive[pos + 1] == 0x4b &&
            archive[pos + 2] == 0x05 && archive[pos + 3] == 0x06) {
            uint16_t commentLength = readZip16(&archive[pos + 20]);
            if (pos + 22 + commentLength <= archive.size()) {
                *offset = pos;
                return true;
            }
        }
        if (pos == minimum) {
            break;
        }
        pos--;
    }
    return false;
}

bool inflateZipBytes(const uint8_t *compressed, size_t compressedSize, std::vector<uint8_t> *out) {
    if (out->empty()) {
        return true;
    }

    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(compressed);
    stream.avail_in = static_cast<uInt>(compressedSize);
    stream.next_out = out->data();
    stream.avail_out = static_cast<uInt>(out->size());

    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return false;
    }
    int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    return status == Z_STREAM_END && stream.total_out == out->size();
}

bool extractZipEntry(const std::vector<uint8_t> &archive,
                     const std::string &targetDir,
                     const std::string &safePath,
                     uint16_t method,
                     uint32_t compressedSize,
                     uint32_t uncompressedSize,
                     uint32_t localHeaderOffset) {
    if (localHeaderOffset + 30 > archive.size()) {
        return false;
    }
    const uint8_t *local = &archive[localHeaderOffset];
    if (readZip32(local) != 0x04034b50) {
        return false;
    }

    uint16_t localNameLength = readZip16(local + 26);
    uint16_t localExtraLength = readZip16(local + 28);
    size_t dataOffset = static_cast<size_t>(localHeaderOffset) + 30 + localNameLength + localExtraLength;
    if (dataOffset > archive.size() || dataOffset + compressedSize > archive.size()) {
        return false;
    }

    std::string outPath = joinPath(targetDir, safePath);
    if (method == 0) {
        const uint8_t *data = compressedSize == 0 ? nullptr : &archive[dataOffset];
        return writeFileBytes(outPath, data, compressedSize);
    }
    if (method != 8) {
        return false;
    }

    std::vector<uint8_t> inflated(uncompressedSize);
    const uint8_t *data = compressedSize == 0 ? nullptr : &archive[dataOffset];
    if (!inflateZipBytes(data, compressedSize, &inflated)) {
        return false;
    }
    return writeFileBytes(outPath, inflated.empty() ? nullptr : inflated.data(), inflated.size());
}

bool directoryContainsOsuFile(const std::string &dir, int depth = 0) {
    if (depth > 5) {
        return false;
    }

    DIR *handle = opendir(dir.c_str());
    if (!handle) {
        return false;
    }

    bool found = false;
    while (!found) {
        dirent *entry = readdir(handle);
        if (!entry) {
            break;
        }

        std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }

        std::string path = joinPath(dir, name);
        if (isDirectory(path)) {
            found = directoryContainsOsuFile(path, depth + 1);
        } else if (endsWithNoCase(name, ".osu")) {
            found = true;
        }
    }

    closedir(handle);
    return found;
}

bool extractOszArchive(const std::string &path, const std::string &destinationRoot) {
    std::vector<uint8_t> archive;
    if (!readFileBytes(path, &archive)) {
        return false;
    }

    size_t eocdOffset = 0;
    if (!findZipEndOfCentralDirectory(archive, &eocdOffset)) {
        return false;
    }

    const uint8_t *eocd = &archive[eocdOffset];
    uint16_t entryCount = readZip16(eocd + 10);
    uint32_t centralDirOffset = readZip32(eocd + 16);
    if (centralDirOffset >= archive.size()) {
        return false;
    }

    std::string targetDir = joinPath(destinationRoot, filenameWithoutExtension(path));
    ensureDirectoryPath(targetDir);

    bool extractedAny = false;
    size_t offset = centralDirOffset;
    for (uint16_t i = 0; i < entryCount; i++) {
        if (offset + 46 > archive.size()) {
            return false;
        }
        const uint8_t *central = &archive[offset];
        if (readZip32(central) != 0x02014b50) {
            return false;
        }

        uint16_t flags = readZip16(central + 8);
        uint16_t method = readZip16(central + 10);
        uint32_t compressedSize = readZip32(central + 20);
        uint32_t uncompressedSize = readZip32(central + 24);
        uint16_t nameLength = readZip16(central + 28);
        uint16_t extraLength = readZip16(central + 30);
        uint16_t commentLength = readZip16(central + 32);
        uint32_t localHeaderOffset = readZip32(central + 42);
        size_t nextOffset = offset + 46 + nameLength + extraLength + commentLength;
        if (nextOffset > archive.size() || offset + 46 + nameLength > archive.size()) {
            return false;
        }

        std::string rawName(reinterpret_cast<const char *>(&archive[offset + 46]), nameLength);
        bool isDirectoryEntry = !rawName.empty() && (rawName.back() == '/' || rawName.back() == '\\');
        std::string safePath;
        if (!safeZipEntryPath(rawName, &safePath)) {
            return false;
        }

        if (!shouldSkipZipEntry(safePath)) {
            if (isDirectoryEntry) {
                ensureDirectoryPath(joinPath(targetDir, safePath));
            } else {
                if ((flags & 0x0001) != 0) {
                    return false;
                }
                if (!extractZipEntry(archive, targetDir, safePath, method, compressedSize, uncompressedSize, localHeaderOffset)) {
                    return false;
                }
                extractedAny = true;
            }
        }

        offset = nextOffset;
    }

    return extractedAny && directoryContainsOsuFile(targetDir);
}

void extractOszFilesInDir(const std::string &dir, const std::string &destinationRoot) {
    DIR *handle = opendir(dir.c_str());
    if (!handle) {
        return;
    }

    std::vector<std::string> archives;
    while (dirent *entry = readdir(handle)) {
        std::string name(entry->d_name);
        if (name == "." || name == ".." || !endsWithNoCase(name, ".osz")) {
            continue;
        }
        archives.push_back(joinPath(dir, name));
    }
    closedir(handle);

    for (const std::string &archivePath : archives) {
        if (extractOszArchive(archivePath, destinationRoot)) {
            if (sceIoRemove(archivePath.c_str()) < 0) {
                std::remove(archivePath.c_str());
            }
        }
    }
}

void scanDirForMaps(const std::string &dir, int depth, std::vector<Beatmap> *maps, int *skipped, int *skippedNon4K) {
    if (depth > 5) {
        return;
    }

    extractOszFilesInDir(dir, dir);

    DIR *handle = opendir(dir.c_str());
    if (!handle) {
        return;
    }

    while (dirent *entry = readdir(handle)) {
        std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }

        std::string path = joinPath(dir, name);
        if (isDirectory(path)) {
            scanDirForMaps(path, depth + 1, maps, skipped, skippedNon4K);
            continue;
        }

        if (!endsWithNoCase(name, ".osu")) {
            continue;
        }

        ParseResult parsed = parseOsuFile(path, dir);
        if (parsed.ok) {
            maps->push_back(parsed.map);
        } else {
            (*skipped)++;
            if (skippedNon4K && parsed.error.find("K mania maps are not supported") != std::string::npos) {
                (*skippedNon4K)++;
            }
        }
    }

    closedir(handle);
}

struct SongEntry {
    std::string key;
    std::string title;
    std::string artist;
    std::string creator;
    std::string folder;
    std::string audioPath;
    std::string backgroundPath;
    std::vector<int> mapIndexes;
    int selectedDifficulty = 0;
};

struct Library {
    std::vector<Beatmap> maps;
    std::vector<SongEntry> songs;
    int selectedSong = 0;
    int skipped = 0;
    int skippedNon4K = 0;
    int convertedDifficulties = 0;
};

std::string songKeyFor(const Beatmap &map) {
    return lowerCopy(map.folder + "|" + map.artist + "|" + map.title + "|" + map.audioFile);
}

void buildSongEntries(Library *library) {
    library->songs.clear();
    for (int i = 0; i < static_cast<int>(library->maps.size()); i++) {
        const Beatmap &map = library->maps[i];
        std::string key = songKeyFor(map);
        int found = -1;
        for (int s = 0; s < static_cast<int>(library->songs.size()); s++) {
            if (library->songs[s].key == key) {
                found = s;
                break;
            }
        }

        if (found < 0) {
            SongEntry entry;
            entry.key = key;
            entry.title = map.title;
            entry.artist = map.artist;
            entry.creator = map.creator;
            entry.folder = map.folder;
            entry.audioPath = map.audioPath;
            entry.backgroundPath = map.backgroundPath;
            library->songs.push_back(entry);
            found = static_cast<int>(library->songs.size()) - 1;
        }

        SongEntry &entry = library->songs[found];
        entry.mapIndexes.push_back(i);
        if (entry.backgroundPath.empty() && !map.backgroundPath.empty()) {
            entry.backgroundPath = map.backgroundPath;
        }
    }

    for (SongEntry &entry : library->songs) {
        std::sort(entry.mapIndexes.begin(), entry.mapIndexes.end(), [library](int a, int b) {
            const Beatmap &ma = library->maps[a];
            const Beatmap &mb = library->maps[b];
            if (ma.overallDifficulty != mb.overallDifficulty) {
                return ma.overallDifficulty < mb.overallDifficulty;
            }
            return lowerCopy(ma.version) < lowerCopy(mb.version);
        });
        entry.selectedDifficulty = clampi(entry.selectedDifficulty, 0, static_cast<int>(entry.mapIndexes.size()) - 1);
    }

    std::sort(library->songs.begin(), library->songs.end(), [](const SongEntry &a, const SongEntry &b) {
        return lowerCopy(a.artist + " " + a.title) < lowerCopy(b.artist + " " + b.title);
    });

    library->selectedSong = clampi(library->selectedSong, 0, static_cast<int>(library->songs.size()) - 1);
}

Beatmap *selectedBeatmap(Library *library) {
    if (!library || library->songs.empty()) {
        return nullptr;
    }
    SongEntry &song = library->songs[clampi(library->selectedSong, 0, static_cast<int>(library->songs.size()) - 1)];
    if (song.mapIndexes.empty()) {
        return nullptr;
    }
    int diff = clampi(song.selectedDifficulty, 0, static_cast<int>(song.mapIndexes.size()) - 1);
    return &library->maps[song.mapIndexes[diff]];
}

const Beatmap *selectedBeatmap(const Library &library) {
    if (library.songs.empty()) {
        return nullptr;
    }
    const SongEntry &song = library.songs[clampi(library.selectedSong, 0, static_cast<int>(library.songs.size()) - 1)];
    if (song.mapIndexes.empty()) {
        return nullptr;
    }
    int diff = clampi(song.selectedDifficulty, 0, static_cast<int>(song.mapIndexes.size()) - 1);
    return &library.maps[song.mapIndexes[diff]];
}

Library scanLibrary() {
    ensureDirectories();
    extractOszFilesInDir(DATA_DIR, SONGS_DIR);

    Library library;
    scanDirForMaps(SONGS_DIR, 0, &library.maps, &library.skipped, &library.skippedNon4K);
    for (const Beatmap &map : library.maps) {
        if (map.converted) {
            library.convertedDifficulties++;
        }
    }

    std::sort(library.maps.begin(), library.maps.end(), [](const Beatmap &a, const Beatmap &b) {
        std::string ak = lowerCopy(a.artist + " " + a.title + " " + a.version);
        std::string bk = lowerCopy(b.artist + " " + b.title + " " + b.version);
        return ak < bk;
    });

    buildSongEntries(&library);
    return library;
}

class BackgroundManager {
public:
    ~BackgroundManager() {
        stopWorker();
        clear();
        freeCache();
    }

    void setPath(const std::string &path, bool immediate = false) {
        if (path.empty()) {
            if (targetPath_.empty() && currentPath_.empty() && !current_) {
                return;
            }
            clear();
            return;
        }

        ensureWorker();
        pump();

        uint64_t now = nowMs();
        if (path != targetPath_) {
            targetPath_ = path;
            targetChangedAtMs_ = now;
        }

        if (targetPath_ == currentPath_) {
            return;
        }

        if (useCached(targetPath_)) {
            return;
        }

        if (!immediate && now - targetChangedAtMs_ < 30) {
            return;
        }

        if (endsWithNoCase(targetPath_, ".jpg") || endsWithNoCase(targetPath_, ".jpeg") ||
            endsWithNoCase(targetPath_, ".png") || endsWithNoCase(targetPath_, ".bmp")) {
            requestDecode(targetPath_);
        } else {
            current_ = nullptr;
            currentPath_ = targetPath_;
        }
    }

    void preloadNow(const std::string &path) {
        stopWorker();

        if (path.empty()) {
            clear();
            return;
        }

        targetPath_ = path;
        targetChangedAtMs_ = nowMs();
        if (targetPath_ == currentPath_) {
            return;
        }
        if (useCached(targetPath_)) {
            return;
        }

        if (endsWithNoCase(targetPath_, ".jpg") || endsWithNoCase(targetPath_, ".jpeg") ||
            endsWithNoCase(targetPath_, ".png") || endsWithNoCase(targetPath_, ".bmp")) {
            DecodedImage image;
            image.path = targetPath_;
            decodeImageThumbnail(targetPath_, &image);
            replaceTexture(image);
        } else {
            current_ = nullptr;
            currentPath_ = targetPath_;
        }
    }

    void draw() {
        vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(8, 11, 16, 255));

        if (current_) {
            uint64_t elapsed = nowMs() - fadeStartMs_;
            float t = clampf(static_cast<float>(elapsed) / 140.0f, 0.0f, 1.0f);
            drawTextureCover(current_, static_cast<uint8_t>(255.0f * t));
        }
    }

    void clear() {
        current_ = nullptr;
        currentPath_.clear();
        targetPath_.clear();
        targetChangedAtMs_ = 0;
        if (workerMutex_ >= 0) {
            sceKernelLockMutex(workerMutex_, 1, nullptr);
            requestedPath_.clear();
            requestSerial_++;
            result_ = DecodedImage{};
            resultPending_ = false;
            sceKernelUnlockMutex(workerMutex_, 1);
        }
    }

private:
    static constexpr int THUMB_W = 480;
    static constexpr int THUMB_H = 272;
    static constexpr int MAX_CACHED_BACKGROUNDS = 8;

    struct DecodedImage {
        std::string path;
        int width = 0;
        int height = 0;
        bool ok = false;
        std::vector<uint32_t> pixels;
    };

    struct CacheEntry {
        std::string path;
        vita2d_texture *texture = nullptr;
        uint64_t lastUsedMs = 0;
    };

    struct JpegErrorManager {
        jpeg_error_mgr pub;
        jmp_buf jump;
    };

    static void jpegErrorExit(j_common_ptr cinfo) {
        JpegErrorManager *err = reinterpret_cast<JpegErrorManager *>(cinfo->err);
        longjmp(err->jump, 1);
    }

    bool useCached(const std::string &path) {
        for (CacheEntry &entry : cache_) {
            if (entry.path == path && entry.texture) {
                current_ = entry.texture;
                currentPath_ = path;
                entry.lastUsedMs = nowMs();
                fadeStartMs_ = nowMs();
                return true;
            }
        }
        return false;
    }

    void replaceTexture(const DecodedImage &image) {
        if (useCached(image.path)) {
            return;
        }

        vita2d_texture *next = nullptr;
        if (image.ok && image.width > 0 && image.height > 0 && !image.pixels.empty()) {
            next = vita2d_create_empty_texture(static_cast<unsigned int>(image.width), static_cast<unsigned int>(image.height));
            if (next) {
                uint8_t *dst = static_cast<uint8_t *>(vita2d_texture_get_datap(next));
                unsigned int strideBytes = vita2d_texture_get_stride(next);
                for (int y = 0; y < image.height; y++) {
                    std::memcpy(dst + static_cast<size_t>(y) * strideBytes,
                                image.pixels.data() + static_cast<size_t>(y) * image.width,
                                static_cast<size_t>(image.width) * sizeof(uint32_t));
                }
                vita2d_texture_set_filters(next, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
            }
        }

        current_ = next;
        currentPath_ = image.path;
        fadeStartMs_ = nowMs();
        if (next) {
            CacheEntry entry;
            entry.path = image.path;
            entry.texture = next;
            entry.lastUsedMs = fadeStartMs_;
            cache_.push_back(entry);
            trimCache();
        }
    }

    void trimCache() {
        while (static_cast<int>(cache_.size()) > MAX_CACHED_BACKGROUNDS) {
            int victim = -1;
            uint64_t oldest = UINT64_MAX;
            for (int i = 0; i < static_cast<int>(cache_.size()); i++) {
                if (cache_[i].texture != current_ && cache_[i].lastUsedMs < oldest) {
                    oldest = cache_[i].lastUsedMs;
                    victim = i;
                }
            }
            if (victim < 0) {
                return;
            }
            vita2d_wait_rendering_done();
            vita2d_free_texture(cache_[victim].texture);
            cache_.erase(cache_.begin() + victim);
        }
    }

    void freeCache() {
        vita2d_wait_rendering_done();
        for (CacheEntry &entry : cache_) {
            if (entry.texture) {
                vita2d_free_texture(entry.texture);
            }
        }
        cache_.clear();
        current_ = nullptr;
    }

    static void drawTextureCover(vita2d_texture *texture, uint8_t alpha) {
        if (!texture || alpha == 0) {
            return;
        }
        float w = static_cast<float>(vita2d_texture_get_width(texture));
        float h = static_cast<float>(vita2d_texture_get_height(texture));
        if (w <= 0.0f || h <= 0.0f) {
            return;
        }
        float scale = std::max(static_cast<float>(SCREEN_W) / w, static_cast<float>(SCREEN_H) / h);
        float x = (SCREEN_W - w * scale) * 0.5f;
        float y = (SCREEN_H - h * scale) * 0.5f;
        vita2d_draw_texture_tint_scale(texture, x, y, scale, scale, RGBA8(255, 255, 255, alpha));
    }

    static void makeThumbnailFromPixels(const std::vector<uint32_t> &source, int sw, int sh, DecodedImage *out) {
        if (!out || source.empty() || sw <= 0 || sh <= 0) {
            return;
        }

        out->width = THUMB_W;
        out->height = THUMB_H;
        out->pixels.resize(static_cast<size_t>(THUMB_W) * THUMB_H);

        float scale = std::max(static_cast<float>(THUMB_W) / static_cast<float>(sw),
                               static_cast<float>(THUMB_H) / static_cast<float>(sh));
        float cropW = static_cast<float>(THUMB_W) / scale;
        float cropH = static_cast<float>(THUMB_H) / scale;
        float cropX = (static_cast<float>(sw) - cropW) * 0.5f;
        float cropY = (static_cast<float>(sh) - cropH) * 0.5f;

        for (int y = 0; y < THUMB_H; y++) {
            for (int x = 0; x < THUMB_W; x++) {
                int sx = clampi(static_cast<int>(cropX + (static_cast<float>(x) + 0.5f) / scale), 0, sw - 1);
                int sy = clampi(static_cast<int>(cropY + (static_cast<float>(y) + 0.5f) / scale), 0, sh - 1);
                out->pixels[static_cast<size_t>(y) * THUMB_W + x] = source[static_cast<size_t>(sy) * sw + sx];
            }
        }

        out->ok = true;
    }

    static bool decodeJpegThumbnail(const std::string &path, DecodedImage *out) {
        if (!out) {
            return false;
        }

        out->path = path;
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }

        jpeg_decompress_struct cinfo{};
        JpegErrorManager jerr{};
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpegErrorExit;

        if (setjmp(jerr.jump)) {
            jpeg_destroy_decompress(&cinfo);
            std::fclose(file);
            out->ok = false;
            out->pixels.clear();
            return false;
        }

        jpeg_create_decompress(&cinfo);
        jpeg_stdio_src(&cinfo, file);
        jpeg_read_header(&cinfo, TRUE);

        unsigned int sourceW = cinfo.image_width;
        unsigned int sourceH = cinfo.image_height;
        cinfo.scale_num = 1;
        if (sourceW >= 3200 || sourceH >= 1800) {
            cinfo.scale_denom = 8;
        } else if (sourceW >= 1200 || sourceH >= 700) {
            cinfo.scale_denom = 4;
        } else if (sourceW >= 640 || sourceH >= 360) {
            cinfo.scale_denom = 2;
        } else {
            cinfo.scale_denom = 1;
        }
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);

        int sw = static_cast<int>(cinfo.output_width);
        int sh = static_cast<int>(cinfo.output_height);
        int stride = sw * 3;
        std::vector<uint8_t> rgb(static_cast<size_t>(stride) * sh);

        while (cinfo.output_scanline < cinfo.output_height) {
            uint8_t *row = rgb.data() + static_cast<size_t>(cinfo.output_scanline) * stride;
            jpeg_read_scanlines(&cinfo, &row, 1);
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        std::fclose(file);

        if (sw <= 0 || sh <= 0) {
            return false;
        }

        out->width = THUMB_W;
        out->height = THUMB_H;
        out->pixels.resize(static_cast<size_t>(THUMB_W) * THUMB_H);

        float scale = std::max(static_cast<float>(THUMB_W) / static_cast<float>(sw),
                               static_cast<float>(THUMB_H) / static_cast<float>(sh));
        float cropW = static_cast<float>(THUMB_W) / scale;
        float cropH = static_cast<float>(THUMB_H) / scale;
        float cropX = (static_cast<float>(sw) - cropW) * 0.5f;
        float cropY = (static_cast<float>(sh) - cropH) * 0.5f;

        for (int y = 0; y < THUMB_H; y++) {
            for (int x = 0; x < THUMB_W; x++) {
                int sx = clampi(static_cast<int>(cropX + (static_cast<float>(x) + 0.5f) / scale), 0, sw - 1);
                int sy = clampi(static_cast<int>(cropY + (static_cast<float>(y) + 0.5f) / scale), 0, sh - 1);
                const uint8_t *src = rgb.data() + (static_cast<size_t>(sy) * sw + sx) * 3;
                out->pixels[static_cast<size_t>(y) * THUMB_W + x] = RGBA8(src[0], src[1], src[2], 255);
            }
        }

        out->ok = true;
        return true;
    }

    static bool decodePngThumbnail(const std::string &path, DecodedImage *out) {
        if (!out) {
            return false;
        }

        out->path = path;
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }

        uint8_t signature[8];
        if (std::fread(signature, 1, sizeof(signature), file) != sizeof(signature) ||
            png_sig_cmp(signature, 0, sizeof(signature)) != 0) {
            std::fclose(file);
            return false;
        }

        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png) {
            std::fclose(file);
            return false;
        }

        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            std::fclose(file);
            return false;
        }

        if (setjmp(png_jmpbuf(png))) {
            png_destroy_read_struct(&png, &info, nullptr);
            std::fclose(file);
            out->ok = false;
            out->pixels.clear();
            return false;
        }

        png_init_io(png, file);
        png_set_sig_bytes(png, sizeof(signature));
        png_read_info(png, info);

        png_uint_32 width = png_get_image_width(png, info);
        png_uint_32 height = png_get_image_height(png, info);
        int colorType = png_get_color_type(png, info);
        int bitDepth = png_get_bit_depth(png, info);

        if (bitDepth == 16) {
            png_set_strip_16(png);
        }
        if (colorType == PNG_COLOR_TYPE_PALETTE) {
            png_set_palette_to_rgb(png);
        }
        if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
            png_set_expand_gray_1_2_4_to_8(png);
        }
        if (png_get_valid(png, info, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(png);
        }
        if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png);
        }
        if (!(colorType & PNG_COLOR_MASK_ALPHA)) {
            png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
        }

        png_read_update_info(png, info);

        int sw = static_cast<int>(width);
        int sh = static_cast<int>(height);
        if (sw <= 0 || sh <= 0 || sw > 8192 || sh > 8192) {
            png_destroy_read_struct(&png, &info, nullptr);
            std::fclose(file);
            return false;
        }

        png_size_t rowBytes = png_get_rowbytes(png, info);
        std::vector<uint8_t> imageBytes(static_cast<size_t>(rowBytes) * sh);
        std::vector<png_bytep> rows(sh);
        for (int y = 0; y < sh; y++) {
            rows[y] = imageBytes.data() + static_cast<size_t>(y) * rowBytes;
        }
        png_read_image(png, rows.data());
        png_read_end(png, nullptr);
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(file);

        std::vector<uint32_t> source(static_cast<size_t>(sw) * sh);
        for (int y = 0; y < sh; y++) {
            const uint8_t *row = imageBytes.data() + static_cast<size_t>(y) * rowBytes;
            for (int x = 0; x < sw; x++) {
                const uint8_t *px = row + static_cast<size_t>(x) * 4;
                source[static_cast<size_t>(y) * sw + x] = RGBA8(px[0], px[1], px[2], px[3]);
            }
        }

        makeThumbnailFromPixels(source, sw, sh, out);
        return out->ok;
    }

    static uint16_t readLe16(const uint8_t *p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    static uint32_t readLe32(const uint8_t *p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    static bool decodeBmpThumbnail(const std::string &path, DecodedImage *out) {
        if (!out) {
            return false;
        }

        out->path = path;
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        std::fseek(file, 0, SEEK_END);
        long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size < 54) {
            std::fclose(file);
            return false;
        }

        std::vector<uint8_t> data(static_cast<size_t>(size));
        size_t read = std::fread(data.data(), 1, data.size(), file);
        std::fclose(file);
        if (read != data.size() || data[0] != 'B' || data[1] != 'M') {
            return false;
        }

        uint32_t pixelOffset = readLe32(data.data() + 10);
        uint32_t dibSize = readLe32(data.data() + 14);
        if (dibSize < 40 || pixelOffset >= data.size()) {
            return false;
        }

        int32_t rawW = static_cast<int32_t>(readLe32(data.data() + 18));
        int32_t rawH = static_cast<int32_t>(readLe32(data.data() + 22));
        uint16_t planes = readLe16(data.data() + 26);
        uint16_t bpp = readLe16(data.data() + 28);
        uint32_t compression = readLe32(data.data() + 30);
        if (rawW <= 0 || rawH == 0 || planes != 1 || compression != 0 || (bpp != 24 && bpp != 32)) {
            return false;
        }

        int sw = rawW;
        int sh = rawH < 0 ? -rawH : rawH;
        bool topDown = rawH < 0;
        if (sw > 8192 || sh > 8192) {
            return false;
        }

        size_t rowStride = ((static_cast<size_t>(sw) * bpp + 31) / 32) * 4;
        if (pixelOffset + rowStride * static_cast<size_t>(sh) > data.size()) {
            return false;
        }

        std::vector<uint32_t> source(static_cast<size_t>(sw) * sh);
        for (int y = 0; y < sh; y++) {
            int srcY = topDown ? y : (sh - 1 - y);
            const uint8_t *row = data.data() + pixelOffset + static_cast<size_t>(srcY) * rowStride;
            for (int x = 0; x < sw; x++) {
                const uint8_t *px = row + static_cast<size_t>(x) * (bpp / 8);
                uint8_t alpha = bpp == 32 ? px[3] : 255;
                source[static_cast<size_t>(y) * sw + x] = RGBA8(px[2], px[1], px[0], alpha);
            }
        }

        makeThumbnailFromPixels(source, sw, sh, out);
        return out->ok;
    }

    static bool decodeImageThumbnail(const std::string &path, DecodedImage *out) {
        if (endsWithNoCase(path, ".jpg") || endsWithNoCase(path, ".jpeg")) {
            return decodeJpegThumbnail(path, out);
        }
        if (endsWithNoCase(path, ".png")) {
            return decodePngThumbnail(path, out);
        }
        if (endsWithNoCase(path, ".bmp")) {
            return decodeBmpThumbnail(path, out);
        }
        return false;
    }

    bool ensureWorker() {
        if (workerThread_ >= 0) {
            return true;
        }

        workerMutex_ = sceKernelCreateMutex("vitamania_bg_mutex", 0, 1, nullptr);
        if (workerMutex_ < 0) {
            workerMutex_ = -1;
            return false;
        }

        stopRequested_ = false;
        workerThread_ = sceKernelCreateThread("vitamania_bg", workerEntry, 0x78, 0x40000, 0, 0, nullptr);
        if (workerThread_ < 0) {
            sceKernelDeleteMutex(workerMutex_);
            workerMutex_ = -1;
            workerThread_ = -1;
            return false;
        }

        workerArgSelf_ = this;
        int start = sceKernelStartThread(workerThread_, sizeof(workerArgSelf_), &workerArgSelf_);
        if (start < 0) {
            sceKernelDeleteThread(workerThread_);
            sceKernelDeleteMutex(workerMutex_);
            workerThread_ = -1;
            workerMutex_ = -1;
            return false;
        }
        return true;
    }

    void stopWorker() {
        if (workerThread_ >= 0) {
            stopRequested_ = true;
            int status = 0;
            sceKernelWaitThreadEnd(workerThread_, &status, nullptr);
            sceKernelDeleteThread(workerThread_);
            workerThread_ = -1;
        }
        if (workerMutex_ >= 0) {
            sceKernelDeleteMutex(workerMutex_);
            workerMutex_ = -1;
        }
    }

    void requestDecode(const std::string &path) {
        if (workerMutex_ < 0) {
            return;
        }
        sceKernelLockMutex(workerMutex_, 1, nullptr);
        if (requestedPath_ != path) {
            requestedPath_ = path;
            requestSerial_++;
        }
        sceKernelUnlockMutex(workerMutex_, 1);
    }

    void pump() {
        if (workerMutex_ < 0) {
            return;
        }

        DecodedImage image;
        bool hasImage = false;
        sceKernelLockMutex(workerMutex_, 1, nullptr);
        if (resultPending_) {
            image = std::move(result_);
            result_ = DecodedImage{};
            resultPending_ = false;
            hasImage = true;
        }
        sceKernelUnlockMutex(workerMutex_, 1);

        if (!hasImage) {
            return;
        }

        if (image.path == targetPath_) {
            replaceTexture(image);
        }
    }

    static int workerEntry(SceSize args, void *argp) {
        if (args != sizeof(BackgroundManager *) || !argp) {
            return -1;
        }
        BackgroundManager *self = *static_cast<BackgroundManager **>(argp);
        return self ? self->workerMain() : -1;
    }

    int workerMain() {
        uint32_t startedSerial = 0;
        while (!stopRequested_) {
            std::string path;
            uint32_t serial = 0;

            sceKernelLockMutex(workerMutex_, 1, nullptr);
            if (requestSerial_ != startedSerial) {
                path = requestedPath_;
                serial = requestSerial_;
                startedSerial = requestSerial_;
            }
            sceKernelUnlockMutex(workerMutex_, 1);

            if (path.empty()) {
                sceKernelDelayThread(10000);
                continue;
            }

            DecodedImage image;
            image.path = path;
            decodeImageThumbnail(path, &image);

            sceKernelLockMutex(workerMutex_, 1, nullptr);
            if (serial == requestSerial_) {
                result_ = std::move(image);
                resultPending_ = true;
            }
            sceKernelUnlockMutex(workerMutex_, 1);
        }
        return 0;
    }

    vita2d_texture *current_ = nullptr;
    std::string currentPath_;
    std::string targetPath_;
    uint64_t targetChangedAtMs_ = 0;
    uint64_t fadeStartMs_ = 0;
    SceUID workerThread_ = -1;
    SceUID workerMutex_ = -1;
    BackgroundManager *workerArgSelf_ = nullptr;
    volatile bool stopRequested_ = false;
    std::string requestedPath_;
    uint32_t requestSerial_ = 0;
    DecodedImage result_;
    bool resultPending_ = false;
    std::vector<CacheEntry> cache_;
};

class Skin {
public:
    ~Skin() {
        free();
    }

    void load() {
        modeMania = loadPng("mode-mania-med.png");
        menuButton = loadPng("menu-button-background.png");
        stageLeft = loadPng("mania-stage-left.png");
        stageRight = loadPng("mania-stage-right.png");
        stageLight = loadPng("mania-stage-light.png");
        stageHint = loadPng("mania-stage-hint.png");
        warningArrow = loadPng("mania-warningarrow.png");
        scorebar = loadPng("scorebar-colour@2x.png");

        note1 = loadPng("mania-note1.png");
        note1H = loadPng("mania-note1H.png");
        note1L = loadPng("mania-note1L.png");
        note2 = loadPng("mania-note2.png");
        note2H = loadPng("mania-note2H.png");
        note2L = loadPng("mania-note2L.png");
        noteS = loadPng("mania-noteS.png");
        noteSH = loadPng("mania-noteSH.png");
        noteSL = loadPng("mania-noteSL.png");

        key1 = loadPng("mania-key1.png");
        key1D = loadPng("mania-key1D.png");
        key2 = loadPng("mania-key2.png");
        key2D = loadPng("mania-key2D.png");
        keyS = loadPng("mania-keyS.png");
        keySD = loadPng("mania-keySD.png");

        hit0 = loadPng("mania-hit0.png");
        hit50 = loadPng("mania-hit50.png");
        hit100 = loadPng("mania-hit100.png");
        hit200 = loadPng("mania-hit200.png");
        hit300 = loadPng("mania-hit300.png");
        hit300g = loadPng("mania-hit300g-0.png");

        pauseContinue = loadPng("pause-continue.png");
        pauseRetry = loadPng("pause-retry.png");
        pauseBack = loadPng("pause-back.png");
        rankingPanel = loadPng("ranking-panel.png");
        rankingX = loadPng("ranking-X.png");
        rankingS = loadPng("ranking-S.png");
        rankingA = loadPng("ranking-A.png");
        rankingB = loadPng("ranking-B.png");
        rankingC = loadPng("ranking-C.png");
        rankingD = loadPng("ranking-D.png");

        inputCross = loadPng("input-cross.png");
        inputCircle = loadPng("input-circle.png");
        inputSquare = loadPng("input-square.png");
        inputTriangle = loadPng("input-triangle.png");
        inputDpadVertical = loadPng("input-dpad-vertical.png");
        inputDpadHorizontal = loadPng("input-dpad-horizontal.png");
        inputDpadDown = loadPng("input-dpad-down.png");
        inputStart = loadPng("input-start.png");
        inputSelect = loadPng("input-select.png");

        for (int i = 0; i < 10; i++) {
            char name[32];
            std::snprintf(name, sizeof(name), "score-%d.png", i);
            scoreDigits[i] = loadPng(name);
        }
        scoreDot = loadPng("score-dot.png");
        scorePercent = loadPng("score-percent.png");
        scoreX = loadPng("score-x.png");
    }

    void free() {
        vita2d_texture **textures[] = {
            &modeMania, &menuButton, &stageLeft, &stageRight, &stageLight,
            &stageHint, &warningArrow, &scorebar,
            &note1, &note1H, &note1L, &note2, &note2H, &note2L, &noteS, &noteSH, &noteSL,
            &key1, &key1D, &key2, &key2D, &keyS, &keySD,
            &hit0, &hit50, &hit100, &hit200, &hit300, &hit300g,
            &pauseContinue, &pauseRetry, &pauseBack, &rankingPanel,
            &rankingX, &rankingS, &rankingA, &rankingB, &rankingC, &rankingD,
            &scoreDot, &scorePercent, &scoreX,
            &inputCross, &inputCircle, &inputSquare, &inputTriangle,
            &inputDpadVertical, &inputDpadHorizontal, &inputDpadDown, &inputStart, &inputSelect,
        };

        vita2d_wait_rendering_done();
        for (vita2d_texture **texture : textures) {
            if (*texture) {
                vita2d_free_texture(*texture);
                *texture = nullptr;
            }
        }
        for (vita2d_texture *&digit : scoreDigits) {
            if (digit) {
                vita2d_free_texture(digit);
                digit = nullptr;
            }
        }
    }

    vita2d_texture *noteForLane(int lane) const {
        switch (lane) {
        case 0:
        case 3:
            return note1 ? note1 : note2;
        case 1:
        case 2:
            return note2 ? note2 : note1;
        default:
            return noteS ? noteS : note1;
        }
    }

    vita2d_texture *holdHeadForLane(int lane) const {
        switch (lane) {
        case 0:
        case 3:
            return note1H ? note1H : noteForLane(lane);
        case 1:
        case 2:
            return note2H ? note2H : noteForLane(lane);
        default:
            return noteSH ? noteSH : noteForLane(lane);
        }
    }

    vita2d_texture *holdTailForLane(int lane) const {
        switch (lane) {
        case 0:
        case 3:
            return note1L ? note1L : noteForLane(lane);
        case 1:
        case 2:
            return note2L ? note2L : noteForLane(lane);
        default:
            return noteSL ? noteSL : noteForLane(lane);
        }
    }

    vita2d_texture *keyForLane(int lane, bool down) const {
        if (lane == 0 || lane == 3) {
            return down && key1D ? key1D : key1;
        }
        return down && key2D ? key2D : key2;
    }

    vita2d_texture *judgeTexture(const std::string &label) const {
        if (label == "320") {
            return hit300g ? hit300g : hit300;
        }
        if (label == "300") {
            return hit300;
        }
        if (label == "200") {
            return hit200;
        }
        if (label == "100") {
            return hit100;
        }
        if (label == "50") {
            return hit50;
        }
        return hit0;
    }

    vita2d_texture *rankForAccuracy(float acc) const {
        if (acc >= 99.95f) {
            return rankingX ? rankingX : rankingS;
        }
        if (acc >= 95.0f) {
            return rankingS ? rankingS : rankingA;
        }
        if (acc >= 90.0f) {
            return rankingA;
        }
        if (acc >= 80.0f) {
            return rankingB;
        }
        if (acc >= 70.0f) {
            return rankingC;
        }
        return rankingD;
    }

    vita2d_texture *scoreGlyph(char c) const {
        if (c >= '0' && c <= '9') {
            return scoreDigits[c - '0'];
        }
        if (c == '.') {
            return scoreDot;
        }
        if (c == '%') {
            return scorePercent;
        }
        if (c == 'x' || c == 'X') {
            return scoreX;
        }
        return nullptr;
    }

    float scoreStringWidth(const std::string &text, float scale) const {
        float width = 0.0f;
        bool drew = false;
        for (char c : text) {
            if (c == ' ') {
                width += 14.0f * scale;
                continue;
            }
            vita2d_texture *glyph = scoreGlyph(c);
            if (!glyph) {
                continue;
            }
            if (drew) {
                width -= 5.0f * scale;
            }
            width += static_cast<float>(vita2d_texture_get_width(glyph)) * scale;
            drew = true;
        }
        return std::max(0.0f, width);
    }

    void drawScoreString(const std::string &text, float x, float y, float scale, uint32_t tint = RGBA8(255, 255, 255, 255)) const {
        bool drew = false;
        for (char c : text) {
            if (c == ' ') {
                x += 14.0f * scale;
                continue;
            }
            vita2d_texture *glyph = scoreGlyph(c);
            if (!glyph) {
                continue;
            }
            if (drew) {
                x -= 5.0f * scale;
            }
            vita2d_draw_texture_tint_scale(glyph, x, y, scale, scale, tint);
            x += static_cast<float>(vita2d_texture_get_width(glyph)) * scale;
            drew = true;
        }
    }

    void drawScoreRight(const std::string &text, float right, float y, float scale, uint32_t tint = RGBA8(255, 255, 255, 255)) const {
        drawScoreString(text, right - scoreStringWidth(text, scale), y, scale, tint);
    }

    void drawScoreCentered(const std::string &text, float cx, float y, float scale, uint32_t tint = RGBA8(255, 255, 255, 255)) const {
        drawScoreString(text, cx - scoreStringWidth(text, scale) * 0.5f, y, scale, tint);
    }

    static void drawFit(vita2d_texture *texture, float x, float y, float w, float h, uint32_t tint = RGBA8(255, 255, 255, 255)) {
        if (!texture) {
            return;
        }
        float tw = static_cast<float>(vita2d_texture_get_width(texture));
        float th = static_cast<float>(vita2d_texture_get_height(texture));
        if (tw <= 0.0f || th <= 0.0f) {
            return;
        }
        vita2d_draw_texture_tint_scale(texture, x, y, w / tw, h / th, tint);
    }

    static void drawCentered(vita2d_texture *texture, float cx, float cy, float maxW, float maxH, uint32_t tint = RGBA8(255, 255, 255, 255)) {
        if (!texture) {
            return;
        }
        float tw = static_cast<float>(vita2d_texture_get_width(texture));
        float th = static_cast<float>(vita2d_texture_get_height(texture));
        if (tw <= 0.0f || th <= 0.0f) {
            return;
        }
        float scale = std::min(maxW / tw, maxH / th);
        float w = tw * scale;
        float h = th * scale;
        vita2d_draw_texture_tint_scale(texture, cx - w * 0.5f, cy - h * 0.5f, scale, scale, tint);
    }

    vita2d_texture *modeMania = nullptr;
    vita2d_texture *menuButton = nullptr;
    vita2d_texture *stageLeft = nullptr;
    vita2d_texture *stageRight = nullptr;
    vita2d_texture *stageLight = nullptr;
    vita2d_texture *stageHint = nullptr;
    vita2d_texture *warningArrow = nullptr;
    vita2d_texture *scorebar = nullptr;
    vita2d_texture *note1 = nullptr;
    vita2d_texture *note1H = nullptr;
    vita2d_texture *note1L = nullptr;
    vita2d_texture *note2 = nullptr;
    vita2d_texture *note2H = nullptr;
    vita2d_texture *note2L = nullptr;
    vita2d_texture *noteS = nullptr;
    vita2d_texture *noteSH = nullptr;
    vita2d_texture *noteSL = nullptr;
    vita2d_texture *key1 = nullptr;
    vita2d_texture *key1D = nullptr;
    vita2d_texture *key2 = nullptr;
    vita2d_texture *key2D = nullptr;
    vita2d_texture *keyS = nullptr;
    vita2d_texture *keySD = nullptr;
    vita2d_texture *hit0 = nullptr;
    vita2d_texture *hit50 = nullptr;
    vita2d_texture *hit100 = nullptr;
    vita2d_texture *hit200 = nullptr;
    vita2d_texture *hit300 = nullptr;
    vita2d_texture *hit300g = nullptr;
    vita2d_texture *pauseContinue = nullptr;
    vita2d_texture *pauseRetry = nullptr;
    vita2d_texture *pauseBack = nullptr;
    vita2d_texture *rankingPanel = nullptr;
    vita2d_texture *rankingX = nullptr;
    vita2d_texture *rankingS = nullptr;
    vita2d_texture *rankingA = nullptr;
    vita2d_texture *rankingB = nullptr;
    vita2d_texture *rankingC = nullptr;
    vita2d_texture *rankingD = nullptr;
    vita2d_texture *scoreDigits[10] = {};
    vita2d_texture *scoreDot = nullptr;
    vita2d_texture *scorePercent = nullptr;
    vita2d_texture *scoreX = nullptr;
    vita2d_texture *inputCross = nullptr;
    vita2d_texture *inputCircle = nullptr;
    vita2d_texture *inputSquare = nullptr;
    vita2d_texture *inputTriangle = nullptr;
    vita2d_texture *inputDpadVertical = nullptr;
    vita2d_texture *inputDpadHorizontal = nullptr;
    vita2d_texture *inputDpadDown = nullptr;
    vita2d_texture *inputStart = nullptr;
    vita2d_texture *inputSelect = nullptr;

private:
    static vita2d_texture *loadPng(const char *name) {
        std::string path = joinPath(SKIN_DIR, name);
        return vita2d_load_PNG_file(path.c_str());
    }
};

class SampleEngine {
public:
    bool start() {
#ifdef VITAMANIA_NO_AVPLAYER
        return false;
#else
        if (running_) {
            return true;
        }

        mutex_ = sceKernelCreateMutex("vitamania_samples_mutex", 0, 1, nullptr);
        if (mutex_ < 0) {
            mutex_ = -1;
            return false;
        }

        stopRequested_ = false;
        thread_ = sceKernelCreateThread("vitamania_samples", threadEntry, 0x60, 0x10000, 0, 0, nullptr);
        if (thread_ < 0) {
            sceKernelDeleteMutex(mutex_);
            mutex_ = -1;
            thread_ = -1;
            return false;
        }

        threadArgSelf_ = this;
        int res = sceKernelStartThread(thread_, sizeof(threadArgSelf_), &threadArgSelf_);
        if (res < 0) {
            sceKernelDeleteThread(thread_);
            sceKernelDeleteMutex(mutex_);
            thread_ = -1;
            mutex_ = -1;
            return false;
        }

        running_ = true;
        return true;
#endif
    }

    void stop() {
#ifndef VITAMANIA_NO_AVPLAYER
        if (thread_ >= 0) {
            stopRequested_ = true;
            int status = 0;
            sceKernelWaitThreadEnd(thread_, &status, nullptr);
            sceKernelDeleteThread(thread_);
            thread_ = -1;
        }
        if (mutex_ >= 0) {
            sceKernelDeleteMutex(mutex_);
            mutex_ = -1;
        }
        running_ = false;
        cache_.clear();
#endif
    }

    void setMasterVolume(int volume) {
        masterVolume_ = clampi(volume, 0, 100);
    }

    void clearVoices() {
#ifndef VITAMANIA_NO_AVPLAYER
        if (mutex_ >= 0) {
            sceKernelLockMutex(mutex_, 1, nullptr);
            for (Voice &voice : voices_) {
                voice.active = false;
            }
            sceKernelUnlockMutex(mutex_, 1);
        }
#endif
    }

    void preload(const Beatmap &map) {
#ifdef VITAMANIA_NO_AVPLAYER
        (void)map;
#else
        std::vector<std::string> paths;
        for (const Note &note : map.notes) {
            if (!note.samplePath.empty() && std::find(paths.begin(), paths.end(), note.samplePath) == paths.end()) {
                paths.push_back(note.samplePath);
            }
        }
        for (const TimedSample &sample : map.storyboardSamples) {
            if (!sample.path.empty() && std::find(paths.begin(), paths.end(), sample.path) == paths.end()) {
                paths.push_back(sample.path);
            }
        }
        for (const std::string &path : paths) {
            sampleIndexFor(path);
        }
#endif
    }

    void play(const std::string &path, int volume) {
#ifdef VITAMANIA_NO_AVPLAYER
        (void)path;
        (void)volume;
#else
        if (!running_ || path.empty() || volume <= 0 || masterVolume_ <= 0) {
            return;
        }

        int sampleIndex = sampleIndexFor(path);
        if (sampleIndex < 0) {
            return;
        }

        sceKernelLockMutex(mutex_, 1, nullptr);
        int voiceIndex = -1;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (!voices_[i].active) {
                voiceIndex = i;
                break;
            }
        }
        if (voiceIndex < 0) {
            voiceIndex = 0;
        }

        const Sample &sample = cache_[sampleIndex].sample;
        voices_[voiceIndex].sampleIndex = sampleIndex;
        voices_[voiceIndex].pos = 0;
        voices_[voiceIndex].step = std::max(1U, static_cast<uint32_t>((static_cast<uint64_t>(sample.sampleRate) << 16) / OUTPUT_RATE));
        voices_[voiceIndex].volume = clampi(volume, 0, 100);
        voices_[voiceIndex].active = true;
        sceKernelUnlockMutex(mutex_, 1);
#endif
    }

private:
#ifndef VITAMANIA_NO_AVPLAYER
    static constexpr int OUTPUT_RATE = 44100;
    static constexpr int BUFFER_SAMPLES = 512;
    static constexpr int MAX_VOICES = 16;

    struct Sample {
        int sampleRate = OUTPUT_RATE;
        int channels = 2;
        std::vector<int16_t> pcm;
    };

    struct CachedSample {
        std::string path;
        Sample sample;
    };

    struct Voice {
        int sampleIndex = -1;
        uint32_t pos = 0;
        uint32_t step = 1 << 16;
        int volume = 100;
        bool active = false;
    };

    static uint16_t readLe16(const uint8_t *p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    static uint32_t readLe32(const uint8_t *p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    static bool readFile(const std::string &path, std::vector<uint8_t> *out) {
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        std::fseek(file, 0, SEEK_END);
        long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(file);
            return false;
        }
        out->resize(static_cast<size_t>(size));
        size_t read = std::fread(out->data(), 1, out->size(), file);
        std::fclose(file);
        return read == out->size();
    }

    static bool loadWav(const std::string &path, Sample *sample) {
        std::vector<uint8_t> data;
        if (!readFile(path, &data) || data.size() < 44) {
            return false;
        }
        if (std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
            return false;
        }

        bool haveFmt = false;
        bool haveData = false;
        uint16_t audioFormat = 0;
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        size_t dataOffset = 0;
        size_t dataSize = 0;

        size_t pos = 12;
        while (pos + 8 <= data.size()) {
            const uint8_t *chunk = data.data() + pos;
            uint32_t size = readLe32(chunk + 4);
            size_t payload = pos + 8;
            if (payload + size > data.size()) {
                break;
            }

            if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
                audioFormat = readLe16(data.data() + payload);
                channels = readLe16(data.data() + payload + 2);
                sampleRate = readLe32(data.data() + payload + 4);
                bitsPerSample = readLe16(data.data() + payload + 14);
                haveFmt = true;
            } else if (std::memcmp(chunk, "data", 4) == 0) {
                dataOffset = payload;
                dataSize = size;
                haveData = true;
            }

            pos = payload + size + (size & 1);
        }

        if (!haveFmt || !haveData || audioFormat != 1 || (channels != 1 && channels != 2) ||
            bitsPerSample != 16 || sampleRate == 0 || dataSize < channels * 2) {
            return false;
        }

        size_t sampleCount = dataSize / 2;
        sample->sampleRate = static_cast<int>(sampleRate);
        sample->channels = static_cast<int>(channels);
        sample->pcm.resize(sampleCount);
        std::memcpy(sample->pcm.data(), data.data() + dataOffset, sampleCount * 2);
        return true;
    }

    int sampleIndexFor(const std::string &path) {
        sceKernelLockMutex(mutex_, 1, nullptr);
        for (int i = 0; i < static_cast<int>(cache_.size()); i++) {
            if (cache_[i].path == path) {
                sceKernelUnlockMutex(mutex_, 1);
                return i;
            }
        }
        sceKernelUnlockMutex(mutex_, 1);

        if (!endsWithNoCase(path, ".wav")) {
            return -1;
        }

        Sample sample;
        if (!loadWav(path, &sample)) {
            return -1;
        }

        sceKernelLockMutex(mutex_, 1, nullptr);
        for (int i = 0; i < static_cast<int>(cache_.size()); i++) {
            if (cache_[i].path == path) {
                sceKernelUnlockMutex(mutex_, 1);
                return i;
            }
        }

        CachedSample cached;
        cached.path = path;
        cached.sample = sample;
        cache_.push_back(cached);
        int index = static_cast<int>(cache_.size()) - 1;
        sceKernelUnlockMutex(mutex_, 1);
        return index;
    }

    static int threadEntry(SceSize args, void *argp) {
        if (args != sizeof(SampleEngine *) || !argp) {
            return -1;
        }
        SampleEngine *self = *static_cast<SampleEngine **>(argp);
        return self ? self->threadMain() : -1;
    }

    int threadMain() {
        int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, BUFFER_SAMPLES, OUTPUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0) {
            return port;
        }

        int16_t out[BUFFER_SAMPLES * 2];
        int32_t mix[BUFFER_SAMPLES * 2];

        while (!stopRequested_) {
            std::memset(mix, 0, sizeof(mix));

            sceKernelLockMutex(mutex_, 1, nullptr);
            for (Voice &voice : voices_) {
                if (!voice.active || voice.sampleIndex < 0 || voice.sampleIndex >= static_cast<int>(cache_.size())) {
                    continue;
                }

                const Sample &sample = cache_[voice.sampleIndex].sample;
                int frames = static_cast<int>(sample.pcm.size()) / sample.channels;
                int gain = clampi(voice.volume, 0, 100) * clampi(masterVolume_, 0, 100);
                for (int i = 0; i < BUFFER_SAMPLES; i++) {
                    int frame = static_cast<int>(voice.pos >> 16);
                    if (frame >= frames) {
                        voice.active = false;
                        break;
                    }

                    int base = frame * sample.channels;
                    int left = sample.pcm[base];
                    int right = sample.channels == 2 ? sample.pcm[base + 1] : left;
                    mix[i * 2] += (left * gain) / 10000;
                    mix[i * 2 + 1] += (right * gain) / 10000;
                    voice.pos += voice.step;
                }
            }
            sceKernelUnlockMutex(mutex_, 1);

            for (int i = 0; i < BUFFER_SAMPLES * 2; i++) {
                out[i] = static_cast<int16_t>(clampi(mix[i], -32768, 32767));
            }
            sceAudioOutOutput(port, out);
        }

        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
        return 0;
    }

    SceUID thread_ = -1;
    SceUID mutex_ = -1;
    SampleEngine *threadArgSelf_ = nullptr;
    volatile bool stopRequested_ = false;
    bool running_ = false;
    int masterVolume_ = 80;
    Voice voices_[MAX_VOICES];
    std::vector<CachedSample> cache_;
#else
    int masterVolume_ = 80;
#endif
};

class AudioPreloadCache {
public:
    ~AudioPreloadCache() {
        stop();
    }

    bool start() {
        if (thread_ >= 0) {
            return true;
        }

        mutex_ = sceKernelCreateMutex("vitamania_preview_cache", 0, 1, nullptr);
        if (mutex_ < 0) {
            mutex_ = -1;
            return false;
        }

        stopRequested_ = false;
        thread_ = sceKernelCreateThread("vitamania_preview_cache", threadEntry, 0x70, 0x10000, 0, 0, nullptr);
        if (thread_ < 0) {
            sceKernelDeleteMutex(mutex_);
            mutex_ = -1;
            thread_ = -1;
            return false;
        }

        threadArgSelf_ = this;
        int startResult = sceKernelStartThread(thread_, sizeof(threadArgSelf_), &threadArgSelf_);
        if (startResult < 0) {
            sceKernelDeleteThread(thread_);
            sceKernelDeleteMutex(mutex_);
            thread_ = -1;
            mutex_ = -1;
            return false;
        }

        return true;
    }

    void stop() {
        if (thread_ >= 0) {
            stopRequested_ = true;
            int status = 0;
            sceKernelWaitThreadEnd(thread_, &status, nullptr);
            sceKernelDeleteThread(thread_);
            thread_ = -1;
        }

        if (mutex_ >= 0) {
            sceKernelLockMutex(mutex_, 1, nullptr);
            requestedPath_.clear();
            cachedPath_.clear();
            cachedData_.clear();
            sceKernelUnlockMutex(mutex_, 1);
            sceKernelDeleteMutex(mutex_);
            mutex_ = -1;
        }
    }

    void request(const std::string &path) {
        if (path.empty() || !endsWithNoCase(path, ".mp3") || !start()) {
            return;
        }

        sceKernelLockMutex(mutex_, 1, nullptr);
        if (cachedPath_ == path && !cachedData_.empty()) {
            sceKernelUnlockMutex(mutex_, 1);
            return;
        }
        if (requestedPath_ != path) {
            requestedPath_ = path;
            requestSerial_++;
        }
        sceKernelUnlockMutex(mutex_, 1);
    }

    bool ready(const std::string &path) {
        if (path.empty() || mutex_ < 0) {
            return false;
        }

        sceKernelLockMutex(mutex_, 1, nullptr);
        bool hit = cachedPath_ == path && !cachedData_.empty();
        sceKernelUnlockMutex(mutex_, 1);
        return hit;
    }

    bool get(const std::string &path, std::vector<uint8_t> *out) {
        if (!out || path.empty() || mutex_ < 0) {
            return false;
        }

        sceKernelLockMutex(mutex_, 1, nullptr);
        bool hit = cachedPath_ == path && !cachedData_.empty();
        if (hit) {
            *out = cachedData_;
        }
        sceKernelUnlockMutex(mutex_, 1);
        return hit;
    }

private:
    static bool readFile(const std::string &path, std::vector<uint8_t> *out) {
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        std::fseek(file, 0, SEEK_END);
        long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(file);
            return false;
        }
        out->resize(static_cast<size_t>(size));
        size_t read = std::fread(out->data(), 1, out->size(), file);
        std::fclose(file);
        return read == out->size();
    }

    static int threadEntry(SceSize args, void *argp) {
        if (args != sizeof(AudioPreloadCache *) || !argp) {
            return -1;
        }
        AudioPreloadCache *self = *static_cast<AudioPreloadCache **>(argp);
        return self ? self->threadMain() : -1;
    }

    int threadMain() {
        uint32_t startedSerial = 0;
        while (!stopRequested_) {
            std::string path;
            uint32_t serial = 0;

            sceKernelLockMutex(mutex_, 1, nullptr);
            if (requestSerial_ != startedSerial) {
                path = requestedPath_;
                serial = requestSerial_;
                startedSerial = requestSerial_;
            }
            sceKernelUnlockMutex(mutex_, 1);

            if (path.empty()) {
                sceKernelDelayThread(10000);
                continue;
            }

            std::vector<uint8_t> data;
            bool ok = readFile(path, &data);

            sceKernelLockMutex(mutex_, 1, nullptr);
            if (serial == requestSerial_) {
                cachedPath_ = path;
                cachedData_ = ok ? std::move(data) : std::vector<uint8_t>{};
            }
            sceKernelUnlockMutex(mutex_, 1);
        }
        return 0;
    }

    SceUID thread_ = -1;
    SceUID mutex_ = -1;
    AudioPreloadCache *threadArgSelf_ = nullptr;
    volatile bool stopRequested_ = false;
    std::string requestedPath_;
    uint32_t requestSerial_ = 0;
    std::string cachedPath_;
    std::vector<uint8_t> cachedData_;
};

class AudioPlayer {
public:
    void setPreloadCache(AudioPreloadCache *cache) {
        preloadCache_ = cache;
    }

    void preloadForPlay(const std::string &path) {
#ifndef VITAMANIA_NO_AVPLAYER
        preloadedAudioPath_.clear();
        preloadedAudioData_.clear();
        if (path.empty() || (!endsWithNoCase(path, ".mp3") && !endsWithNoCase(path, ".ogg"))) {
            return;
        }

        std::vector<uint8_t> data;
        if (readFile(path, &data)) {
            preloadedAudioPath_ = path;
            preloadedAudioData_ = std::move(data);
        }
#else
        (void)path;
#endif
    }

    bool play(const std::string &path, std::string *error, int startOffsetMs = 0) {
#ifdef VITAMANIA_NO_AVPLAYER
        (void)path;
        (void)startOffsetMs;
        stop();
        if (error) {
            *error = "No-audio build, using silent clock";
        }
        return false;
#else
        stop();
        if (path.empty()) {
            if (error) {
                *error = "No audio file in beatmap";
            }
            return false;
        }

        startOffsetMs_ = std::max(0, startOffsetMs);
        if (endsWithNoCase(path, ".mp3")) {
            return playMp3(path, error);
        }
        if (endsWithNoCase(path, ".ogg")) {
            return playOgg(path, error);
        }

        return playAvPlayer(path, error);
#endif
    }

    void stop() {
#ifndef VITAMANIA_NO_AVPLAYER
        stopMp3();
        stopOgg();
        stopAvPlayer();
#endif
        active_ = false;
        startedAtMs_ = 0;
        pauseStartedAtMs_ = 0;
        pausedAccumMs_ = 0;
        startOffsetMs_ = 0;
    }

    bool active() const {
        return active_;
    }

    void setMusicVolume(int volume) {
        musicVolume_ = clampi(volume, 0, 100);
    }

    void pause() {
#ifndef VITAMANIA_NO_AVPLAYER
        if (mp3Active_ && !mp3Paused_) {
            mp3Paused_ = true;
            pauseStartedAtMs_ = nowMs();
        }
        if (oggActive_ && !oggPaused_) {
            oggPaused_ = true;
            pauseStartedAtMs_ = nowMs();
        }
        if (avHandle_ >= 0) {
            sceAvPlayerPause(avHandle_);
        }
#endif
    }

    void resume() {
#ifndef VITAMANIA_NO_AVPLAYER
        if (mp3Active_ && mp3Paused_) {
            uint64_t now = nowMs();
            if (pauseStartedAtMs_ > 0 && now > pauseStartedAtMs_) {
                pausedAccumMs_ += now - pauseStartedAtMs_;
            }
            pauseStartedAtMs_ = 0;
            mp3Paused_ = false;
        }
        if (oggActive_ && oggPaused_) {
            uint64_t now = nowMs();
            if (pauseStartedAtMs_ > 0 && now > pauseStartedAtMs_) {
                pausedAccumMs_ += now - pauseStartedAtMs_;
            }
            pauseStartedAtMs_ = 0;
            oggPaused_ = false;
        }
        if (avHandle_ >= 0) {
            sceAvPlayerResume(avHandle_);
        }
#endif
    }

    int currentMs() const {
#ifndef VITAMANIA_NO_AVPLAYER
        if (mp3Active_) {
            if (startedAtMs_ == 0) {
                return 0;
            }
            uint64_t clock = mp3Paused_ && pauseStartedAtMs_ > 0 ? pauseStartedAtMs_ : nowMs();
            if (clock <= startedAtMs_ + pausedAccumMs_) {
                return 0;
            }
            return static_cast<int>(clock - startedAtMs_ - pausedAccumMs_);
        }

        if (oggActive_) {
            if (startedAtMs_ == 0) {
                return 0;
            }
            uint64_t clock = oggPaused_ && pauseStartedAtMs_ > 0 ? pauseStartedAtMs_ : nowMs();
            if (clock <= startedAtMs_ + pausedAccumMs_) {
                return 0;
            }
            return static_cast<int>(clock - startedAtMs_ - pausedAccumMs_);
        }

        if (avHandle_ >= 0) {
            uint64_t t = sceAvPlayerCurrentTime(avHandle_);
            if (t > 0 || nowMs() - startedAtMs_ > 250) {
                return static_cast<int>(t);
            }
        }
#endif
        if (startedAtMs_ == 0) {
            return 0;
        }
        return static_cast<int>(nowMs() - startedAtMs_);
    }

private:
#ifndef VITAMANIA_NO_AVPLAYER
    struct Mp3Frame {
        size_t offset = 0;
        int size = 0;
        int sampleRate = 0;
        int channels = 0;
        int version = 0;
        int samples = 0;
    };

    static size_t skipId3v2(const std::vector<uint8_t> &data) {
        if (data.size() < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') {
            return 0;
        }

        size_t size = (static_cast<size_t>(data[6] & 0x7F) << 21) |
                      (static_cast<size_t>(data[7] & 0x7F) << 14) |
                      (static_cast<size_t>(data[8] & 0x7F) << 7) |
                      static_cast<size_t>(data[9] & 0x7F);
        if (data[5] & 0x10) {
            size += 10;
        }
        return std::min<size_t>(data.size(), 10 + size);
    }

    static bool parseMp3FrameAt(const std::vector<uint8_t> &data, size_t offset, Mp3Frame *frame) {
        if (offset + 4 > data.size()) {
            return false;
        }

        uint32_t h = (static_cast<uint32_t>(data[offset]) << 24) |
                     (static_cast<uint32_t>(data[offset + 1]) << 16) |
                     (static_cast<uint32_t>(data[offset + 2]) << 8) |
                     static_cast<uint32_t>(data[offset + 3]);

        if ((h & 0xFFE00000U) != 0xFFE00000U) {
            return false;
        }

        int version = static_cast<int>((h >> 19) & 0x3);
        int layer = static_cast<int>((h >> 17) & 0x3);
        int bitrateIndex = static_cast<int>((h >> 12) & 0xF);
        int sampleIndex = static_cast<int>((h >> 10) & 0x3);
        int padding = static_cast<int>((h >> 9) & 0x1);
        int channelMode = static_cast<int>((h >> 6) & 0x3);

        if (version == SCE_AUDIODEC_MP3_MPEG_VERSION_RESERVED || layer != 1 ||
            bitrateIndex == 0 || bitrateIndex == 15 || sampleIndex == 3) {
            return false;
        }

        static const int bitrateMpeg1[16] = {
            0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0,
        };
        static const int bitrateMpeg2[16] = {
            0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,
        };
        static const int rates[4][3] = {
            {11025, 12000, 8000},
            {0, 0, 0},
            {22050, 24000, 16000},
            {44100, 48000, 32000},
        };

        int bitrate = version == SCE_AUDIODEC_MP3_MPEG_VERSION_1
                          ? bitrateMpeg1[bitrateIndex]
                          : bitrateMpeg2[bitrateIndex];
        int sampleRate = rates[version][sampleIndex];
        if (bitrate <= 0 || sampleRate <= 0) {
            return false;
        }

        int frameSize = 0;
        int samples = 0;
        if (version == SCE_AUDIODEC_MP3_MPEG_VERSION_1) {
            frameSize = (144000 * bitrate) / sampleRate + padding;
            samples = 1152;
        } else {
            frameSize = (72000 * bitrate) / sampleRate + padding;
            samples = 576;
        }

        if (frameSize <= 4 || frameSize > SCE_AUDIODEC_MP3_MAX_ES_SIZE ||
            offset + static_cast<size_t>(frameSize) > data.size()) {
            return false;
        }

        frame->offset = offset;
        frame->size = frameSize;
        frame->sampleRate = sampleRate;
        frame->channels = channelMode == 3 ? 1 : 2;
        frame->version = version;
        frame->samples = samples;
        return true;
    }

    static bool findNextMp3Frame(const std::vector<uint8_t> &data, size_t start, Mp3Frame *frame) {
        for (size_t pos = start; pos + 4 <= data.size(); pos++) {
            if (parseMp3FrameAt(data, pos, frame)) {
                return true;
            }
        }
        return false;
    }

    static bool readFile(const std::string &path, std::vector<uint8_t> *out) {
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        std::fseek(file, 0, SEEK_END);
        long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(file);
            return false;
        }
        out->resize(static_cast<size_t>(size));
        size_t read = std::fread(out->data(), 1, out->size(), file);
        std::fclose(file);
        return read == out->size();
    }

    bool playMp3(const std::string &path, std::string *error) {
        if (preloadedAudioPath_ == path && !preloadedAudioData_.empty()) {
            mp3Data_ = std::move(preloadedAudioData_);
            preloadedAudioPath_.clear();
        } else if ((!preloadCache_ || !preloadCache_->get(path, &mp3Data_)) && !readFile(path, &mp3Data_)) {
            if (error) {
                *error = "Could not read MP3";
            }
            return false;
        }

        Mp3Frame first;
        if (!findNextMp3Frame(mp3Data_, skipId3v2(mp3Data_), &first)) {
            mp3Data_.clear();
            if (error) {
                *error = "No MP3 frames found";
            }
            return false;
        }

        SceAudiodecInitParam init{};
        init.mp3.size = sizeof(SceAudiodecInitStreamParam);
        init.mp3.totalStreams = 1;
        int initResult = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &init);
        if (initResult < 0 && static_cast<uint32_t>(initResult) != SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED) {
            mp3Data_.clear();
            if (error) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "MP3 init failed: 0x%08X", initResult);
                *error = buf;
            }
            return false;
        }
        mp3LibraryReady_ = true;
        mp3LibraryOwned_ = initResult >= 0;

        mp3StopRequested_ = false;
        mp3Paused_ = false;
        pauseStartedAtMs_ = 0;
        pausedAccumMs_ = 0;
        mp3Thread_ = sceKernelCreateThread("vitamania_mp3", mp3ThreadEntry, 0x40, 0x10000, 0, 0, nullptr);
        if (mp3Thread_ < 0) {
            termMp3Library();
            mp3Data_.clear();
            if (error) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "MP3 thread failed: 0x%08X", mp3Thread_);
                *error = buf;
            }
            mp3Thread_ = -1;
            return false;
        }

        threadArgSelf_ = this;
        int start = sceKernelStartThread(mp3Thread_, sizeof(threadArgSelf_), &threadArgSelf_);
        if (start < 0) {
            sceKernelDeleteThread(mp3Thread_);
            mp3Thread_ = -1;
            termMp3Library();
            mp3Data_.clear();
            if (error) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "MP3 start failed: 0x%08X", start);
                *error = buf;
            }
            return false;
        }

        (void)first;
        mp3Active_ = true;
        active_ = true;
        startedAtMs_ = 0;
        return true;
    }

    bool playOgg(const std::string &path, std::string *error) {
        oggPath_ = path;
        if (preloadedAudioPath_ == path && !preloadedAudioData_.empty()) {
            oggData_ = std::move(preloadedAudioData_);
            preloadedAudioPath_.clear();
        } else {
            oggData_.clear();
        }
        oggStopRequested_ = false;
        oggPaused_ = false;
        pauseStartedAtMs_ = 0;
        pausedAccumMs_ = 0;

        oggThread_ = sceKernelCreateThread("vitamania_ogg", oggThreadEntry, 0x48, 0x18000, 0, 0, nullptr);
        if (oggThread_ < 0) {
            if (error) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "OGG thread failed: 0x%08X", oggThread_);
                *error = buf;
            }
            oggThread_ = -1;
            oggPath_.clear();
            return false;
        }

        threadArgSelf_ = this;
        int start = sceKernelStartThread(oggThread_, sizeof(threadArgSelf_), &threadArgSelf_);
        if (start < 0) {
            sceKernelDeleteThread(oggThread_);
            oggThread_ = -1;
            oggPath_.clear();
            if (error) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "OGG start failed: 0x%08X", start);
                *error = buf;
            }
            return false;
        }

        oggActive_ = true;
        active_ = true;
        startedAtMs_ = 0;
        return true;
    }

    bool playAvPlayer(const std::string &path, std::string *error) {
        int mod = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        if (mod < 0 && mod != static_cast<int>(0x805a1003)) {
            if (error) {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "AvPlayer module load failed: 0x%08X", mod);
                *error = buf;
            }
            return false;
        }

        SceAvPlayerInitData init{};
        init.debugLevel = 0;
        init.basePriority = 0xA0;
        init.numOutputVideoFrameBuffers = 0;
        init.autoStart = SCE_FALSE;
        init.defaultLanguage = "en";

        avHandle_ = sceAvPlayerInit(&init);
        if (avHandle_ < 0) {
            if (error) {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "AvPlayer init failed: 0x%08X", avHandle_);
                *error = buf;
            }
            avHandle_ = -1;
            return false;
        }

        int add = sceAvPlayerAddSource(avHandle_, path.c_str());
        if (add < 0) {
            if (error) {
                char buf[100];
                std::snprintf(buf, sizeof(buf), "AvPlayer rejected audio: 0x%08X", add);
                *error = buf;
            }
            stop();
            return false;
        }

        int start = sceAvPlayerStart(avHandle_);
        if (start < 0) {
            if (error) {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "AvPlayer start failed: 0x%08X", start);
                *error = buf;
            }
            stop();
            return false;
        }

        startedAtMs_ = nowMs();
        pauseStartedAtMs_ = 0;
        pausedAccumMs_ = 0;
        active_ = true;
        return true;
    }

    void stopAvPlayer() {
        if (avHandle_ >= 0) {
            sceAvPlayerStop(avHandle_);
            sceAvPlayerClose(avHandle_);
        }
        avHandle_ = -1;
    }

    void stopMp3() {
        if (mp3Thread_ >= 0) {
            mp3StopRequested_ = true;
            int status = 0;
            sceKernelWaitThreadEnd(mp3Thread_, &status, nullptr);
            sceKernelDeleteThread(mp3Thread_);
            mp3Thread_ = -1;
        }
        mp3Active_ = false;
        mp3Paused_ = false;
        mp3Data_.clear();
        termMp3Library();
    }

    void stopOgg() {
        if (oggThread_ >= 0) {
            oggStopRequested_ = true;
            int status = 0;
            sceKernelWaitThreadEnd(oggThread_, &status, nullptr);
            sceKernelDeleteThread(oggThread_);
            oggThread_ = -1;
        }
        oggActive_ = false;
        oggPaused_ = false;
        oggPath_.clear();
        oggData_.clear();
    }

    static int mp3ThreadEntry(SceSize args, void *argp) {
        if (args != sizeof(AudioPlayer *) || !argp) {
            return -1;
        }
        AudioPlayer *self = *static_cast<AudioPlayer **>(argp);
        if (!self) {
            return -1;
        }
        return self->mp3ThreadMain();
    }

    static int oggThreadEntry(SceSize args, void *argp) {
        if (args != sizeof(AudioPlayer *) || !argp) {
            return -1;
        }
        AudioPlayer *self = *static_cast<AudioPlayer **>(argp);
        if (!self) {
            return -1;
        }
        return self->oggThreadMain();
    }

    int oggThreadMain() {
        int error = 0;
        stb_vorbis *vorbis = nullptr;
        if (!oggData_.empty()) {
            vorbis = stb_vorbis_open_memory(oggData_.data(), static_cast<int>(oggData_.size()), &error, nullptr);
        } else {
            vorbis = stb_vorbis_open_filename(oggPath_.c_str(), &error, nullptr);
        }
        if (!vorbis) {
            oggActive_ = false;
            active_ = false;
            return error ? -error : -1;
        }

        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        int outChannels = info.channels == 1 ? 1 : 2;
        int mode = outChannels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
        int sampleRate = static_cast<int>(info.sample_rate);
        constexpr int bufferSamples = 1024;
        int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, bufferSamples, sampleRate,
                                       static_cast<SceAudioOutMode>(mode));
        if (port < 0 && sampleRate == 48000) {
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, bufferSamples, sampleRate,
                                       static_cast<SceAudioOutMode>(mode));
        }
        if (port < 0) {
            stb_vorbis_close(vorbis);
            oggActive_ = false;
            active_ = false;
            return port;
        }

        if (startOffsetMs_ > 0) {
            uint64_t sampleOffset = (static_cast<uint64_t>(startOffsetMs_) * static_cast<uint64_t>(sampleRate)) / 1000ULL;
            stb_vorbis_seek(vorbis, static_cast<unsigned int>(std::min<uint64_t>(sampleOffset, 0xFFFFFFFFULL)));
        }

        std::vector<int16_t> pcm(static_cast<size_t>(bufferSamples * outChannels));
        int lastAppliedVolume = -1;
        while (!oggStopRequested_) {
            if (oggPaused_) {
                sceKernelDelayThread(10000);
                continue;
            }

            int samples = stb_vorbis_get_samples_short_interleaved(vorbis, outChannels, pcm.data(), bufferSamples * outChannels);
            if (samples <= 0) {
                break;
            }

            if (samples < bufferSamples) {
                std::memset(pcm.data() + samples * outChannels, 0,
                            static_cast<size_t>((bufferSamples - samples) * outChannels * sizeof(int16_t)));
            }

            if (startedAtMs_ == 0) {
                startedAtMs_ = nowMs();
                if (startOffsetMs_ > 0 && startedAtMs_ > static_cast<uint64_t>(startOffsetMs_)) {
                    startedAtMs_ -= static_cast<uint64_t>(startOffsetMs_);
                }
            }
            if (lastAppliedVolume != musicVolume_) {
                int volume = (SCE_AUDIO_OUT_MAX_VOL * clampi(musicVolume_, 0, 100)) / 100;
                int volumes[2] = {volume, volume};
                sceAudioOutSetVolume(port,
                                     static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                                     volumes);
                lastAppliedVolume = musicVolume_;
            }
            sceAudioOutOutput(port, pcm.data());
        }

        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
        stb_vorbis_close(vorbis);
        oggActive_ = false;
        active_ = false;
        return 0;
    }

    int mp3ThreadMain() {
        Mp3Frame first;
        if (!findNextMp3Frame(mp3Data_, skipId3v2(mp3Data_), &first)) {
            mp3Active_ = false;
            active_ = false;
            termMp3Library();
            return -1;
        }

        int mode = first.channels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
        int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, first.samples, first.sampleRate,
                                       static_cast<SceAudioOutMode>(mode));
        if (port < 0 && first.sampleRate == 48000) {
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, first.samples, first.sampleRate,
                                       static_cast<SceAudioOutMode>(mode));
        }
        if (port < 0) {
            mp3Active_ = false;
            active_ = false;
            termMp3Library();
            return port;
        }

        int lastAppliedVolume = -1;
        uint8_t *es = static_cast<uint8_t *>(memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(SCE_AUDIODEC_MP3_MAX_ES_SIZE)));
        uint8_t *pcm = static_cast<uint8_t *>(memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(SCE_AUDIODEC_MP3_MAX_SAMPLES * SCE_AUDIODEC_MP3_MAX_CH_IN_DECODER * 2)));
        if (!es || !pcm) {
            if (es) {
                std::free(es);
            }
            if (pcm) {
                std::free(pcm);
            }
            sceAudioOutReleasePort(port);
            mp3Active_ = false;
            active_ = false;
            termMp3Library();
            return -2;
        }

        SceAudiodecInfo info{};
        info.mp3.size = sizeof(SceAudiodecInfoMp3);
        info.mp3.ch = first.channels;
        info.mp3.version = first.version;

        SceAudiodecCtrl ctrl{};
        ctrl.size = sizeof(SceAudiodecCtrl);
        ctrl.pEs = es;
        ctrl.maxEsSize = SCE_AUDIODEC_MP3_MAX_ES_SIZE;
        ctrl.pPcm = pcm;
        ctrl.maxPcmSize = SCE_AUDIODEC_MP3_MAX_SAMPLES * SCE_AUDIODEC_MP3_MAX_CH_IN_DECODER * 2;
        ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
        ctrl.pInfo = &info;

        int res = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_MP3);
        if (res < 0) {
            std::free(es);
            std::free(pcm);
            sceAudioOutReleasePort(port);
            mp3Active_ = false;
            active_ = false;
            termMp3Library();
            return res;
        }

        size_t pos = first.offset;
        int skippedMs = 0;
        while (!mp3StopRequested_) {
            if (mp3Paused_) {
                sceKernelDelayThread(10000);
                continue;
            }

            Mp3Frame frame;
            if (!findNextMp3Frame(mp3Data_, pos, &frame)) {
                break;
            }

            pos = frame.offset + static_cast<size_t>(frame.size);
            if (frame.sampleRate != first.sampleRate || frame.channels != first.channels ||
                frame.samples != first.samples) {
                continue;
            }

            int frameMs = (frame.samples * 1000) / frame.sampleRate;
            if (startOffsetMs_ > 0 && skippedMs + frameMs < startOffsetMs_) {
                skippedMs += frameMs;
                continue;
            }

            std::memcpy(es, mp3Data_.data() + frame.offset, static_cast<size_t>(frame.size));
            info.mp3.ch = frame.channels;
            info.mp3.version = frame.version;
            ctrl.inputEsSize = static_cast<SceUInt32>(frame.size);
            ctrl.outputPcmSize = 0;

            res = sceAudiodecDecode(&ctrl);
            if (res < 0) {
                sceAudiodecClearContext(&ctrl);
                continue;
            }

            int expectedBytes = frame.samples * frame.channels * 2;
            if (ctrl.outputPcmSize < static_cast<SceUInt32>(expectedBytes)) {
                std::memset(pcm + ctrl.outputPcmSize, 0, static_cast<size_t>(expectedBytes) - ctrl.outputPcmSize);
            }

            if (startedAtMs_ == 0) {
                startedAtMs_ = nowMs();
            }
            if (lastAppliedVolume != musicVolume_) {
                int volume = (SCE_AUDIO_OUT_MAX_VOL * clampi(musicVolume_, 0, 100)) / 100;
                int volumes[2] = {volume, volume};
                sceAudioOutSetVolume(port,
                                     static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                                     volumes);
                lastAppliedVolume = musicVolume_;
            }
            sceAudioOutOutput(port, pcm);
        }

        sceAudioOutOutput(port, nullptr);
        sceAudiodecDeleteDecoder(&ctrl);
        std::free(es);
        std::free(pcm);
        sceAudioOutReleasePort(port);
        mp3Active_ = false;
        termMp3Library();
        return 0;
    }

    void termMp3Library() {
        if (mp3LibraryOwned_) {
            sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);
        }
        mp3LibraryOwned_ = false;
        mp3LibraryReady_ = false;
    }

    int avHandle_ = -1;
    SceUID mp3Thread_ = -1;
    SceUID oggThread_ = -1;
    AudioPlayer *threadArgSelf_ = nullptr;
    volatile bool mp3StopRequested_ = false;
    volatile bool mp3Active_ = false;
    volatile bool mp3Paused_ = false;
    volatile bool oggStopRequested_ = false;
    volatile bool oggActive_ = false;
    volatile bool oggPaused_ = false;
    bool mp3LibraryReady_ = false;
    bool mp3LibraryOwned_ = false;
    std::vector<uint8_t> mp3Data_;
    std::string oggPath_;
    std::vector<uint8_t> oggData_;
    std::string preloadedAudioPath_;
    std::vector<uint8_t> preloadedAudioData_;
#endif

    bool active_ = false;
    uint64_t startedAtMs_ = 0;
    uint64_t pauseStartedAtMs_ = 0;
    uint64_t pausedAccumMs_ = 0;
    int startOffsetMs_ = 0;
    volatile int musicVolume_ = 100;
    AudioPreloadCache *preloadCache_ = nullptr;
};

enum class Scene {
    Library,
    Options,
    Playing,
    Paused,
    Countdown,
    Results,
};

enum class Judge {
    Marvelous = 0,
    Perfect = 1,
    Great = 2,
    Good = 3,
    Ok = 4,
    Miss = 5,
};

const char *judgeLabel(Judge judge) {
    switch (judge) {
    case Judge::Marvelous:
        return "320";
    case Judge::Perfect:
        return "300";
    case Judge::Great:
        return "200";
    case Judge::Good:
        return "100";
    case Judge::Ok:
        return "50";
    case Judge::Miss:
    default:
        return "MISS";
    }
}

int judgeValue(Judge judge) {
    switch (judge) {
    case Judge::Marvelous:
        return 320;
    case Judge::Perfect:
        return 300;
    case Judge::Great:
        return 200;
    case Judge::Good:
        return 100;
    case Judge::Ok:
        return 50;
    case Judge::Miss:
    default:
        return 0;
    }
}

uint32_t judgeColor(Judge judge) {
    switch (judge) {
    case Judge::Marvelous:
        return RGBA8(118, 245, 215, 255);
    case Judge::Perfect:
        return RGBA8(244, 225, 92, 255);
    case Judge::Great:
        return RGBA8(116, 200, 255, 255);
    case Judge::Good:
        return RGBA8(127, 226, 132, 255);
    case Judge::Ok:
        return RGBA8(255, 162, 93, 255);
    case Judge::Miss:
    default:
        return RGBA8(255, 92, 116, 255);
    }
}

struct Windows {
    int marvelous;
    int perfect;
    int great;
    int good;
    int ok;
    int miss;
};

Windows windowsForOd(float od) {
    float t = clampf(od, 0.0f, 10.0f) / 10.0f;
    Windows w;
    w.marvelous = static_cast<int>(32.0f - 14.0f * t);
    w.perfect = static_cast<int>(50.0f - 18.0f * t);
    w.great = static_cast<int>(82.0f - 24.0f * t);
    w.good = static_cast<int>(118.0f - 30.0f * t);
    w.ok = static_cast<int>(150.0f - 34.0f * t);
    w.miss = static_cast<int>(180.0f - 42.0f * t);
    return w;
}

Judge judgeForDelta(int absDelta, const Windows &w) {
    if (absDelta <= w.marvelous) {
        return Judge::Marvelous;
    }
    if (absDelta <= w.perfect) {
        return Judge::Perfect;
    }
    if (absDelta <= w.great) {
        return Judge::Great;
    }
    if (absDelta <= w.good) {
        return Judge::Good;
    }
    if (absDelta <= w.ok) {
        return Judge::Ok;
    }
    return Judge::Miss;
}

struct PlayState {
    Beatmap map;
    Windows windows{};
    int score = 0;
    int combo = 0;
    int maxCombo = 0;
    int judged = 0;
    int possible = 0;
    int weighted = 0;
    int counts[6] = {};
    std::string lastJudge;
    uint32_t lastJudgeColor = RGBA8(255, 255, 255, 255);
    int lastJudgeAt = -1000000;
    bool hasJudge = false;
    std::string audioWarning;
    bool audioOk = false;
    uint64_t silentStartedAt = 0;
    uint64_t pauseStartedAt = 0;
    size_t nextStoryboardSample = 0;
    bool scoreSubmitted = false;
};

int totalJudgementsFor(const Beatmap &map) {
    int total = 0;
    for (const Note &note : map.notes) {
        total += note.hold ? 2 : 1;
    }
    return total;
}

float accuracy(const PlayState &play) {
    if (play.judged <= 0) {
        return 100.0f;
    }
    return 100.0f * static_cast<float>(play.weighted) / static_cast<float>(play.judged * 320);
}

struct ScoreRecord {
    std::string key;
    int score = 0;
    int accuracyHundredths = 0;
    int maxCombo = 0;
    int plays = 0;
};

class ScoreStore {
public:
    void load() {
        records_.clear();
        FILE *file = std::fopen(SCORES_PATH, "rb");
        if (!file) {
            return;
        }

        char raw[1024];
        while (std::fgets(raw, sizeof(raw), file)) {
            std::string line = trim(raw);
            if (line.empty()) {
                continue;
            }
            std::vector<std::string> parts = split(line, '\t');
            if (parts.size() < 5) {
                continue;
            }
            ScoreRecord record;
            record.key = parts[0];
            record.score = parseInt(parts[1], 0);
            record.accuracyHundredths = parseInt(parts[2], 0);
            record.maxCombo = parseInt(parts[3], 0);
            record.plays = parseInt(parts[4], 0);
            records_.push_back(record);
        }

        std::fclose(file);
    }

    void save() const {
        ensureDirectories();
        FILE *file = std::fopen(SCORES_PATH, "wb");
        if (!file) {
            return;
        }

        for (const ScoreRecord &record : records_) {
            std::fprintf(file, "%s\t%d\t%d\t%d\t%d\n",
                         record.key.c_str(),
                         record.score,
                         record.accuracyHundredths,
                         record.maxCombo,
                         record.plays);
        }

        std::fclose(file);
    }

    const ScoreRecord *find(const Beatmap &map) const {
        for (const ScoreRecord &record : records_) {
            if (record.key == map.osuPath) {
                return &record;
            }
        }
        return nullptr;
    }

    void submit(const PlayState &play) {
        if (play.judged <= 0 || play.map.osuPath.empty()) {
            return;
        }

        int acc = clampi(static_cast<int>(std::lround(accuracy(play) * 100.0f)), 0, 10000);
        ScoreRecord *record = nullptr;
        for (ScoreRecord &existing : records_) {
            if (existing.key == play.map.osuPath) {
                record = &existing;
                break;
            }
        }

        if (!record) {
            ScoreRecord fresh;
            fresh.key = play.map.osuPath;
            records_.push_back(fresh);
            record = &records_.back();
        }

        record->plays++;
        if (play.score > record->score ||
            (play.score == record->score && acc > record->accuracyHundredths)) {
            record->score = play.score;
            record->accuracyHundredths = acc;
            record->maxCombo = play.maxCombo;
        }
        save();
    }

private:
    std::vector<ScoreRecord> records_;
};

void addJudge(PlayState *play, Judge judge, int ms) {
    int idx = static_cast<int>(judge);
    play->counts[idx]++;
    play->judged++;
    play->weighted += judgeValue(judge);

    if (judge == Judge::Miss) {
        play->combo = 0;
    } else {
        play->combo++;
        play->maxCombo = std::max(play->maxCombo, play->combo);
        play->score += judgeValue(judge) * (100 + std::min(play->combo, 400)) / 100;
    }

    play->lastJudge = judgeLabel(judge);
    play->lastJudgeColor = judgeColor(judge);
    play->lastJudgeAt = ms;
    play->hasJudge = true;
}

void preparePlay(const Beatmap &map, PlayState *play, AudioPlayer *audio, SampleEngine *samples) {
    if (audio) {
        audio->stop();
    }
    *play = PlayState{};
    play->map = map;
    for (Note &note : play->map.notes) {
        note.headHit = false;
        note.complete = false;
        note.missed = false;
        note.activeHold = false;
    }
    play->windows = windowsForOd(play->map.overallDifficulty);
    play->possible = totalJudgementsFor(play->map);
    if (samples) {
        samples->clearVoices();
    }
}

void startPlayAudio(PlayState *play, AudioPlayer *audio) {
    if (!play || !audio) {
        return;
    }
    audio->stop();
    play->silentStartedAt = nowMs();
    play->audioOk = audio->play(play->map.audioPath, &play->audioWarning);
    if (!play->audioOk && play->audioWarning.empty()) {
        play->audioWarning = "Audio unavailable, using silent clock";
    }
}

void pausePlay(PlayState *play, AudioPlayer *audio) {
    play->pauseStartedAt = nowMs();
    audio->pause();
}

void resumePlay(PlayState *play, AudioPlayer *audio) {
    if (play->pauseStartedAt > 0 && !play->audioOk) {
        uint64_t now = nowMs();
        if (now > play->pauseStartedAt) {
            play->silentStartedAt += now - play->pauseStartedAt;
        }
    }
    play->pauseStartedAt = 0;
    audio->resume();
}

int chartTimeMs(const PlayState &play, const AudioPlayer &audio) {
    if (play.audioOk && audio.active()) {
        return audio.currentMs();
    }
    if (play.pauseStartedAt > 0) {
        return static_cast<int>(play.pauseStartedAt - play.silentStartedAt);
    }
    return static_cast<int>(nowMs() - play.silentStartedAt);
}

void finishNoteMiss(PlayState *play, Note *note, int ms) {
    if (note->complete) {
        return;
    }
    note->missed = true;
    note->complete = true;
    if (note->hold && !note->headHit) {
        addJudge(play, Judge::Miss, ms);
        addJudge(play, Judge::Miss, ms);
    } else {
        addJudge(play, Judge::Miss, ms);
    }
}

void triggerNoteSample(const Note &note, SampleEngine *samples, const Settings &settings) {
    (void)note;
    (void)samples;
    (void)settings;
}

void pressLane(PlayState *play, int lane, int ms, SampleEngine *samples, const Settings &settings) {
    int best = -1;
    int bestDelta = 999999;

    for (size_t i = 0; i < play->map.notes.size(); i++) {
        Note &note = play->map.notes[i];
        if (note.lane != lane || note.complete || note.headHit) {
            continue;
        }
        if (note.startMs > ms + play->windows.miss) {
            break;
        }
        int delta = ms - note.startMs;
        int absDelta = std::abs(delta);
        if (absDelta <= play->windows.miss && absDelta < bestDelta) {
            best = static_cast<int>(i);
            bestDelta = absDelta;
        }
    }

    if (best < 0) {
        return;
    }

    Note &note = play->map.notes[best];
    Judge judge = judgeForDelta(bestDelta, play->windows);
    note.headHit = true;

    if (judge == Judge::Miss) {
        finishNoteMiss(play, &note, ms);
        return;
    }

    addJudge(play, judge, ms);
    triggerNoteSample(note, samples, settings);
    if (note.hold) {
        note.activeHold = true;
    } else {
        note.complete = true;
    }
}

void releaseLane(PlayState *play, int lane, int ms) {
    for (Note &note : play->map.notes) {
        if (note.lane != lane || !note.hold || !note.headHit || note.complete) {
            continue;
        }

        if (ms < note.endMs - play->windows.miss) {
            note.missed = true;
            note.complete = true;
            note.activeHold = false;
            addJudge(play, Judge::Miss, ms);
        } else {
            int absDelta = std::abs(ms - note.endMs);
            Judge judge = judgeForDelta(absDelta, play->windows);
            if (judge == Judge::Miss) {
                finishNoteMiss(play, &note, ms);
            } else {
                note.complete = true;
                note.activeHold = false;
                addJudge(play, judge, ms);
            }
        }
        return;
    }
}

void updateStoryboardSamples(PlayState *play, int ms, SampleEngine *samples, const Settings &settings) {
    (void)ms;
    (void)samples;
    (void)settings;
    if (play) {
        play->nextStoryboardSample = play->map.storyboardSamples.size();
    }
}

void updatePlaying(PlayState *play, int ms, const bool laneDown[LANE_COUNT], SampleEngine *samples, const Settings &settings) {
    updateStoryboardSamples(play, ms, samples, settings);

    for (Note &note : play->map.notes) {
        if (note.complete) {
            continue;
        }

        if (!note.headHit && ms > note.startMs + play->windows.miss) {
            finishNoteMiss(play, &note, ms);
            continue;
        }

        if (note.hold && note.headHit && !note.complete) {
            if (!laneDown[note.lane] && ms < note.endMs - play->windows.miss) {
                note.missed = true;
                note.complete = true;
                note.activeHold = false;
                addJudge(play, Judge::Miss, ms);
                continue;
            }

            if (laneDown[note.lane] && ms >= note.endMs) {
                note.complete = true;
                note.activeHold = false;
                addJudge(play, Judge::Marvelous, ms);
                continue;
            }

            if (ms > note.endMs + play->windows.miss) {
                finishNoteMiss(play, &note, ms);
            }
        }
    }
}

bool playComplete(const PlayState &play, int ms) {
    if (play.map.notes.empty()) {
        return true;
    }
    const Note &last = play.map.notes.back();
    int lastTime = last.hold ? last.endMs : last.startMs;
    if (play.judged >= play.possible) {
        return ms >= lastTime + END_DEADSPACE_MS;
    }
    return ms > lastTime + 3500;
}

float wave01(float periodMs, float phase = 0.0f) {
    float t = static_cast<float>(nowMs() % static_cast<uint64_t>(periodMs)) / periodMs;
    return 0.5f + 0.5f * std::sin((t + phase) * PI_F * 2.0f);
}

void drawSoftPanel(int x, int y, int w, int h, bool selected = false) {
    uint8_t edge = selected ? 210 : 110;
    vita2d_draw_rectangle(x, y, w, h, selected ? RGBA8(43, 34, 60, 224) : RGBA8(2, 7, 13, 218));
    vita2d_draw_rectangle(x, y, w, 2, RGBA8(220, 236, 245, edge));
    vita2d_draw_rectangle(x, y + h - 2, w, 2, RGBA8(220, 236, 245, edge / 2));
    vita2d_draw_rectangle(x, y, 2, h, RGBA8(220, 236, 245, edge));
    vita2d_draw_rectangle(x + w - 2, y, 2, h, RGBA8(220, 236, 245, edge / 2));
}

void drawHeader(vita2d_pgf *font, const Skin &skin) {
    vita2d_draw_rectangle(0, 0, SCREEN_W, 62, RGBA8(2, 3, 7, 246));
    vita2d_draw_rectangle(0, 60, SCREEN_W, 2, RGBA8(236, 87, 170, 224));
    Skin::drawCentered(skin.modeMania, 40.0f, 31.0f, 42.0f, 42.0f);
    drawText(font, 72, 40, RGBA8(255, 255, 255, 255), 1.06f, "osu!mania");
    drawText(font, 760, 39, RGBA8(214, 228, 236, 230), 0.68f, "VitaMania");
}

float fittedTextureWidth(vita2d_texture *texture, float maxW, float maxH) {
    if (!texture) {
        return 0.0f;
    }
    float tw = static_cast<float>(vita2d_texture_get_width(texture));
    float th = static_cast<float>(vita2d_texture_get_height(texture));
    if (tw <= 0.0f || th <= 0.0f) {
        return 0.0f;
    }
    float scale = std::min(maxW / tw, maxH / th);
    return tw * scale;
}

void drawInputHint(vita2d_pgf *font, vita2d_texture *glyph, const char *label, int cx, int cy,
                   float textScale = 0.62f, float iconMaxW = 28.0f, float iconMaxH = 28.0f,
                   uint32_t color = RGBA8(226, 238, 244, 240)) {
    const int textW = label ? vita2d_pgf_text_width(font, textScale, label) : 0;
    const float iconW = fittedTextureWidth(glyph, iconMaxW, iconMaxH);
    const float gap = iconW > 0.0f && textW > 0 ? 8.0f : 0.0f;
    const float totalW = iconW + gap + static_cast<float>(textW);
    float x = static_cast<float>(cx) - totalW * 0.5f;
    if (glyph && iconW > 0.0f) {
        Skin::drawCentered(glyph, x + iconW * 0.5f, static_cast<float>(cy), iconMaxW, iconMaxH, RGBA8(255, 255, 255, 245));
        x += iconW + gap;
    }
    if (label && textW > 0) {
        int textH = vita2d_pgf_text_height(font, textScale, label);
        int baseline = cy + textH / 2 - static_cast<int>(textScale * 3.0f);
        vita2d_pgf_draw_text(font, static_cast<int>(x), baseline, color, textScale, label);
    }
}

void drawLibrary(vita2d_pgf *font, const Library &library, BackgroundManager *background, const ScoreStore &scores, const Skin &skin) {
    if (background) {
        background->draw();
    } else {
        vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(9, 13, 18, 255));
    }
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(5, 8, 12, 185));
    drawHeader(font, skin);

    drawText(font, 44, 96, RGBA8(255, 255, 255, 255), 1.08f, "Songs");
    drawInputHint(font, skin.inputDpadVertical, "Song", 118, 126, 0.58f, 24.0f, 24.0f);
    drawInputHint(font, skin.inputDpadHorizontal, "Difficulty", 248, 126, 0.58f, 34.0f, 24.0f);
    drawInputHint(font, skin.inputCross, "Play", 390, 126, 0.58f, 24.0f, 24.0f);
    drawInputHint(font, skin.inputSquare, "Options", 500, 126, 0.58f, 24.0f, 24.0f);
    drawInputHint(font, skin.inputSelect, "BG", 620, 126, 0.58f, 38.0f, 24.0f);

    if (library.songs.empty()) {
        vita2d_draw_rectangle(76, 180, 808, 188, RGBA8(21, 29, 39, 232));
        vita2d_draw_rectangle(76, 180, 808, 4, RGBA8(118, 245, 215, 255));
        drawText(font, 110, 240, RGBA8(238, 248, 255, 255), 1.08f, "No playable osu maps found");
        drawText(font, 110, 284, RGBA8(170, 190, 198, 255), 0.76f, "Drop .osz files in ux0:data/vitamania; folders go in Songs/");
        drawText(font, 110, 318, RGBA8(170, 190, 198, 255), 0.76f, "Native 4K, other mania keys, and standard maps are playable.");
        return;
    }

    int start = std::max(0, library.selectedSong - 2);
    int end = std::min(static_cast<int>(library.songs.size()), start + 5);
    start = std::max(0, end - 5);

    for (int i = start; i < end; i++) {
        const SongEntry &song = library.songs[i];
        if (song.mapIndexes.empty()) {
            continue;
        }
        int diffIndex = clampi(song.selectedDifficulty, 0, static_cast<int>(song.mapIndexes.size()) - 1);
        const Beatmap &map = library.maps[song.mapIndexes[diffIndex]];
        int row = i - start;
        int y = 150 + row * 70;
        bool selected = i == library.selectedSong;
        float pulse = selected ? wave01(1100.0f) : 0.0f;
        int rowX = selected ? 44 : 54;
        int rowW = selected ? 872 : 852;
        uint32_t accent = selected ? RGBA8(118, 245, 215, 255) : RGBA8(74, 94, 106, 180);

        drawSoftPanel(rowX, y, rowW, 62, selected);
        vita2d_draw_rectangle(rowX + 8, y + 5, 8, 52, accent);
        if (selected) {
            vita2d_draw_rectangle(rowX + 18, y + 2, static_cast<int>(rowW - 36), 1, RGBA8(255, 125, 220, static_cast<uint8_t>(72 + pulse * 92.0f)));
            vita2d_draw_rectangle(rowX + 18 + static_cast<int>(pulse * (rowW - 156)), y + 2, 120, 1, RGBA8(118, 245, 215, 220));
        }

        std::string title = fitText(map.artist + " - " + map.title, 42);
        std::string version = map.converted ? map.version + " (" + map.conversionLabel + ")" : map.version;
        std::string diff = fitText("[" + version + "] by " + map.creator, 42);
        drawText(font, rowX + 42, y + 32, selected ? RGBA8(255, 255, 255, 255) : RGBA8(226, 236, 242, 245), 0.96f, "%s", title.c_str());
        drawText(font, rowX + 42, y + 54, RGBA8(190, 208, 216, 232), 0.66f, "%s", diff.c_str());
        drawText(font, rowX + 582, y + 37, RGBA8(224, 236, 242, 245), 0.76f, "%d notes", static_cast<int>(map.notes.size()));
        drawText(font, rowX + 728, y + 37, RGBA8(224, 236, 242, 245), 0.76f, "%.1f", map.overallDifficulty);
        drawText(font, rowX + 806, y + 37, RGBA8(224, 236, 242, 245), 0.76f, "%d/%d", diffIndex + 1, static_cast<int>(song.mapIndexes.size()));
        if (!map.warning.empty()) {
            drawText(font, rowX + 648, y + 55, RGBA8(255, 162, 93, 235), 0.56f, "audio?");
        }
    }

    const Beatmap *map = selectedBeatmap(library);
    const ScoreRecord *score = map ? scores.find(*map) : nullptr;
    if (score) {
        drawText(font, 52, 522, RGBA8(255, 255, 255, 255), 0.78f,
                 "Best %d   %.2f%%   %dx   %d plays",
                 score->score,
                 static_cast<float>(score->accuracyHundredths) / 100.0f,
                 score->maxCombo,
                 score->plays);
    } else {
        drawText(font, 52, 522, RGBA8(190, 210, 220, 255), 0.78f, "No score yet for this difficulty");
    }
    if (library.convertedDifficulties > 0) {
        drawText(font, 512, 522, RGBA8(166, 188, 198, 230), 0.58f, "%d songs, %d difficulties, %d converted, %d skipped",
                 static_cast<int>(library.songs.size()),
                 static_cast<int>(library.maps.size()),
                 library.convertedDifficulties,
                 library.skipped);
    } else {
        drawText(font, 632, 522, RGBA8(166, 188, 198, 230), 0.64f, "%d songs, %d difficulties, %d skipped",
                 static_cast<int>(library.songs.size()),
                 static_cast<int>(library.maps.size()),
                 library.skipped);
    }
}

void drawLaneBoard(vita2d_pgf *font, const PlayState &play, int ms, const bool laneDown[LANE_COUNT], const Settings &settings, BackgroundManager *background, const Skin &skin) {
    constexpr float boardX = 300.0f;
    constexpr float boardY = 62.0f;
    constexpr float laneW = 90.0f;
    constexpr float laneH = 430.0f;
    constexpr float receptorY = 463.0f;
    const float speed = clampf(play.map.scrollSpeed, 0.28f, 0.42f);
    constexpr float noteH = 22.0f;
    const float boardW = laneW * LANE_COUNT;
    const float boardCenterX = boardX + boardW * 0.5f;

    if (background) {
        background->draw();
    } else {
        vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(8, 11, 16, 255));
    }
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 214));

    vita2d_draw_rectangle(boardX - 22.0f, boardY, boardW + 44.0f, laneH, RGBA8(0, 0, 0, 118));
    Skin::drawFit(skin.stageLeft, boardX - 12.0f, boardY, 12.0f, laneH, RGBA8(255, 255, 255, 224));
    Skin::drawFit(skin.stageRight, boardX + boardW, boardY, 12.0f, laneH, RGBA8(255, 255, 255, 224));

    for (int lane = 0; lane < LANE_COUNT; lane++) {
        float x = boardX + lane * laneW;
        uint32_t bg = laneDown[lane] ? RGBA8(8, 12, 20, 250) : RGBA8(0, 1, 5, 246);
        vita2d_draw_rectangle(x, boardY, laneW - 2, laneH, bg);
        if (laneDown[lane]) {
            Skin::drawFit(skin.stageLight, x, boardY, laneW - 2.0f, laneH, RGBA8(255, 255, 255, 118));
        }
        vita2d_draw_rectangle(x, boardY, 1, laneH, RGBA8(142, 160, 174, 92));
    }
    vita2d_draw_rectangle(boardX + boardW, boardY, 1, laneH, RGBA8(142, 160, 174, 92));
    for (const Note &note : play.map.notes) {
        if (note.complete && (!note.hold || !note.missed || ms > note.endMs + 350)) {
            continue;
        }

        float x = boardX + note.lane * laneW + 5.0f;
        float yStart = receptorY - (static_cast<float>(note.startMs - ms) * speed);
        float yEnd = receptorY - (static_cast<float>(note.endMs - ms) * speed);
        float noteW = laneW - 12.0f;

        if (note.hold) {
            float top = std::min(yStart, yEnd);
            float bottom = std::max(yStart, yEnd);
            if (note.headHit && !note.missed) {
                bottom = std::min(bottom, receptorY);
            }
            if (bottom < boardY - 50 || top > boardY + laneH + 60) {
                continue;
            }
            float bodyH = bottom - top;
            if (bodyH > 1.0f) {
                if (!note.headHit || note.missed) {
                    bodyH = std::max(8.0f, bodyH);
                }
                uint32_t bodyTint = note.headHit ? RGBA8(255, 255, 255, 170) : RGBA8(255, 255, 255, 232);
                vita2d_texture *bodyTexture = skin.holdTailForLane(note.lane);
                if (bodyTexture) {
                    Skin::drawFit(bodyTexture, x, top, noteW, bodyH, bodyTint);
                } else {
                    vita2d_draw_rectangle(x + 11.0f, top, noteW - 22.0f, bodyH, note.headHit ? RGBA8(236, 87, 170, 150) : RGBA8(255, 230, 0, 190));
                }
                vita2d_draw_rectangle(x + noteW * 0.47f, top, noteW * 0.06f, bodyH, RGBA8(0, 0, 0, 90));
            }
            if (!note.headHit) {
                vita2d_texture *head = skin.holdHeadForLane(note.lane);
                if (head) {
                    Skin::drawFit(head, x, yStart - noteH * 0.5f, noteW, noteH);
                } else {
                    vita2d_draw_rectangle(x, yStart - noteH * 0.5f, noteW, noteH, RGBA8(124, 184, 255, 255));
                }
            }
            vita2d_texture *cap = skin.holdHeadForLane(note.lane);
            if (cap) {
                Skin::drawFit(cap, x, yEnd - noteH * 0.5f, noteW, noteH);
            } else {
                vita2d_draw_rectangle(x, yEnd - noteH * 0.5f, noteW, noteH, RGBA8(118, 245, 215, 230));
            }
        } else {
            if (yStart < boardY - 40 || yStart > boardY + laneH + 50 || note.headHit) {
                continue;
            }
            vita2d_texture *noteTexture = skin.noteForLane(note.lane);
            if (noteTexture) {
                Skin::drawFit(noteTexture, x, yStart - noteH * 0.5f, noteW, noteH);
            } else {
                vita2d_draw_rectangle(x, yStart - 9.0f, noteW, 18.0f, RGBA8(244, 225, 92, 255));
            }
        }
    }

    for (int lane = 0; lane < LANE_COUNT; lane++) {
        float x = boardX + lane * laneW;
        vita2d_texture *key = skin.keyForLane(lane, laneDown[lane]);
        if (key) {
            Skin::drawFit(key, x + 3.0f, receptorY - 28.0f, laneW - 8.0f, 58.0f);
        } else {
            vita2d_draw_rectangle(x + 3.0f, receptorY - 24.0f, laneW - 8.0f, 48.0f,
                                  laneDown[lane] ? RGBA8(236, 87, 170, 255) : RGBA8(30, 38, 48, 255));
        }
        vita2d_draw_rectangle(x + 9.0f, receptorY - 1.0f, laneW - 18.0f, 2.0f, RGBA8(255, 255, 255, 190));
        uint32_t color = laneDown[lane] ? RGBA8(118, 245, 215, 255) : RGBA8(156, 177, 186, 255);
        int labelX = static_cast<int>(x + laneW * 0.5f - static_cast<float>(settings.laneLabels[lane].size()) * 5.5f);
        drawText(font, labelX, 529, color, 0.64f, "%s", settings.laneLabels[lane].c_str());
    }

    Skin::drawCentered(skin.warningArrow, boardCenterX, receptorY + 41.0f, 34.0f, 34.0f, RGBA8(255, 125, 220, 214));

    std::string title = fitText(play.map.artist + " - " + play.map.title, 36);
    drawText(font, 26, 32, RGBA8(238, 248, 255, 230), 0.66f, "%s", title.c_str());
    drawText(font, 26, 56, RGBA8(180, 196, 205, 190), 0.48f, "%s", fitText(play.map.version, 30).c_str());

    char scoreText[16];
    std::snprintf(scoreText, sizeof(scoreText), "%08d", clampi(play.score, 0, 99999999));
    char accText[16];
    std::snprintf(accText, sizeof(accText), "%.2f%%", accuracy(play));
    if (skin.scoreStringWidth(scoreText, 0.58f) > 0.0f) {
        skin.drawScoreRight(scoreText, 936.0f, 14.0f, 0.58f);
        skin.drawScoreRight(accText, 936.0f, 54.0f, 0.38f, RGBA8(255, 255, 255, 238));
    } else {
        drawText(font, 772, 40, RGBA8(255, 255, 255, 255), 0.96f, "%s", scoreText);
        drawText(font, 824, 72, RGBA8(255, 255, 255, 238), 0.62f, "%s", accText);
    }

    std::string comboText = std::to_string(play.combo) + "x";
    if (skin.scoreStringWidth(comboText, 0.36f) > 0.0f) {
        skin.drawScoreString(comboText, 26.0f, 108.0f, 0.36f, RGBA8(118, 245, 215, 230));
    } else {
        drawText(font, 26, 132, RGBA8(118, 245, 215, 230), 0.72f, "%s", comboText.c_str());
    }

    if (play.hasJudge) {
        int judgeAge = std::max(0, ms - play.lastJudgeAt);
        float pop = 0.0f;
        if (judgeAge < 120) {
            float t = 1.0f - static_cast<float>(judgeAge) / 120.0f;
            pop = std::sin(t * PI_F) * 0.075f;
        }
        vita2d_texture *judge = skin.judgeTexture(play.lastJudge);
        std::string centerCombo = std::to_string(play.combo);
        float comboScale = 0.74f + pop;
        if (skin.scoreStringWidth(centerCombo, comboScale) > 0.0f) {
            skin.drawScoreCentered(centerCombo, boardCenterX, boardY + 188.0f - pop * 18.0f, comboScale, RGBA8(255, 245, 158, 238));
        } else {
            drawTextCentered(font, static_cast<int>(boardCenterX), static_cast<int>(boardY + 208.0f - pop * 18.0f),
                             RGBA8(255, 245, 158, 238), 1.42f + pop * 1.25f, "%s", centerCombo.c_str());
        }
        if (judge) {
            Skin::drawCentered(judge, boardCenterX, boardY + 240.0f, 168.0f * (1.0f + pop), 58.0f * (1.0f + pop));
        } else {
            drawTextCentered(font, static_cast<int>(boardCenterX), static_cast<int>(boardY + 254.0f),
                             play.lastJudgeColor, 1.10f * (1.0f + pop), "%s", play.lastJudge.c_str());
        }
    }

    if (!play.audioOk && !play.audioWarning.empty()) {
        drawText(font, 666, 520, RGBA8(255, 162, 93, 230), 0.48f, "%s", fitText(play.audioWarning, 36).c_str());
    }
}

void drawResults(vita2d_pgf *font, const PlayState &play, BackgroundManager *background, const Skin &skin) {
    if (background) {
        background->draw();
    } else {
        vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(9, 13, 18, 255));
    }
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 208));
    drawHeader(font, skin);

    vita2d_texture *rank = skin.rankForAccuracy(accuracy(play));
    Skin::drawCentered(rank, 818.0f, 128.0f, 88.0f, 88.0f);

    std::string title = fitText(play.map.artist + " - " + play.map.title, 46);
    drawText(font, 62, 110, RGBA8(238, 248, 255, 255), 1.00f, "%s", title.c_str());
    drawText(font, 62, 140, RGBA8(180, 198, 207, 230), 0.66f, "%s", fitText(play.map.version, 48).c_str());

    vita2d_draw_rectangle(62, 166, 836, 2, RGBA8(118, 245, 215, 150));

    char resultScore[16];
    std::snprintf(resultScore, sizeof(resultScore), "%08d", clampi(play.score, 0, 99999999));
    constexpr int statY = 188;
    constexpr int statW = 252;
    constexpr int statH = 96;
    constexpr int statGap = 26;
    constexpr int statX0 = (SCREEN_W - statW * 3 - statGap * 2) / 2;

    drawSoftPanel(statX0, statY, statW, statH, false);
    if (skin.scoreStringWidth(resultScore, 0.50f) > 0.0f) {
        skin.drawScoreCentered(resultScore, statX0 + statW * 0.5f, statY + 22.0f, 0.50f, RGBA8(118, 245, 215, 255));
    } else {
        drawTextCentered(font, statX0 + statW / 2, statY + 58, RGBA8(118, 245, 215, 255), 1.08f, "%d", play.score);
    }
    drawTextCentered(font, statX0 + statW / 2, statY + 78, RGBA8(170, 190, 200, 230), 0.62f, "score");

    std::string resultCombo = std::to_string(play.maxCombo) + "x";
    int comboX = statX0 + statW + statGap;
    drawSoftPanel(comboX, statY, statW, statH, false);
    if (skin.scoreStringWidth(resultCombo, 0.50f) > 0.0f) {
        skin.drawScoreCentered(resultCombo, comboX + statW * 0.5f, statY + 22.0f, 0.50f, RGBA8(246, 251, 252, 255));
    } else {
        drawTextCentered(font, comboX + statW / 2, statY + 58, RGBA8(246, 251, 252, 255), 1.08f, "%dx", play.maxCombo);
    }
    drawTextCentered(font, comboX + statW / 2, statY + 78, RGBA8(170, 190, 200, 230), 0.62f, "max combo");

    char resultAcc[16];
    std::snprintf(resultAcc, sizeof(resultAcc), "%.2f%%", accuracy(play));
    int accX = comboX + statW + statGap;
    drawSoftPanel(accX, statY, statW, statH, false);
    if (skin.scoreStringWidth(resultAcc, 0.50f) > 0.0f) {
        skin.drawScoreCentered(resultAcc, accX + statW * 0.5f, statY + 22.0f, 0.50f, RGBA8(246, 251, 252, 255));
    } else {
        drawTextCentered(font, accX + statW / 2, statY + 58, RGBA8(246, 251, 252, 255), 1.08f, "%.2f%%", accuracy(play));
    }
    drawTextCentered(font, accX + statW / 2, statY + 78, RGBA8(170, 190, 200, 230), 0.62f, "accuracy");

    constexpr int judgeW = 128;
    constexpr int judgeH = 96;
    constexpr int judgeGap = 18;
    constexpr int judgeY = 326;
    constexpr int judgeStartX = (SCREEN_W - judgeW * 6 - judgeGap * 5) / 2;
    const char *labels[6] = {"320", "300", "200", "100", "50", "Miss"};
    for (int i = 0; i < 6; i++) {
        int x = judgeStartX + i * (judgeW + judgeGap);
        drawSoftPanel(x, judgeY, judgeW, judgeH, false);
        drawTextCentered(font, x + judgeW / 2, judgeY + 39, judgeColor(static_cast<Judge>(i)), i == 5 ? 0.62f : 0.76f, "%s", labels[i]);
        drawTextCentered(font, x + judgeW / 2, judgeY + 78, RGBA8(232, 239, 241, 255), 0.82f, "%d", play.counts[i]);
    }

    constexpr int buttonY = 466;
    constexpr int buttonH = 50;
    drawSoftPanel(194, buttonY, 252, buttonH, false);
    drawInputHint(font, skin.inputCross, "Replay", 320, buttonY + buttonH / 2, 0.66f, 26.0f, 26.0f, RGBA8(255, 255, 255, 245));
    drawSoftPanel(514, buttonY, 252, buttonH, false);
    drawInputHint(font, skin.inputCircle, "Song Select", 640, buttonY + buttonH / 2, 0.66f, 26.0f, 26.0f, RGBA8(255, 255, 255, 245));
}

void drawMenuRow(vita2d_pgf *font, int x, int y, int w, const char *label, const std::string &value, bool selected, const Skin *skin = nullptr, vita2d_texture *icon = nullptr) {
    constexpr int rowH = 50;
    float pulse = selected ? wave01(900.0f) : 0.0f;
    uint32_t bg = selected ? RGBA8(30, 54, 68, 236) : RGBA8(4, 10, 16, 226);
    uint32_t accent = selected ? RGBA8(118, 245, 215, 255) : RGBA8(96, 122, 134, 190);
    vita2d_draw_rectangle(x, y, w, rowH, bg);
    vita2d_draw_rectangle(x, y, w, 2, selected ? RGBA8(255, 255, 255, static_cast<uint8_t>(205 + pulse * 50.0f)) : RGBA8(116, 138, 150, 172));
    vita2d_draw_rectangle(x, y + rowH - 2, w, 2, selected ? RGBA8(255, 255, 255, 220) : RGBA8(116, 138, 150, 132));
    vita2d_draw_rectangle(x, y, 2, rowH, selected ? RGBA8(255, 255, 255, 220) : RGBA8(116, 138, 150, 132));
    vita2d_draw_rectangle(x + w - 2, y, 2, rowH, selected ? RGBA8(255, 255, 255, 220) : RGBA8(116, 138, 150, 132));
    vita2d_draw_rectangle(x + 7, y + 3, 6, rowH - 6, accent);
    if (icon) {
        Skin::drawCentered(icon, static_cast<float>(x + 52), static_cast<float>(y + rowH / 2), 92.0f, 38.0f);
    }
    drawText(font, x + (icon ? 104 : 32), y + 33, selected ? RGBA8(246, 251, 252, 255) : RGBA8(214, 228, 234, 245), 0.90f, "%s", label);
    if (!value.empty()) {
        if (skin && value.find('%') != std::string::npos && skin->scoreStringWidth(value, 0.46f) > 0.0f) {
            skin->drawScoreRight(value, static_cast<float>(x + w - 32), static_cast<float>(y + 9), 0.46f, RGBA8(255, 255, 255, selected ? 255 : 230));
        } else {
            drawText(font, x + w - 230, y + 34, RGBA8(214, 230, 238, selected ? 255 : 230), 0.92f, "%s", fitText(value, 22).c_str());
        }
    }
}

void drawPauseImageRow(int x, int y, int w, int h, vita2d_texture *texture, bool selected) {
    float pulse = selected ? wave01(900.0f) : 0.0f;
    uint32_t bg = selected ? RGBA8(30, 54, 68, 236) : RGBA8(4, 10, 16, 226);
    uint32_t edge = selected ? RGBA8(255, 255, 255, static_cast<uint8_t>(205 + pulse * 50.0f)) : RGBA8(116, 138, 150, 172);
    vita2d_draw_rectangle(x, y, w, h, bg);
    vita2d_draw_rectangle(x, y, w, 2, edge);
    vita2d_draw_rectangle(x, y + h - 2, w, 2, edge);
    vita2d_draw_rectangle(x, y, 2, h, edge);
    vita2d_draw_rectangle(x + w - 2, y, 2, h, edge);
    vita2d_draw_rectangle(x + 7, y + 3, 6, h - 6, selected ? RGBA8(118, 245, 215, 255) : RGBA8(96, 122, 134, 190));
    Skin::drawCentered(texture, static_cast<float>(x + w / 2), static_cast<float>(y + h / 2), static_cast<float>(w - 34), static_cast<float>(h - 4), selected ? RGBA8(255, 255, 255, 255) : RGBA8(220, 232, 238, 235));
}

void drawPauseMenu(vita2d_pgf *font, const PlayState &play, const Settings &settings, int selected, const Skin &skin) {
    constexpr int panelX = 216;
    constexpr int panelY = 72;
    constexpr int panelW = 528;
    constexpr int panelH = 418;
    constexpr int rowX = 246;
    constexpr int rowW = 468;
    constexpr int rowH = 50;

    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 154));
    vita2d_draw_rectangle(panelX, panelY, panelW, panelH, RGBA8(2, 6, 13, 232));
    vita2d_draw_rectangle(panelX, panelY, panelW, 4, RGBA8(118, 245, 215, 240));

    drawText(font, rowX, 122, RGBA8(238, 248, 255, 255), 1.18f, "Paused");
    drawText(font, rowX, 154, RGBA8(170, 190, 200, 224), 0.66f, "%s", fitText(play.map.version, 38).c_str());

    drawPauseImageRow(rowX, 178, rowW, rowH, skin.pauseContinue, selected == 0);
    drawPauseImageRow(rowX, 232, rowW, rowH, skin.pauseRetry, selected == 1);
    drawMenuRow(font, rowX, 286, rowW, "Music Volume", std::to_string(settings.musicVolume) + "%", selected == 2, &skin);
    drawPauseImageRow(rowX, 340, rowW, rowH, skin.pauseBack, selected == 3);

    drawInputHint(font, skin.inputDpadVertical, "Select", 315, 466, 0.54f, 22.0f, 22.0f, RGBA8(150, 172, 182, 230));
    drawInputHint(font, skin.inputDpadHorizontal, "Adjust", 438, 466, 0.54f, 30.0f, 22.0f, RGBA8(150, 172, 182, 230));
    drawInputHint(font, skin.inputCross, "Confirm", 560, 466, 0.54f, 22.0f, 22.0f, RGBA8(150, 172, 182, 230));
    drawInputHint(font, skin.inputStart, "Resume", 680, 466, 0.54f, 34.0f, 22.0f, RGBA8(150, 172, 182, 230));
}

void drawCountdown(vita2d_pgf *font, uint64_t countdownStartMs) {
    uint64_t elapsed = nowMs() - countdownStartMs;
    int remaining = 3 - static_cast<int>(elapsed / 1000);
    remaining = clampi(remaining, 1, 3);

    char text[4];
    std::snprintf(text, sizeof(text), "%d", remaining);
    constexpr float scale = 3.10f;
    int width = 0;
    int height = 0;
    vita2d_pgf_text_dimensions(font, scale, text, &width, &height);

    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 86));
    vita2d_pgf_draw_text(font,
                         SCREEN_W / 2 - width / 2,
                         SCREEN_H / 2 + height / 2 - static_cast<int>(scale * 3.0f),
                         RGBA8(246, 251, 252, 255),
                         scale,
                         text);
}

void drawOptions(vita2d_pgf *font, const Settings &settings, int selected, int captureLane, BackgroundManager *background, const Skin &skin) {
    if (background) {
        background->draw();
    } else {
        vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(9, 13, 18, 255));
    }
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(5, 8, 12, 198));
    drawHeader(font, skin);

    vita2d_draw_rectangle(118, 80, 724, 430, RGBA8(12, 18, 25, 226));
    vita2d_draw_rectangle(118, 80, 724, 4, RGBA8(118, 245, 215, 255));
    drawText(font, 158, 106, RGBA8(238, 248, 255, 255), 1.16f, "Options");

    for (int lane = 0; lane < LANE_COUNT; lane++) {
        std::string label = "Lane " + std::to_string(lane + 1);
        std::string value = captureLane == lane ? "Press button..." : settings.laneLabels[lane];
        drawMenuRow(font, 158, 132 + lane * 54, 644, label.c_str(), value, selected == lane, &skin);
    }
    drawMenuRow(font, 158, 356, 644, "Music Volume", std::to_string(settings.musicVolume) + "%", selected == 4, &skin);
    drawMenuRow(font, 158, 410, 644, "Backgrounds", settings.backgrounds ? "On" : "Off", selected == 5, &skin);
    drawMenuRow(font, 158, 464, 644, "Back", "", selected == 6, &skin);

    if (captureLane >= 0) {
        drawText(font, 168, 534, RGBA8(255, 220, 116, 255), 0.56f, "Press the button for lane %d.", captureLane + 1);
        drawInputHint(font, skin.inputSelect, "Cancel", 600, 528, 0.54f, 34.0f, 22.0f, RGBA8(255, 220, 116, 255));
    } else {
        drawInputHint(font, skin.inputCross, "Edit Lane", 248, 528, 0.54f, 22.0f, 22.0f, RGBA8(138, 160, 170, 255));
        drawInputHint(font, skin.inputDpadHorizontal, "Adjust", 392, 528, 0.54f, 30.0f, 22.0f, RGBA8(138, 160, 170, 255));
        drawInputHint(font, skin.inputTriangle, "Reset", 524, 528, 0.54f, 22.0f, 22.0f, RGBA8(138, 160, 170, 255));
        drawInputHint(font, skin.inputCircle, "Back", 638, 528, 0.54f, 22.0f, 22.0f, RGBA8(138, 160, 170, 255));
    }
}

void drawLoading(vita2d_pgf *font, const Skin &skin, const char *message, const char *detail = nullptr) {
    float pulse = wave01(1200.0f);

    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 255));
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(3, 5, 11, 255));
    Skin::drawCentered(skin.modeMania, SCREEN_W * 0.5f, 246.0f, 82.0f + pulse * 8.0f, 82.0f + pulse * 8.0f, RGBA8(255, 255, 255, 238));
    drawTextCentered(font, SCREEN_W / 2, 326, RGBA8(245, 250, 252, 255), 1.04f, "%s", message);
    if (detail && detail[0]) {
        drawTextCentered(font, SCREEN_W / 2, 362, RGBA8(170, 190, 200, 230), 0.62f, "%s", detail);
    }
}

void presentLoading(vita2d_pgf *font, const Skin &skin, const char *message, const char *detail = nullptr) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    drawLoading(font, skin, message, detail);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    sceKernelDelayThread(16000);
}

} // namespace

int main() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    ensureDirectories();

    vita2d_init();
    vita2d_set_vblank_wait(1);
    vita2d_pgf *font = vita2d_load_default_pgf();
    Skin skin;
    skin.load();

    presentLoading(font, skin, "Scanning songs", "This might take a while if you added new .osz files");
    Library library = scanLibrary();
    Scene scene = Scene::Library;
    PlayState play;
    AudioPlayer audio;
    AudioPlayer previewAudio;
    AudioPreloadCache previewCache;
    SampleEngine samples;
    BackgroundManager background;
    ScoreStore scores;
    Settings settings;
    int pauseSelected = 0;
    int optionSelected = 0;
    int captureLane = -1;
    uint64_t countdownStartMs = 0;
    bool countdownStartsAudio = false;
    std::string previewTargetPath;
    std::string previewPlayingPath;
    int previewTargetOffset = 0;
    int previewPlayingOffset = -1;
    uint64_t previewChangedAt = 0;
    bool libraryTouched = false;
    audio.setMusicVolume(settings.musicVolume);
    previewAudio.setPreloadCache(&previewCache);
    previewAudio.setMusicVolume(35);
    previewCache.start();
    scores.load();

    uint32_t previousButtons = 0;
    bool running = true;

    while (running) {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositiveExt(0, &pad, 1);

        uint32_t buttons = pad.buttons;
        uint32_t pressed = buttons & ~previousButtons;
        uint32_t released = previousButtons & ~buttons;
        bool laneDown[LANE_COUNT] = {};
        for (int lane = 0; lane < LANE_COUNT; lane++) {
            laneDown[lane] = (buttons & settings.laneButtons[lane]) != 0;
        }

        if (scene == Scene::Library) {
            if (pressed & SCE_CTRL_CIRCLE) {
                running = false;
            }
            if (pressed & SCE_CTRL_SQUARE) {
                scene = Scene::Options;
            }
            if (pressed & SCE_CTRL_TRIANGLE) {
                presentLoading(font, skin, "Refreshing library", "This might take a while if you added new .osz files");
                library = scanLibrary();
            }
            if (pressed & SCE_CTRL_SELECT) {
                settings.backgrounds = !settings.backgrounds;
                if (!settings.backgrounds) {
                    background.clear();
                }
            }
            if (!library.songs.empty()) {
                if (pressed & SCE_CTRL_UP) {
                    libraryTouched = true;
                    library.selectedSong = (library.selectedSong + static_cast<int>(library.songs.size()) - 1) % static_cast<int>(library.songs.size());
                }
                if (pressed & SCE_CTRL_DOWN) {
                    libraryTouched = true;
                    library.selectedSong = (library.selectedSong + 1) % static_cast<int>(library.songs.size());
                }
                SongEntry &song = library.songs[library.selectedSong];
                if (!song.mapIndexes.empty()) {
                    if (pressed & SCE_CTRL_LEFT) {
                        libraryTouched = true;
                        song.selectedDifficulty = (song.selectedDifficulty + static_cast<int>(song.mapIndexes.size()) - 1) % static_cast<int>(song.mapIndexes.size());
                    }
                    if (pressed & SCE_CTRL_RIGHT) {
                        libraryTouched = true;
                        song.selectedDifficulty = (song.selectedDifficulty + 1) % static_cast<int>(song.mapIndexes.size());
                    }
                }
                if (pressed & SCE_CTRL_CROSS) {
                    Beatmap *map = selectedBeatmap(&library);
                    if (map) {
                        presentLoading(font, skin, "Loading beatmap");
                        previewAudio.stop();
                        previewCache.stop();
                        previewPlayingPath.clear();
                        previewTargetPath.clear();
                        previewPlayingOffset = -1;
                        previewTargetOffset = 0;
                        audio.setMusicVolume(settings.musicVolume);
                        samples.clearVoices();
                        preparePlay(*map, &play, &audio, &samples);
                        audio.preloadForPlay(play.map.audioPath);
                        background.preloadNow(settings.backgrounds ? play.map.backgroundPath : "");
                        countdownStartMs = nowMs();
                        countdownStartsAudio = true;
                        scene = Scene::Countdown;
                    }
                }
            }
        } else if (scene == Scene::Options) {
            if (captureLane >= 0) {
                if (pressed & SCE_CTRL_SELECT) {
                    captureLane = -1;
                } else {
                    uint32_t chosen = firstBindablePressed(pressed);
                    if (chosen) {
                        settings.setLaneButton(captureLane, chosen);
                        captureLane = -1;
                    }
                }
            } else {
                if (pressed & SCE_CTRL_CIRCLE) {
                    scene = Scene::Library;
                }
                if (pressed & SCE_CTRL_TRIANGLE) {
                    settings.applyPreset(0);
                }
                if (pressed & SCE_CTRL_UP) {
                    optionSelected = (optionSelected + 6) % 7;
                }
                if (pressed & SCE_CTRL_DOWN) {
                    optionSelected = (optionSelected + 1) % 7;
                }
                if (pressed & SCE_CTRL_LEFT) {
                    if (optionSelected == 4) {
                        settings.musicVolume = clampi(settings.musicVolume - 5, 0, 100);
                        audio.setMusicVolume(settings.musicVolume);
                    } else if (optionSelected == 5) {
                        settings.backgrounds = !settings.backgrounds;
                        if (!settings.backgrounds) {
                            background.clear();
                        }
                    }
                }
                if (pressed & SCE_CTRL_RIGHT) {
                    if (optionSelected == 4) {
                        settings.musicVolume = clampi(settings.musicVolume + 5, 0, 100);
                        audio.setMusicVolume(settings.musicVolume);
                    } else if (optionSelected == 5) {
                        settings.backgrounds = !settings.backgrounds;
                        if (!settings.backgrounds) {
                            background.clear();
                        }
                    }
                }
                if (pressed & SCE_CTRL_CROSS) {
                    if (optionSelected >= 0 && optionSelected < LANE_COUNT) {
                        captureLane = optionSelected;
                    } else if (optionSelected == 5) {
                        settings.backgrounds = !settings.backgrounds;
                        if (!settings.backgrounds) {
                            background.clear();
                        }
                    } else if (optionSelected == 6) {
                        scene = Scene::Library;
                    }
                }
            }
        } else if (scene == Scene::Playing) {
            int ms = chartTimeMs(play, audio);
            if (pressed & SCE_CTRL_START) {
                pauseSelected = 0;
                pausePlay(&play, &audio);
                scene = Scene::Paused;
            } else {
                for (int lane = 0; lane < LANE_COUNT; lane++) {
                    if (pressed & settings.laneButtons[lane]) {
                        pressLane(&play, lane, ms, &samples, settings);
                    }
                    if (released & settings.laneButtons[lane]) {
                        releaseLane(&play, lane, ms);
                    }
                }

                updatePlaying(&play, ms, laneDown, &samples, settings);
                if (playComplete(play, ms)) {
                    if (!play.scoreSubmitted) {
                        scores.submit(play);
                        play.scoreSubmitted = true;
                    }
                    audio.stop();
                    samples.clearVoices();
                    scene = Scene::Results;
                }
            }
        } else if (scene == Scene::Paused) {
            if (pressed & SCE_CTRL_UP) {
                pauseSelected = (pauseSelected + 3) % 4;
            }
            if (pressed & SCE_CTRL_DOWN) {
                pauseSelected = (pauseSelected + 1) % 4;
            }
            if (pressed & SCE_CTRL_LEFT) {
                if (pauseSelected == 2) {
                    settings.musicVolume = clampi(settings.musicVolume - 5, 0, 100);
                    audio.setMusicVolume(settings.musicVolume);
                }
            }
            if (pressed & SCE_CTRL_RIGHT) {
                if (pauseSelected == 2) {
                    settings.musicVolume = clampi(settings.musicVolume + 5, 0, 100);
                    audio.setMusicVolume(settings.musicVolume);
                }
            }
            if ((pressed & SCE_CTRL_START) || (pressed & SCE_CTRL_CIRCLE)) {
                countdownStartMs = nowMs();
                countdownStartsAudio = false;
                scene = Scene::Countdown;
            }
            if (pressed & SCE_CTRL_CROSS) {
                if (pauseSelected == 0) {
                    countdownStartMs = nowMs();
                    countdownStartsAudio = false;
                    scene = Scene::Countdown;
                } else if (pauseSelected == 1) {
                    Beatmap restartMap = play.map;
                    presentLoading(font, skin, "Restarting");
                    previewAudio.stop();
                    previewCache.stop();
                    samples.clearVoices();
                    audio.setMusicVolume(settings.musicVolume);
                    preparePlay(restartMap, &play, &audio, &samples);
                    audio.preloadForPlay(play.map.audioPath);
                    background.preloadNow(settings.backgrounds ? play.map.backgroundPath : "");
                    countdownStartMs = nowMs();
                    countdownStartsAudio = true;
                    scene = Scene::Countdown;
                } else if (pauseSelected == 3) {
                    audio.stop();
                    samples.clearVoices();
                    scene = Scene::Library;
                }
            }
        } else if (scene == Scene::Countdown) {
            if (pressed & SCE_CTRL_CIRCLE) {
                if (countdownStartsAudio) {
                    audio.stop();
                    samples.clearVoices();
                    scene = Scene::Library;
                } else {
                    scene = Scene::Paused;
                }
            } else if (nowMs() - countdownStartMs >= static_cast<uint64_t>(countdownStartsAudio ? COUNTDOWN_MS + START_LEAD_IN_MS : COUNTDOWN_MS)) {
                if (countdownStartsAudio) {
                    audio.setMusicVolume(settings.musicVolume);
                    startPlayAudio(&play, &audio);
                    countdownStartsAudio = false;
                } else {
                    resumePlay(&play, &audio);
                }
                scene = Scene::Playing;
            }
        } else if (scene == Scene::Results) {
            if (pressed & SCE_CTRL_CIRCLE) {
                scene = Scene::Library;
            }
            if (pressed & SCE_CTRL_CROSS) {
                Beatmap replayMap = play.map;
                presentLoading(font, skin, "Loading replay");
                previewAudio.stop();
                previewCache.stop();
                previewPlayingPath.clear();
                previewTargetPath.clear();
                previewPlayingOffset = -1;
                previewTargetOffset = 0;
                samples.clearVoices();
                audio.setMusicVolume(settings.musicVolume);
                preparePlay(replayMap, &play, &audio, &samples);
                audio.preloadForPlay(play.map.audioPath);
                background.preloadNow(settings.backgrounds ? play.map.backgroundPath : "");
                countdownStartMs = nowMs();
                countdownStartsAudio = true;
                scene = Scene::Countdown;
            }
        }

        const Beatmap *menuMap = selectedBeatmap(library);
        if (scene == Scene::Library && menuMap && !menuMap->audioPath.empty()) {
            previewCache.request(menuMap->audioPath);
        }
        if (scene == Scene::Library && libraryTouched && menuMap && !menuMap->audioPath.empty()) {
            int previewOffset = std::max(0, menuMap->previewTime);
            if (previewTargetPath != menuMap->audioPath || previewTargetOffset != previewOffset) {
                previewTargetPath = menuMap->audioPath;
                previewTargetOffset = previewOffset;
                previewChangedAt = nowMs();
                if (previewPlayingPath != previewTargetPath || previewPlayingOffset != previewTargetOffset) {
                    previewAudio.stop();
                    previewPlayingPath.clear();
                    previewPlayingOffset = -1;
                }
            }

            bool previewWarm = !endsWithNoCase(previewTargetPath, ".mp3") ||
                               previewCache.ready(previewTargetPath);
            if (previewPlayingPath.empty() && previewWarm && nowMs() - previewChangedAt > PREVIEW_START_DELAY_MS) {
                std::string previewError;
                previewAudio.setMusicVolume(35);
                if (previewAudio.play(previewTargetPath, &previewError, previewTargetOffset)) {
                    previewPlayingPath = previewTargetPath;
                    previewPlayingOffset = previewTargetOffset;
                } else {
                    previewTargetPath.clear();
                    previewPlayingPath.clear();
                    previewTargetOffset = 0;
                    previewPlayingOffset = -1;
                }
            }
        } else {
            previewAudio.stop();
            previewTargetPath.clear();
            previewPlayingPath.clear();
            previewTargetOffset = 0;
            previewPlayingOffset = -1;
        }

        if (scene == Scene::Library || scene == Scene::Options) {
            background.setPath(settings.backgrounds && menuMap ? menuMap->backgroundPath : "", false);
        } else {
            background.setPath(settings.backgrounds ? play.map.backgroundPath : "", true);
        }

        int drawMs = (scene == Scene::Playing || scene == Scene::Paused || scene == Scene::Countdown) ? chartTimeMs(play, audio) : 0;
        if (scene == Scene::Countdown && countdownStartsAudio) {
            uint64_t elapsed = nowMs() - countdownStartMs;
            uint64_t total = static_cast<uint64_t>(COUNTDOWN_MS + START_LEAD_IN_MS);
            drawMs = -static_cast<int>(total - std::min<uint64_t>(elapsed, total));
        }
        vita2d_start_drawing();
        vita2d_clear_screen();

        if (scene == Scene::Library) {
            drawLibrary(font, library, &background, scores, skin);
        } else if (scene == Scene::Options) {
            drawOptions(font, settings, optionSelected, captureLane, &background, skin);
        } else if (scene == Scene::Playing) {
            drawLaneBoard(font, play, drawMs, laneDown, settings, &background, skin);
        } else if (scene == Scene::Paused) {
            drawLaneBoard(font, play, drawMs, laneDown, settings, &background, skin);
            drawPauseMenu(font, play, settings, pauseSelected, skin);
        } else if (scene == Scene::Countdown) {
            drawLaneBoard(font, play, drawMs, laneDown, settings, &background, skin);
            if (!countdownStartsAudio || nowMs() - countdownStartMs < static_cast<uint64_t>(COUNTDOWN_MS)) {
                drawCountdown(font, countdownStartMs);
            }
        } else {
            drawResults(font, play, &background, skin);
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();

        previousButtons = buttons;
    }

    audio.stop();
    previewAudio.stop();
    previewCache.stop();
    samples.stop();
    background.clear();
    skin.free();
    if (font) {
        vita2d_free_pgf(font);
    }
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
