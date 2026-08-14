// DOOM music, played on the Nano3s's real hardware buzzer.
//
// The buzzer (/dev/input/event1, same device fb_button.rs already drives
// for click feedback via EV_SND/SND_TONE) is a monophonic square-wave
// tone generator -- one frequency at a time, no polyphony, no volume
// control. This plugs into doomgeneric's real music_module_t interface
// (i_sound.h) so DOOM's own existing S_ChangeMusic()/level-start/
// intermission/menu logic decides *when* to play what, exactly as it
// already does for every other port -- this file only has to turn one
// MUS-derived MIDI track into a stream of buzzer tones.
//
// Pipeline: WAD music lump (MUS format) -> mus2mid() [already vendored,
// unmodified] -> standard MIDI file, always type 0 / single track / 70
// ticks per quarter note (confirmed by reading mus2mid.c's own fixed
// header) -> parsed here event-by-event on a dedicated thread that sleeps
// between events according to each delta-time -> monophonic note
// arbitration (last-note-priority with fallback to the next still-held
// note, like an old monophonic analog synth) -> EV_SND/SND_TONE.

#include "doomtype.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>

#define BEEPER_DEV "/dev/input/event1"
#define MIDI_DRUM_CHANNEL 9  // General MIDI channel 10 (0-based 9) -- not melodic, skipped

typedef struct {
    uint8_t *data;
    size_t len;
} bz_song_t;

// i_sound.c's I_BindSoundVariables() references these under #ifdef
// FEATURE_SOUND; normally defined in i_sdlsound.c, which isn't part of
// this build. We don't use resampling at all (no digital sound), so
// these just need to exist for the link to succeed.
int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

static pthread_t s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_thread_running = 0;
static volatile int s_stop_requested = 0;
static volatile int s_paused = 0;
static bz_song_t *s_current_song = NULL;
static int s_current_looping = 0;

// ── Buzzer I/O (same wire format as fb_button.rs's write_event/beep) ────

static void write_event(int fd, uint16_t type, uint16_t code, int32_t value) {
    uint8_t event[24];
    memset(event, 0, sizeof(event));
    memcpy(event + 16, &type, 2);
    memcpy(event + 18, &code, 2);
    memcpy(event + 20, &value, 4);
    ssize_t ignore = write(fd, event, sizeof(event));
    (void)ignore;
}

static void buzzer_set_tone(int fd, int freq_hz) {
    write_event(fd, 0x12 /*EV_SND*/, 0x02 /*SND_TONE*/, freq_hz);
    write_event(fd, 0x00 /*EV_SYN*/, 0, 0);
}

// ── Monophonic note arbitration ──────────────────────────────────────
// Tracks which (channel, note) pairs are currently held down. DOOM's MUS
// tracks typically run a rhythmically busy bass/percussion-adjacent part
// underneath a sustained melody; an earlier "last note triggered wins"
// scheme made the buzzer flip back and forth onto every short bass note,
// which is audible as crackle/noise concentrated on the low end (correct
// pitch, but constant re-triggering). Highest-held-note priority is the
// standard fix for reducing polyphonic game music to one monophonic
// voice -- melody usually sits on top, so a busy bass line underneath a
// held melody note no longer steals the buzzer at all.
#define MAX_HELD 32
typedef struct {
    int channel;
    int note;
} held_note_t;

static held_note_t s_held[MAX_HELD];
static int s_held_count = 0;
static int s_beeper_fd = -1;

// Identity (not just frequency) of whatever's currently sounding, so we
// can tell "the winning note is still the same note" (some unrelated
// lower note came or went, nothing should change) apart from "a genuinely
// new note-on became the winner" (even if it happens to be the same
// pitch as before -- two repeated melody notes should still be heard as
// two notes, not one continuous tone).
static int s_sounding_channel = -1;
static int s_sounding_note = -1;

// Set from the web gamepad's mute button (buzzer_music_set_muted, below).
// Arbitration/timing keep running exactly as normal while muted -- only
// the actual hardware write is suppressed -- so unmuting mid-song resumes
// in sync instead of restarting or drifting.
// Defaults to muted: audio starts silent every launch, unmute is an
// explicit opt-in from the gamepad page rather than something you have to
// race to silence. web_gamepad.c's page renders its mute button's initial
// label to match this default -- keep the two in sync if this changes.
static volatile int s_muted = 1;

#define ARTICULATION_GAP_MS 12

static double note_to_freq(int note) {
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

static void sound_highest_held(void) {
    if (s_held_count == 0) {
        if (s_sounding_note != -1) {
            s_sounding_channel = -1;
            s_sounding_note = -1;
            buzzer_set_tone(s_beeper_fd, 0);
        }
        return;
    }

    int winner = 0;
    for (int i = 1; i < s_held_count; i++) {
        if (s_held[i].note > s_held[winner].note) {
            winner = i;
        }
    }
    int channel = s_held[winner].channel;
    int note = s_held[winner].note;

    if (channel == s_sounding_channel && note == s_sounding_note) {
        // Same note that was already sounding (some other, lower held
        // note changed without affecting the winner) -- don't touch it.
        return;
    }

    // A genuinely new note is taking over the one available voice.
    // Brief mute-then-resound gives it real articulation instead of
    // gliding straight from the old pitch (or, for a repeated note,
    // instead of silently continuing the same tone as if nothing
    // happened) -- this is the same "staccato" trick classic PC-speaker
    // music emulation uses. It's a single deliberate gap per musical
    // note change (at most a few times a second even in busy passages),
    // not the rapid uncontrolled retriggering that caused the earlier
    // crackle bug -- that was many flips per second between two DIFFERENT
    // notes fighting for the voice, not one clean gap per real note.
    s_sounding_channel = channel;
    s_sounding_note = note;
    buzzer_set_tone(s_beeper_fd, 0);
    usleep(ARTICULATION_GAP_MS * 1000);
    buzzer_set_tone(s_beeper_fd, s_muted ? 0 : (int)(note_to_freq(note) + 0.5));
}

// Public, called from web_gamepad.c's /audio handler (the gamepad page's
// mute button). Note-tracking (s_held/s_sounding_*) keeps running
// untouched while muted -- only the hardware write is gated -- so this
// just needs to fix up the buzzer's actual state immediately on each
// transition rather than waiting for the next note change to notice.
void buzzer_music_set_muted(int new_muted) {
    s_muted = new_muted ? 1 : 0;
    if (s_beeper_fd < 0) {
        return;
    }
    if (s_muted) {
        buzzer_set_tone(s_beeper_fd, 0);
    } else if (s_sounding_note != -1) {
        buzzer_set_tone(s_beeper_fd, (int)(note_to_freq(s_sounding_note) + 0.5));
    }
}

int buzzer_music_is_muted(void) {
    return s_muted;
}

static void note_on(int channel, int note) {
    if (channel == MIDI_DRUM_CHANNEL || note < 0 || note > 127) {
        return;
    }
    if (s_held_count < MAX_HELD) {
        s_held[s_held_count].channel = channel;
        s_held[s_held_count].note = note;
        s_held_count++;
    }
    sound_highest_held();
}

static void note_off(int channel, int note) {
    for (int i = 0; i < s_held_count; i++) {
        if (s_held[i].channel == channel && s_held[i].note == note) {
            memmove(&s_held[i], &s_held[i + 1], (size_t)(s_held_count - i - 1) * sizeof(held_note_t));
            s_held_count--;
            break;
        }
    }
    sound_highest_held();
}

static void all_notes_off(void) {
    s_held_count = 0;
    s_sounding_channel = -1;
    s_sounding_note = -1;
    buzzer_set_tone(s_beeper_fd, 0);
}

// ── Minimal single-track Standard MIDI File reader ──────────────────
// mus2mid.c always emits type-0 (single MTrk), 70 ticks/quarter, and
// never writes a Set Tempo meta event -- so the SMF-spec default of
// 120 BPM (500000 us/quarter) applies unless one shows up anyway, which
// we still honor for robustness.

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} midi_reader_t;

static int mr_u8(midi_reader_t *r, uint8_t *out) {
    if (r->p >= r->end) return 0;
    *out = *r->p++;
    return 1;
}

static int mr_vlq(midi_reader_t *r, uint32_t *out) {
    uint32_t value = 0;
    uint8_t byte;
    for (int i = 0; i < 4; i++) {
        if (!mr_u8(r, &byte)) return 0;
        value = (value << 7) | (byte & 0x7F);
        if (!(byte & 0x80)) {
            *out = value;
            return 1;
        }
    }
    return 0;
}

static void *playback_thread(void *arg) {
    (void)arg;

    pthread_mutex_lock(&s_lock);
    bz_song_t *song = s_current_song;
    int looping = s_current_looping;
    pthread_mutex_unlock(&s_lock);

    if (!song || song->len < 22 || memcmp(song->data, "MThd", 4) != 0) {
        s_thread_running = 0;
        return NULL;
    }

    uint16_t division = (uint16_t)((song->data[12] << 8) | song->data[13]);
    if (division == 0) {
        division = 70;
    }

    // Find the MTrk chunk (always the second chunk in mus2mid's output,
    // but scan properly instead of assuming a fixed header size).
    const uint8_t *p = song->data + 14; // header size (14) already validated by mem == "MThd"
    const uint8_t *fend = song->data + song->len;
    const uint8_t *track = NULL;
    uint32_t track_len = 0;
    while (p + 8 <= fend) {
        uint32_t chunk_len = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
        if (memcmp(p, "MTrk", 4) == 0) {
            track = p + 8;
            track_len = chunk_len;
            break;
        }
        p += 8 + chunk_len;
    }

    if (!track || track + track_len > fend) {
        s_thread_running = 0;
        return NULL;
    }

    double us_per_quarter = 500000.0; // SMF default (120 BPM)

    do {
        midi_reader_t r = {track, track + track_len};
        uint8_t running_status = 0;

        while (r.p < r.end && !s_stop_requested) {
            uint32_t delta;
            if (!mr_vlq(&r, &delta)) break;

            if (delta > 0) {
                double ms = (delta * (us_per_quarter / division)) / 1000.0;
                // Sleep in short slices so a stop/pause request lands
                // promptly instead of after a multi-second note.
                while (ms > 0 && !s_stop_requested) {
                    while (s_paused && !s_stop_requested) {
                        usleep(20 * 1000);
                    }
                    double slice = ms > 20.0 ? 20.0 : ms;
                    usleep((useconds_t)(slice * 1000.0));
                    ms -= slice;
                }
            }
            if (s_stop_requested) break;

            uint8_t status;
            if (!mr_u8(&r, &status)) break;

            if (status < 0x80) {
                // Running status: this byte is actually the first data
                // byte of a repeat of the previous event type.
                if (running_status == 0) break;
                r.p--; // put the data byte back, we'll reread it below
                status = running_status;
            } else {
                running_status = status;
            }

            uint8_t hi = status & 0xF0;
            uint8_t channel = status & 0x0F;

            if (hi == 0x80 || hi == 0x90) {
                uint8_t note, vel;
                if (!mr_u8(&r, &note) || !mr_u8(&r, &vel)) break;
                if (hi == 0x90 && vel > 0) {
                    note_on(channel, note);
                } else {
                    note_off(channel, note);
                }
            } else if (hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
                uint8_t a, b;
                if (!mr_u8(&r, &a) || !mr_u8(&r, &b)) break;
            } else if (hi == 0xC0 || hi == 0xD0) {
                uint8_t a;
                if (!mr_u8(&r, &a)) break;
            } else if (status == 0xFF) {
                uint8_t meta_type;
                uint32_t meta_len;
                if (!mr_u8(&r, &meta_type) || !mr_vlq(&r, &meta_len)) break;
                if (meta_type == 0x51 && meta_len == 3 && r.p + 3 <= r.end) {
                    us_per_quarter = (double)((r.p[0] << 16) | (r.p[1] << 8) | r.p[2]);
                }
                if (meta_type == 0x2F) {
                    r.p += meta_len;
                    break; // end of track
                }
                r.p += meta_len;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sysex_len;
                if (!mr_vlq(&r, &sysex_len)) break;
                r.p += sysex_len;
            } else {
                break; // unrecognized status, bail rather than desync
            }
        }

        all_notes_off();
    } while (looping && !s_stop_requested);

    s_thread_running = 0;
    return NULL;
}

// Public, called from main()'s SIGTERM/SIGINT handler path (see
// doomgeneric_nano3s.c) so a normal `kill` actually silences the buzzer.
// SIGKILL can't be caught by any process -- the beeper is a stateful
// hardware device that keeps sounding whatever tone it was last told to
// play regardless of whether the writing process is still alive, so a
// hard kill leaves it stuck until something else explicitly silences it.
// Always stop with SIGTERM (plain `kill`), never SIGKILL/-9.
void nano3s_doom_emergency_silence(void) {
    s_stop_requested = 1;
    if (s_thread_running) {
        pthread_join(s_thread, NULL);
    }
    if (s_beeper_fd >= 0) {
        all_notes_off();
    }
}

// ── music_module_t interface ─────────────────────────────────────────

static boolean BZ_InitMusic(void) {
    s_beeper_fd = open(BEEPER_DEV, O_WRONLY);
    return s_beeper_fd >= 0;
}

static void BZ_ShutdownMusic(void) {
    s_stop_requested = 1;
    if (s_thread_running) {
        pthread_join(s_thread, NULL);
    }
    if (s_beeper_fd >= 0) {
        close(s_beeper_fd);
        s_beeper_fd = -1;
    }
}

static void BZ_SetMusicVolume(int volume) {
    (void)volume; // the buzzer has no volume control, only on/off
}

static void BZ_PauseMusic(void) {
    s_paused = 1;
    all_notes_off();
}

static void BZ_ResumeMusic(void) {
    s_paused = 0;
}

static void *BZ_RegisterSong(void *data, int len) {
    if (!data || len <= 0) {
        return NULL;
    }
    bz_song_t *song = malloc(sizeof(bz_song_t));
    if (!song) {
        return NULL;
    }

    if (len > 4 && memcmp(data, "MThd", 4) == 0) {
        // Already a standard MIDI file -- just take a copy.
        song->data = malloc((size_t)len);
        if (!song->data) {
            free(song);
            return NULL;
        }
        memcpy(song->data, data, (size_t)len);
        song->len = (size_t)len;
        return song;
    }

    // MUS format (the normal case for DOOM WAD music lumps).
    MEMFILE *instream = mem_fopen_read(data, (size_t)len);
    MEMFILE *outstream = mem_fopen_write();
    int result = mus2mid(instream, outstream);

    if (result == 0) {
        void *outbuf;
        size_t outbuf_len;
        mem_get_buf(outstream, &outbuf, &outbuf_len);
        song->data = malloc(outbuf_len);
        if (song->data) {
            memcpy(song->data, outbuf, outbuf_len);
            song->len = outbuf_len;
        }
    } else {
        song->data = NULL;
    }

    mem_fclose(instream);
    mem_fclose(outstream);

    if (!song->data) {
        free(song);
        return NULL;
    }
    return song;
}

static void BZ_UnRegisterSong(void *handle) {
    bz_song_t *song = (bz_song_t *)handle;
    if (song) {
        free(song->data);
        free(song);
    }
}

static void BZ_PlaySong(void *handle, boolean looping) {
    if (!handle || s_beeper_fd < 0) {
        return;
    }

    // Stop whatever's already playing before starting the new song.
    if (s_thread_running) {
        s_stop_requested = 1;
        pthread_join(s_thread, NULL);
    }

    pthread_mutex_lock(&s_lock);
    s_current_song = (bz_song_t *)handle;
    s_current_looping = looping ? 1 : 0;
    pthread_mutex_unlock(&s_lock);

    s_stop_requested = 0;
    s_paused = 0;
    s_thread_running = 1;
    if (pthread_create(&s_thread, NULL, playback_thread, NULL) != 0) {
        s_thread_running = 0;
    }
}

static void BZ_StopSong(void) {
    s_stop_requested = 1;
    if (s_thread_running) {
        pthread_join(s_thread, NULL);
    }
    all_notes_off();
}

static boolean BZ_MusicIsPlaying(void) {
    return s_thread_running != 0;
}

static void BZ_PollMusic(void) {
    // Timing is handled entirely by playback_thread's own sleeps; nothing
    // to do on the poll tick the main game loop calls.
}

music_module_t DG_music_module = {
    NULL, 0,
    BZ_InitMusic,
    BZ_ShutdownMusic,
    BZ_SetMusicVolume,
    BZ_PauseMusic,
    BZ_ResumeMusic,
    BZ_RegisterSong,
    BZ_UnRegisterSong,
    BZ_PlaySong,
    BZ_StopSong,
    BZ_MusicIsPlaying,
    BZ_PollMusic,
};

// No digital sound effects -- the buzzer is monophonic and already busy
// playing music; layering SFX on top would just fight it for the one
// available voice. A fully silent, always-successful sound module keeps
// the rest of the engine (which expects one to exist once FEATURE_SOUND
// is defined) happy without touching the buzzer.
static boolean BZS_Init(boolean use_sfx_prefix) { (void)use_sfx_prefix; return true; }
static void BZS_Shutdown(void) {}
static int BZS_GetSfxLumpNum(sfxinfo_t *sfxinfo) { (void)sfxinfo; return -1; }
static void BZS_Update(void) {}
static void BZS_UpdateSoundParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }
static int BZS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    (void)sfxinfo; (void)channel; (void)vol; (void)sep;
    return -1;
}
static void BZS_StopSound(int channel) { (void)channel; }
static boolean BZS_SoundIsPlaying(int channel) { (void)channel; return false; }
static void BZS_CacheSounds(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

sound_module_t DG_sound_module = {
    NULL, 0,
    BZS_Init,
    BZS_Shutdown,
    BZS_GetSfxLumpNum,
    BZS_Update,
    BZS_UpdateSoundParams,
    BZS_StartSound,
    BZS_StopSound,
    BZS_SoundIsPlaying,
    BZS_CacheSounds,
};
