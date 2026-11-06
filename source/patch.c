/**
 * @file patch.c
 * @brief Asphalt 4 hooks: SoundEngine BGM, tilt steering from the analog stick, device language,
 *        physical buttons -> the game's own touch zones, and the remappable controls.txt.
 * @details Evidence for every address/offset is in PORTING_PLAN.md (Phase 3/7/8 notes).
 */
#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/system_param.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "coreaudio.h"
#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "patch.h"

const import_entry patch_imports[] = {{"", 0, 0}};
const int patch_imports_count = 0;

// ---------------------------------------------------------------- controls.txt

#define CONTROLS_PATH DATA_PATH "controls.txt"
#define ACT_NONE 0
#define ACT_NITRO -1 // GetPhoneRotSpeedFwdBack > 12288 -> flags |= 0x24 (CPlayingGame::UpdateGame 0x35ccc)
#define ACT_STEER_L -2
#define ACT_STEER_R -3

static const struct {
    const char *name;
    uint32_t mask;
} btn_names[] = {
    {"CROSS", SCE_CTRL_CROSS},       {"CIRCLE", SCE_CTRL_CIRCLE}, {"SQUARE", SCE_CTRL_SQUARE},
    {"TRIANGLE", SCE_CTRL_TRIANGLE}, {"L", SCE_CTRL_LTRIGGER},   {"R", SCE_CTRL_RTRIGGER},
    {"START", SCE_CTRL_START},       {"SELECT", SCE_CTRL_SELECT}, {"UP", SCE_CTRL_UP},
    {"DOWN", SCE_CTRL_DOWN},         {"LEFT", SCE_CTRL_LEFT},     {"RIGHT", SCE_CTRL_RIGHT},
};
#define NBTN (sizeof(btn_names) / sizeof(btn_names[0]))

// action per button: ACT_* or a CTouchZones zone id (> 0). Zone ids from CTouchZones::AddZone call
// sites: 7/8 (CPlayingGame::DrawCarControls, ZonePressed: flag bits 0/1), 101 (bit 3), 102 (0x824,
// includes the nitro bits 0x24), 103/105/106 (CPlayingGame::DrawGame), 0 (splash "any key").
static int btn_action[NBTN];
static int steer_deadzone = 10, steer_sens = 100, steer_invert = 0;
int patch_screen_flip;

static const char default_controls[] =
    "# Asphalt 4 Elite Racing - PS Vita controls. Format: BUTTON = ACTION\n"
    "# BUTTON: CROSS CIRCLE SQUARE TRIANGLE L R START SELECT UP DOWN LEFT RIGHT\n"
    "# ACTION: NITRO (= iPhone tilt forward), STEER_LEFT, STEER_RIGHT, NONE,\n"
    "#         ZONE:<id> (holds a finger on the game's touch zone <id>; logged as [INPUT] zones)\n"
    "# The left stick steers (= iPhone tilt). The front touch screen always works as on iPhone.\n"
    "CROSS = NITRO\n"
    "SQUARE = ZONE:101\n"
    "CIRCLE = ZONE:102\n"
    "TRIANGLE = ZONE:106\n"
    "L = ZONE:7\n"
    "R = ZONE:8\n"
    "START = ZONE:103\n"
    "SELECT = ZONE:105\n"
    "LEFT = STEER_LEFT\n"
    "RIGHT = STEER_RIGHT\n"
    "UP = NONE\n"
    "DOWN = NONE\n"
    "STEER_DEADZONE = 10\n"
    "STEER_SENSITIVITY = 100\n"
    "STEER_INVERT = 0\n"
    "# 1 = the picture is upside down: rotate the screen (and the touch mapping) by 180 degrees\n"
    "SCREEN_FLIP = 0\n";

static int parse_action(const char *v) {
    if (!strncasecmp(v, "ZONE:", 5))
        return atoi(v + 5) > 0 ? atoi(v + 5) : ACT_NONE;
    if (!strcasecmp(v, "NITRO"))
        return ACT_NITRO;
    if (!strcasecmp(v, "STEER_LEFT"))
        return ACT_STEER_L;
    if (!strcasecmp(v, "STEER_RIGHT"))
        return ACT_STEER_R;
    return ACT_NONE;
}

static void parse_controls(char *text) {
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char key[32], val[32];
        if (*line == '#' || sscanf(line, " %31[A-Za-z_] = %31s", key, val) != 2)
            continue;
        if (!strcasecmp(key, "STEER_DEADZONE"))
            steer_deadzone = atoi(val) < 0 ? 0 : atoi(val) > 100 ? 100 : atoi(val);
        else if (!strcasecmp(key, "STEER_SENSITIVITY"))
            steer_sens = atoi(val) < 10 ? 10 : atoi(val) > 400 ? 400 : atoi(val);
        else if (!strcasecmp(key, "STEER_INVERT"))
            steer_invert = atoi(val) != 0;
        else if (!strcasecmp(key, "SCREEN_FLIP"))
            patch_screen_flip = atoi(val) != 0;
        else
            for (unsigned i = 0; i < NBTN; i++)
                if (!strcasecmp(key, btn_names[i].name))
                    btn_action[i] = parse_action(val);
    }
}

static void load_controls(void) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", default_controls);
    parse_controls(buf); // defaults first: a partial user file keeps the rest
    SceUID fd = sceIoOpen(CONTROLS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        fd = sceIoOpen(CONTROLS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, default_controls, sizeof(default_controls) - 1);
            sceIoClose(fd);
        }
        l_info("[INPUT] %s missing: defaults written", CONTROLS_PATH);
    } else {
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        buf[n > 0 ? n : 0] = 0;
        parse_controls(buf);
    }
    char line[512];
    int o = 0;
    for (unsigned i = 0; i < NBTN; i++)
        o += snprintf(line + o, sizeof(line) - o, "%s=%d ", btn_names[i].name, btn_action[i]);
    l_info("[INPUT] controls: %s deadzone %d sens %d%% invert %d screen_flip %d", line, steer_deadzone, steer_sens,
           steer_invert, patch_screen_flip);
}

// ---------------------------------------------------------------- tilt -> analog stick

// GetPhoneRotLeftRight(orientation) (0x60dc8) returns atan2(accel) * 4096 / (pi/2): -4096..4096.
// CPlayingGame::UpdateGame negates it into touchWeelDir; |touchWeelDir| > 1024 sets the steer bits
// 0x200 / 0x400. The stick is read here at call time (game thread) - pad is updated by the main loop.
static int stick_steer(void) {
    int x = (int)pad.lx - 128;
    for (unsigned i = 0; i < NBTN; i++)
        if (pad.buttons & btn_names[i].mask) {
            if (btn_action[i] == ACT_STEER_L)
                x = -127;
            else if (btn_action[i] == ACT_STEER_R)
                x = 127;
        }
    int ax = x < 0 ? -x : x;
    if (ax <= steer_deadzone)
        return 0;
    int v = (ax - steer_deadzone) * 2048 / (127 - steer_deadzone); // full stick = 2x the steer threshold
    v = v * steer_sens / 100;
    if (v > 4096)
        v = 4096;
    if (x < 0)
        v = -v;
    return steer_invert ? v : -v; // touchWeelDir = -ret: stick right -> positive touchWeelDir
}

static int hook_GetPhoneRotLeftRight(int orientation) { return stick_steer(); }

static int hook_GetPhoneRotSpeedFwdBack(int orientation) {
    for (unsigned i = 0; i < NBTN; i++)
        if (btn_action[i] == ACT_NITRO && (pad.buttons & btn_names[i].mask))
            return 16384; // > 12288: "phone tilted forward" = nitro
    return 0;
}

// ---------------------------------------------------------------- SoundEngine BGM

// Apple SoundEngine (CrashLanding) BackgroundTrackMgr streams the ALAC .m4a through AudioQueue; it
// is replaced by an OpenAL source playing the offline-converted .wav (tools/convert_media.sh).
// CSoundWrap::MusicLoad (0x5cf6c) passes g_AppPath + name; tracks loop like BackgroundTrackMgr.
static int hook_LoadBackgroundMusicTrack(const char *path, int add_to_queue, int load_at_once) {
    return sound_bgm_load(path, 1, 0);
}
static int hook_StartBackgroundMusic(void) { return sound_bgm_start(1); }
static int hook_StopBackgroundMusic(int stop_at_end) { return sound_bgm_stop_at_end(stop_at_end & 0xFF); }
static int hook_UnloadBackgroundMusicTrack(void) { return sound_bgm_unload(); }
static int hook_SetBackgroundMusicVolume(float vol) { return sound_bgm_set_volume(vol); } // softfp: vol in r0
static int hook_IsMusicPlaying(void) { return sound_bgm_is_playing(); }

// ---------------------------------------------------------------- language

// GetDeviceLanguage(char*) (0x93f28) sprintf's AppleLanguages[0]; the bundle has de/en/es/fr/it/ja.lproj.
static void hook_GetDeviceLanguage(char *out) {
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    const char *code = "en";
    switch (lang) {
    case SCE_SYSTEM_PARAM_LANG_FRENCH: code = "fr"; break;
    case SCE_SYSTEM_PARAM_LANG_SPANISH: code = "es"; break;
    case SCE_SYSTEM_PARAM_LANG_GERMAN: code = "de"; break;
    case SCE_SYSTEM_PARAM_LANG_ITALIAN: code = "it"; break;
    case SCE_SYSTEM_PARAM_LANG_JAPANESE: code = "ja"; break;
    }
    if (out)
        strcpy(out, code);
    static int logged;
    if (!logged++)
        l_info("[BOOT] GetDeviceLanguage -> %s (Vita language %d)", code, lang);
}

// ---------------------------------------------------------------- buttons -> touch zones

// CTouchZones* = (*CSingletonManager::c_pTls)->[+4]->[+0x28] (_UpdateTouchPress 0x5cbb8).
// +0x10: zone array, +0x14: s16 count; 10-byte zone = u8 id, s16 x1 (+2), x2 (+4), y1 (+6), y2 (+8)
// in the landscape 480x320 space (CTouchZones::AddZone 0x64098). Zones are rebuilt every frame by
// the game thread, so the last seen centre of each id is cached.
typedef struct {
    int id, x, y;
} zone_pos;
static zone_pos zcache[32];
static int nzcache;

static uint8_t *touch_zones(void) {
    static uint32_t *tls;
    if (!tls)
        tls = (uint32_t *)macho_sym("__ZN17CSingletonManager6c_pTlsE");
    uint32_t sm = tls ? *tls : 0, tbl = sm ? ((uint32_t *)sm)[1] : 0;
    return tbl ? *(uint8_t **)(tbl + 0x28) : NULL;
}

static void scan_zones(void) {
    uint8_t *tz = touch_zones();
    if (!tz)
        return;
    uint8_t *z = *(uint8_t **)(tz + 0x10);
    int n = *(int16_t *)(tz + 0x14);
    for (int i = 0; z && i < n && i < 64; i++) {
        uint8_t *e = z + i * 10;
        int16_t x1 = *(int16_t *)(e + 2), x2 = *(int16_t *)(e + 4), y1 = *(int16_t *)(e + 6), y2 = *(int16_t *)(e + 8);
        if (x1 < 0 || y1 < 0 || x2 <= x1 || y2 <= y1 || x2 > 480 || y2 > 320)
            continue;
        int k = 0;
        while (k < nzcache && zcache[k].id != e[0])
            k++;
        if (k == nzcache) {
            if (nzcache == 32)
                continue;
            nzcache++;
            l_info("[INPUT] zone %d at %d,%d-%d,%d", e[0], x1, y1, x2, y2);
        }
        zcache[k] = (zone_pos){e[0], (x1 + x2) / 2, (y1 + y2) / 2};
    }
}

static void add_touch(int id, int x, int y) {
    if (touch_count >= SCE_TOUCH_MAX_REPORT)
        return;
    touch_point *t = &touches[touch_count++];
    t->id = id;
    t->gx = x < 0 ? 0 : x > 479 ? 479 : x;
    t->gy = y < 0 ? 0 : y > 319 ? 319 : y;
    t->down = 1;
}

// In a race the CTouchZones listener (tz+0xc) is the CPlayingGame, whose vtable slots 0/1 are
// ZonePressed/ZoneReleased(short id) (0x2c08c/0x2c1a8): 7/8 steer (flag bits 0/1), 101 brake
// (bit 3), 102 nitro (0x824). Injected touches never reached them (console run #6), so the
// buttons and the stick call them directly, on the game thread (ctx_present), on press/release.
static const int race_ids[] = {7, 8, 101, 102};
#define NRACE (sizeof(race_ids) / sizeof(race_ids[0]))

static int is_race_id(int id) {
    for (unsigned k = 0; k < NRACE; k++)
        if (race_ids[k] == id)
            return 1;
    return 0;
}

static void *race_listener(void) {
    static uint32_t vt;
    if (!vt) {
        uintptr_t s = macho_sym("__ZTV12CPlayingGame");
        vt = s ? (uint32_t)s + 8 : 1;
    }
    uint8_t *tz = touch_zones();
    void *l = tz ? *(void **)(tz + 0xc) : NULL;
    return l && *(uint32_t *)l == vt ? l : NULL;
}

void patch_game_frame(void) {
    static void *last;
    static int held[NRACE];
    void *l = race_listener();
    if (l != last) { // new race / back from pause: start from a released state
        last = l;
        for (unsigned k = 0; k < NRACE; k++) {
            if (l)
                ((void (*)(void *, int))(*(uint32_t **)l)[1])(l, race_ids[k]);
            held[k] = 0;
        }
    }
    if (!l)
        return;
    int x = (int)pad.lx - 128, want[NRACE] = {x < -steer_deadzone - 30, x > steer_deadzone + 30, 0, 0};
    for (unsigned i = 0; i < NBTN; i++) {
        if (!(pad.buttons & btn_names[i].mask))
            continue;
        int a = btn_action[i];
        a = a == ACT_STEER_L ? 7 : a == ACT_STEER_R ? 8 : a == ACT_NITRO ? 102 : a;
        for (unsigned k = 0; k < NRACE; k++)
            want[k] |= race_ids[k] == a;
    }
    if (steer_invert) {
        int t = want[0];
        want[0] = want[1];
        want[1] = t;
    }
    for (unsigned k = 0; k < NRACE; k++)
        if (want[k] != held[k]) {
            held[k] = want[k];
            ((void (*)(void *, int))(*(uint32_t **)l)[want[k] ? 0 : 1])(l, race_ids[k]);
        }
}

void patch_input(void) {
    static int loaded;
    if (!loaded++)
        load_controls();
    scan_zones();
    int racing = race_listener() != NULL;
    for (unsigned i = 0; i < NBTN; i++) {
        if (btn_action[i] <= 0 || !(pad.buttons & btn_names[i].mask) || (racing && is_race_id(btn_action[i])))
            continue;
        for (int k = 0; k < nzcache; k++)
            if (zcache[k].id == btn_action[i]) {
                add_touch(1000 + i, zcache[k].x, zcache[k].y);
                break;
            }
    }
}

// _LOGDEBUG (0x5cd4c) is compiled empty in the shipping build; its messages ("TRY TO OPEN [%s]",
// "FAILED TO CREATE FBO", "path:[%s]") are routed to the log as [GAME] lines for console triage.
static void hook_LOGDEBUG(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), fmt ? fmt : "(null)", &w);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    l_debug("[GAME] %s", buf);
}

void patch_overlay(void) {}

void patch_frame(void) {}

void patch_init(void) {
    l_info("Initializing Asphalt 4 patches...");
    macho_hook("_SoundEngine_LoadBackgroundMusicTrack", hook_LoadBackgroundMusicTrack);
    macho_hook("_SoundEngine_StartBackgroundMusic", hook_StartBackgroundMusic);
    macho_hook("_SoundEngine_StopBackgroundMusic", hook_StopBackgroundMusic);
    macho_hook("_SoundEngine_UnloadBackgroundMusicTrack", hook_UnloadBackgroundMusicTrack);
    macho_hook("_SoundEngine_SetBackgroundMusicVolume", hook_SetBackgroundMusicVolume);
    macho_hook("_SoundEngine_IsMusicPlaying", hook_IsMusicPlaying);
    macho_hook("_GetPhoneRotLeftRight", hook_GetPhoneRotLeftRight);
    macho_hook("_GetPhoneRotSpeedFwdBack", hook_GetPhoneRotSpeedFwdBack);
    macho_hook("__Z17GetDeviceLanguagePc", hook_GetDeviceLanguage);
    macho_hook("_LOGDEBUG", hook_LOGDEBUG);
}
