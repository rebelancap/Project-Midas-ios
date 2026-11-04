/**
 * PC-port audio stubs — Phase-4 deferral of the AL synthesizer.
 *
 * The real libultra audio library (src/libultra/audio + src/libultrare) is
 * scheduled for Phase 4. Until then every al* symbol the kept game code
 * references is satisfied here so the game runs SILENT without crashing.
 *
 * Design rules used below (each stub carries its own rationale):
 *  - Functions whose results feed loops/derefs get REAL minimal
 *    implementations (alLink/alUnlink, alHeapInit/alHeapDBAlloc).
 *  - Functions that only drive the (never-running) synthesizer are no-ops.
 *  - Query functions return the value that makes every audited caller take
 *    its benign exit path (alCSPGetState -> AL_STOPPED, alEvtqNextEvent ->
 *    positive delta, alCents2Ratio -> 1.0).
 *  - The SFX sound-bank consumed by snd.c sndPlaySfx() cannot be stubbed
 *    with zeros (it is deref'd 3 levels deep before any al* call), so a
 *    minimal fake bank graph is provided (portPcmBank below) and wired in
 *    via the crafted _sfxctl/_instrumentsctl segment blobs in
 *    port/src/segments.c.
 *
 * Callers audited: src/audi.c (amCreateAudioManager/amHandleFrameMessage),
 * src/music.c (musicSeqPlayerInit/musicTrackXPlay/Stop/ApplySeqpVol),
 * src/snd.c (sndNewPlayerInit/sndPlaySfx/sndSetupSound/sndPlayerVoiceHandler).
 */
#include <string.h>
#include <stdint.h>

#include <ultra64.h>
#include <PR/libaudio.h>

/* ==== AUDIO PHASE 1c ===================================================== *
 * The REAL AL library is now compiled (see CMakeLists' audio block): sl.c
 * provides alGlobals/alInit/alClose/alLink/alUnlink; heapinit/heapalloc
 * the heap; event.c the event queues; synthesizer.c + syn*.c the voices;
 * load/resample/env_wip/save/buses/reverb/drvrNew the filter chain.
 * port/src/audio_pc.c executes the emitted Acmd lists and feeds SDL.
 * The stubs that remain below are ONLY the compressed-MIDI sequencer
 * layer (music phase) and the BE-aware alBnkfNew. */

/* ==== bank file ========================================================== */

/* ==== bank file (REAL, audio phase 1) ==================================== */
/*
 * The _sfxctl/_instrumentsctl segments now carry the REAL SGI .ctl images
 * (byte-exact BIG-ENDIAN, revision 'B1'; see port/src/segments.c). This is
 * bnkf.c's patch walk with the BE->native field conversion folded in: on
 * N64 native IS big-endian so SGI reads fields directly; on PC every
 * multibyte field must be swapped exactly once, then offsets promoted to
 * host pointers (i686 build: 4-byte pointers overwrite 4-byte offsets in
 * place, same as N64).
 *
 * Guards against double work: bank/instrument/sound/wavetable records
 * carry a flags byte (SGI's own revisit guard, reused). Envelopes, ADPCM
 * books and loop records have NO flags field and CAN be shared, so their
 * content-swap is tracked in a visited set; the pointer fields referencing
 * them are per-parent and always patched.
 *
 * Format verified against assets/music/sfx.ctl before writing this:
 * revision B1, bankCount 1, bank @0x5BA0 {instCount 1, sampleRate 22050,
 * instArray[0]=0x5778}, instrument {vol 7F, pan 40, bendRange 200,
 * soundCount 261, soundArray at +16} - STANDARD layout (GE's snd.h "Alt"
 * view reads soundArray at +12 with 1-based sfx indices; the addresses
 * coincide, so both views work off this patch).
 */

static u16 portBSwap16(u16 v)
{
    return (u16)((v >> 8) | (v << 8));
}

static u32 portBSwap32(u32 v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v & 0xFF00u) << 8) | (v << 24);
}

#if defined(PLATFORM_64BIT)
extern void portConLogf(const char *fmt, ...);
static void portConLogfBnkf(s16 rev)
{
    portConLogf("ge: alBnkfNew: bank revision 0x%04X is not a converted 'B1' image - left alone\n", (u32)(u16)rev);
}
#endif

#define BNKF_VISIT_MAX 4096

static const void *s_bnkfVisited[BNKF_VISIT_MAX];
static s32 s_bnkfVisitedCount;

/* returns 1 the first time ptr is seen, 0 after */
static s32 portBnkfVisit(const void *ptr)
{
    s32 i;

    for (i = 0; i < s_bnkfVisitedCount; i++) {
        if (s_bnkfVisited[i] == ptr) {
            return 0;
        }
    }
    if (s_bnkfVisitedCount < BNKF_VISIT_MAX) {
        s_bnkfVisited[s_bnkfVisitedCount++] = ptr;
    }
    return 1;
}

#if defined(PLATFORM_64BIT)
/*
 * 64-bit hosts (ripple round, docs/port-ios/10-ripples.md A4). The 32-bit
 * patch walk below cannot run here: it rewrites 4-byte ROM offset slots as
 * 8-byte pointers in place (R16 class). music.c's D37 arm has already run
 * romdataFixupAudioBank (port/src/romdata.c, ported from the donor), which
 * re-lays the bank tree out in host order with 8-byte pointer slots holding
 * image-relative offsets (0 = absent) and wavetable bases holding .tbl
 * offsets. What is left is SGI bnkf.c's rebase plus Midas's RAW16 sample
 * swap. The ROM image (and so the .tbl) is a private, writable commit inside
 * the guest window, so `table + offset` is a window pointer whose (u32) is a
 * lossless guest address (audi.c's DMA callback hands it to the Acmd list as
 * an s32).
 */
#include <stddef.h>

_Static_assert(offsetof(ALBankFile, bankArray) == 8, "ALBankFile layout (romdataFixupAudioBank)");
_Static_assert(offsetof(ALBank, sampleRate) == 4 && offsetof(ALBank, percussion) == 8 &&
               offsetof(ALBank, instArray) == 16, "ALBank layout (romdataFixupAudioBank)");
_Static_assert(offsetof(ALInstrument, bendRange) == 12 && offsetof(ALInstrument, soundCount) == 14 &&
               offsetof(ALInstrument, soundArray) == 16, "ALInstrument layout (romdataFixupAudioBank)");
_Static_assert(offsetof(ALSound, keyMap) == 8 && offsetof(ALSound, wavetable) == 16 &&
               offsetof(ALSound, samplePan) == 24 && offsetof(ALSound, flags) == 26, "ALSound layout");
_Static_assert(offsetof(ALWaveTable, len) == 8 && offsetof(ALWaveTable, type) == 12 &&
               offsetof(ALWaveTable, flags) == 13 && offsetof(ALWaveTable, waveInfo.adpcmWave.loop) == 16 &&
               offsetof(ALWaveTable, waveInfo.adpcmWave.book) == 24 &&
               offsetof(ALWaveTable, waveInfo.rawWave.loop) == 16, "ALWaveTable layout");
_Static_assert(offsetof(ALEnvelope, attackVolume) == 12 && offsetof(ALADPCMloop, state) == 12 &&
               offsetof(ALADPCMBook, book) == 8, "ALEnvelope/ALADPCMloop/ALADPCMBook layout");

/* image-relative offset in a pointer slot -> live pointer (0 stays NULL) */
#define BNKF_REBASE(type, slot, base) \
    ((slot) = (slot) ? (type)((uintptr_t)(base) + (uintptr_t)(slot)) : (type)NULL)

static void portBnkfPatchSound(ALSound *s, uintptr_t base, uintptr_t table)
{
    ALWaveTable *w;

    if (s->flags) {
        return;
    }
    s->flags = 1;

    BNKF_REBASE(ALEnvelope *, s->envelope, base);
    BNKF_REBASE(ALKeyMap *, s->keyMap, base);
    BNKF_REBASE(ALWaveTable *, s->wavetable, base);
    w = s->wavetable;
    if (w == NULL || w->flags) {
        return;
    }
    w->flags = 1;
    w->base = (u8 *)(table + (uintptr_t)w->base);
    if (w->type == AL_ADPCM_WAVE) {
        BNKF_REBASE(ALADPCMBook *, w->waveInfo.adpcmWave.book, base);
        BNKF_REBASE(ALADPCMloop *, w->waveInfo.adpcmWave.loop, base);
    } else if (w->type == AL_RAW16_WAVE) {
        BNKF_REBASE(ALRawLoop *, w->waveInfo.rawWave.loop, base);
        /* RAW16 samples are big-endian s16 in the .tbl, the mixer plays
         * native s16: swap once (visited-guarded), as the 32-bit arm does. */
        if (portBnkfVisit(w->base)) {
            s16 *smp = (s16 *)w->base;
            s32 ns = w->len >> 1;
            s32 k;

            for (k = 0; k < ns; k++) {
                smp[k] = (s16)portBSwap16((u16)smp[k]);
            }
        }
    }
}

static void portBnkfPatchInst(ALInstrument *inst, uintptr_t base, uintptr_t table)
{
    s32 i;

    if (inst->flags) {
        return;
    }
    inst->flags = 1;

    for (i = 0; i < inst->soundCount; i++) {
        BNKF_REBASE(ALSound *, inst->soundArray[i], base);
        if (inst->soundArray[i] != NULL) {
            portBnkfPatchSound(inst->soundArray[i], base, table);
        }
    }
}

static void portBnkfPatchBank(ALBank *bank, uintptr_t base, uintptr_t table)
{
    s32 i;

    if (bank->flags) {
        return;
    }
    bank->flags = 1;

    BNKF_REBASE(ALInstrument *, bank->percussion, base);
    if (bank->percussion != NULL) {
        portBnkfPatchInst(bank->percussion, base, table);
    }
    for (i = 0; i < bank->instCount; i++) {
        BNKF_REBASE(ALInstrument *, bank->instArray[i], base);
        if (bank->instArray[i] != NULL) {
            portBnkfPatchInst(bank->instArray[i], base, table);
        }
    }
}

void alBnkfNew(ALBankFile *f, u8 *table)
{
    uintptr_t base = (uintptr_t)f;
    s32 i;

    s_bnkfVisitedCount = 0;

    if (f->revision != AL_BANK_VERSION) {
        /* romdataFixupAudioBank did not convert it (bad ROM data / OOM) */
        portConLogfBnkf(f->revision);
        return;
    }
    for (i = 0; i < f->bankCount; i++) {
        BNKF_REBASE(ALBank *, f->bankArray[i], base);
        if (f->bankArray[i] != NULL) {
            portBnkfPatchBank(f->bankArray[i], base, (uintptr_t)table);
        }
    }

    { /* bring-up (GE_DUMP_BOND): prove the parse - bank/inst/sound shape */
        extern void ge_dbg_vec(const char *tag, const float *v, int n);
        ALBank *bank = f->bankArray[0];
        float v[7];

        if (bank == NULL || bank->instArray[0] == NULL || bank->instArray[0]->soundArray[0] == NULL ||
            bank->instArray[0]->soundArray[0]->wavetable == NULL) {
            return;
        }
        v[0] = (float)f->bankCount;
        v[1] = (float)bank->instCount;
        v[2] = (float)bank->sampleRate;
        v[3] = (float)bank->instArray[0]->soundCount;
        v[4] = (float)bank->instArray[0]->soundArray[0]->wavetable->type;
        v[5] = (float)bank->instArray[0]->soundArray[0]->wavetable->len;
        v[6] = (float)(u32)(uintptr_t)bank->instArray[0]->soundArray[0]->wavetable->base;
        ge_dbg_vec("bnkf banks/insts/rate/sounds/wt0type/len/base", v, 7);
    }
}
#else /* !PLATFORM_64BIT: Midas's 32-bit in-place patch walk, unchanged */
static void portBnkfPatchSound(ALSound *s, uintptr_t base, uintptr_t table)
{
    ALWaveTable *w;

    if (s->flags) {
        return;
    }
    s->flags = 1;

    s->envelope = (ALEnvelope *)(base + portBSwap32((u32)s->envelope));
    if (portBnkfVisit(s->envelope)) {
        s->envelope->attackTime = (s32)portBSwap32((u32)s->envelope->attackTime);
        s->envelope->decayTime = (s32)portBSwap32((u32)s->envelope->decayTime);
        s->envelope->releaseTime = (s32)portBSwap32((u32)s->envelope->releaseTime);
    }

    /* keymap fields are all bytes - promote the pointer, nothing to swap */
    s->keyMap = (ALKeyMap *)(base + portBSwap32((u32)s->keyMap));

    s->wavetable = (ALWaveTable *)(base + portBSwap32((u32)s->wavetable));
    w = s->wavetable;
    if (!w->flags) {
        w->flags = 1;
        w->base = (u8 *)(table + portBSwap32((u32)w->base));
        w->len = (s32)portBSwap32((u32)w->len);

        if (w->type == AL_ADPCM_WAVE) {
            if (w->waveInfo.adpcmWave.book != NULL) {
                ALADPCMBook *book = (ALADPCMBook *)(base + portBSwap32((u32)w->waveInfo.adpcmWave.book));

                w->waveInfo.adpcmWave.book = book;
                if (portBnkfVisit(book)) {
                    s32 n;

                    book->order = (s32)portBSwap32((u32)book->order);
                    book->npredictors = (s32)portBSwap32((u32)book->npredictors);
                    for (n = 0; n < book->order * book->npredictors * 8; n++) {
                        book->book[n] = (s16)portBSwap16((u16)book->book[n]);
                    }
                }
            }
            if (w->waveInfo.adpcmWave.loop != NULL) {
                ALADPCMloop *loop = (ALADPCMloop *)(base + portBSwap32((u32)w->waveInfo.adpcmWave.loop));

                w->waveInfo.adpcmWave.loop = loop;
                if (portBnkfVisit(loop)) {
                    s32 n;

                    loop->start = portBSwap32(loop->start);
                    loop->end = portBSwap32(loop->end);
                    loop->count = portBSwap32(loop->count);
                    for (n = 0; n < 16; n++) {
                        ((s16 *)loop->state)[n] = (s16)portBSwap16((u16)((s16 *)loop->state)[n]);
                    }
                }
            }
        } else if (w->type == AL_RAW16_WAVE) {
            if (w->waveInfo.rawWave.loop != NULL) {
                ALRawLoop *loop = (ALRawLoop *)(base + portBSwap32((u32)w->waveInfo.rawWave.loop));

                w->waveInfo.rawWave.loop = loop;
                if (portBnkfVisit(loop)) {
                    loop->start = portBSwap32(loop->start);
                    loop->end = portBSwap32(loop->end);
                    loop->count = portBSwap32(loop->count);
                }
            }

            /* RAW16 samples are big-endian s16 in the .tbl but the mixer
             * plays native s16, so swap them once (visited-guarded). ADPCM
             * decodes bytes and needs no swap. */
            if (portBnkfVisit(w->base)) {
                s16 *smp = (s16 *)w->base;
                s32 ns = w->len >> 1;
                s32 k;

                for (k = 0; k < ns; k++) {
                    smp[k] = (s16)portBSwap16((u16)smp[k]);
                }
            }
        }
    }
}

static void portBnkfPatchInst(ALInstrument *inst, uintptr_t base, uintptr_t table)
{
    s32 i;

    if (inst->flags) {
        return;
    }
    inst->flags = 1;

    inst->bendRange = (s16)portBSwap16((u16)inst->bendRange);
    inst->soundCount = (s16)portBSwap16((u16)inst->soundCount);

    for (i = 0; i < inst->soundCount; i++) {
        inst->soundArray[i] = (ALSound *)(base + portBSwap32((u32)inst->soundArray[i]));
        portBnkfPatchSound(inst->soundArray[i], base, table);
    }
}

static void portBnkfPatchBank(ALBank *bank, uintptr_t base, uintptr_t table)
{
    s32 i;

    if (bank->flags) {
        return;
    }
    bank->flags = 1;

    bank->instCount = (s16)portBSwap16((u16)bank->instCount);
    bank->sampleRate = (s32)portBSwap32((u32)bank->sampleRate);

    if (bank->percussion != NULL) {
        bank->percussion = (ALInstrument *)(base + portBSwap32((u32)bank->percussion));
        portBnkfPatchInst(bank->percussion, base, table);
    }

    for (i = 0; i < bank->instCount; i++) {
        bank->instArray[i] = (ALInstrument *)(base + portBSwap32((u32)bank->instArray[i]));
        if (bank->instArray[i] != (ALInstrument *)base) {
            portBnkfPatchInst(bank->instArray[i], base, table);
        }
    }
}

void alBnkfNew(ALBankFile *f, u8 *table)
{
    uintptr_t base = (uintptr_t)f;
    s32 i;

    s_bnkfVisitedCount = 0;

    f->revision = (s16)portBSwap16((u16)f->revision);
    if (f->revision != AL_BANK_VERSION) {
        /* not a BE ctl image (or already patched) - leave it alone */
        return;
    }
    f->bankCount = (s16)portBSwap16((u16)f->bankCount);

    for (i = 0; i < f->bankCount; i++) {
        f->bankArray[i] = (ALBank *)(base + portBSwap32((u32)f->bankArray[i]));
        if (f->bankArray[i] != (ALBank *)base) {
            portBnkfPatchBank(f->bankArray[i], base, (uintptr_t)table);
        }
    }

    { /* bring-up (GE_DUMP_BOND): prove the parse - bank/inst/sound shape */
        extern void ge_dbg_vec(const char *tag, const float *v, int n);
        ALBank *bank = f->bankArray[0];
        float v[7];

        v[0] = (float)f->bankCount;
        v[1] = (float)bank->instCount;
        v[2] = (float)bank->sampleRate;
        v[3] = (float)bank->instArray[0]->soundCount;
        v[4] = (float)bank->instArray[0]->soundArray[0]->wavetable->type;
        v[5] = (float)bank->instArray[0]->soundArray[0]->wavetable->len;
        v[6] = (float)(uintptr_t)bank->instArray[0]->soundArray[0]->wavetable->base;
        ge_dbg_vec("bnkf banks/insts/rate/sounds/wt0type/len/base", v, 7);
    }
}
#endif /* PLATFORM_64BIT */

/* ==== compressed-midi seq / seq player =================================== */

/**
 * Real alCSeqNew parses a compressed MIDI stream header. The stream our
 * crafted sample table produces is an empty deflate block (decompresses
 * to 0 bytes), so there is nothing to parse; zero the struct so any later
 * reader sees a consistent "empty sequence".
 */
/* (The compressed-MIDI sequencer stubs - alCSeqNew, the alCSP set, and
 * alSeqpSetBank - were retired with the MUSIC PHASE: cseq.c, csplayer.c,
 * the csp wrappers and seqpsetbank.c are compiled for real now, and the
 * real music.sbk feeds them via _musicsampletblSegmentRomStart.) */

/* (alCents2Ratio, alAudioFrame, the alSyn* voice API, and the fake
 * portPcmBank graph were retired with AUDIO PHASE 1c: the real library
 * provides the former, and the real banks replaced the latter.) */
