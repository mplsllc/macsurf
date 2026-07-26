/*
 * ostls_entropy.c -- macTLS entropy gathering.
 *
 * macEntropy Stage A: cryptographic accumulator (SHA-256 pool).
 * macEntropy Stage C: cold-start seed-file persistence.
 *
 * Accumulator (Stage A): the pool is a running BearSSL SHA-256 context
 * that every source sample is folded into via br_sha256_update. Seed
 * extraction clones the pool, mixes a domain-separation tag, and
 * finalises 32 bytes (br_sha256_out is const, so the live pool is
 * untouched); the extracted seed is folded back so successive
 * extractions are independent and the output is never the raw running
 * hash. The 32-byte seed is injected into BearSSL's engine, which runs
 * its own HMAC-DRBG downstream -- macTLS supplies the seed material, it
 * does not implement the generator.
 *
 * Cold-start persistence (Stage C): the first handshake after a clean
 * boot is the thinnest the pool ever is -- no mouse movement or network
 * jitter has accumulated yet. To carry entropy across boots we read a
 * seed file from the Preferences folder at first use and fold it into
 * the pool, and we write a fresh (domain-separated) seed back so the
 * next boot starts warm. The file is NEVER trusted alone -- it is only
 * ever mixed in alongside live samples, and the persisted seed uses a
 * different extraction tag than the seed handed to TLS, so reading the
 * file does not reveal handshake randomness. (On a single-user OS 9
 * box, physical disk access is already game-over; this just avoids a
 * thin first handshake.)
 *
 * Source breadth (Stage B): OSTLS_StirTimer folds packet-arrival timing
 * into the pool at every OTRcv during a fetch. Validation (Stage E):
 * OSTLS_EntropySelfTest confirmed non-degenerate output and distinct
 * seed streams across launches on real G3 hardware (2026-05-29). This is
 * macEntropy v1.0. See MACENTROPY_SCOPE.md.
 */

#include "ostls_entropy.h"
#include "bearssl_hash.h"
#include "ostls_log.h"

/* Forward declarations for the exported API. CW8's flat-folder access
 * paths may not have macTLS/os9 explicitly listed, so the #include above
 * can silently miss and leave the in-file pool_ensure_init→OSTLS_LoadSeed
 * call at implicit-int, conflicting with the later void(void) definition.
 * Declaring locally guarantees the prototype is in scope before first use. */
void OSTLS_LoadSeed(void);
void OSTLS_SaveSeed(void);
void OSTLS_StirTimer(unsigned long hint);
void OSTLS_StirEntropy(const void *data, unsigned long len);
void OSTLS_CollectEntropy(void);
unsigned long OSTLS_EntropySampleCount(void);

#ifdef __MWERKS__
#include <Types.h>
#include <Events.h>
#include <Timer.h>
#include <Quickdraw.h>
#include <Files.h>
#include <Folders.h>
#include <Script.h>             /* smRoman */
#else
#include <stdint.h>
typedef uint32_t UInt32;
typedef struct { UInt32 hi; UInt32 lo; } UnsignedWide;
typedef struct { short v; short h; } Point;
/* Non-CW8 stubs so the file parses under the Retro68 syntax check; no
 * File Manager exists on Linux, so these no-op. Mirrors ostls_log.c. */
typedef long OSStatus;
typedef struct {
    short vRefNum;
    long parID;
    unsigned char name[64];
} FSSpec;
#ifndef noErr
#define noErr 0
#endif
#define kOnSystemDisk -32768
#define kPreferencesFolderType 'pref'
#define fsRdPerm 1
#define fsRdWrPerm 3
#define smRoman 0
#define eofErr -39
static OSStatus FindFolder(short v, long t, unsigned char create,
    short *out_v, long *out_d){(void)v;(void)t;(void)create;
    *out_v=0;*out_d=0;return noErr;}
static OSStatus FSMakeFSSpec(short v, long d, const unsigned char *n, FSSpec *s){
    (void)v;(void)d;(void)n;(void)s;return noErr;}
static OSStatus FSpDelete(const FSSpec *s){(void)s;return noErr;}
static OSStatus FSpCreate(const FSSpec *s, unsigned long c, unsigned long t,
    short script){(void)s;(void)c;(void)t;(void)script;return noErr;}
static OSStatus FSpOpenDF(const FSSpec *s, char perm, short *r){
    (void)s;(void)perm;*r=0;return noErr;}
static OSStatus FSRead(short r, long *count, void *buf){
    (void)r;(void)buf;*count=0;return eofErr;}
static OSStatus FSWrite(short r, long *count, const void *buf){
    (void)r;(void)count;(void)buf;return noErr;}
static OSStatus SetEOF(short r, long pos){(void)r;(void)pos;return noErr;}
static OSStatus FSClose(short r){(void)r;return noErr;}
static OSStatus FlushVol(const unsigned char *name, short v){(void)name;(void)v;return noErr;}
#endif

#include <string.h>

/* The entropy pool: a running SHA-256 context, never reset for the life
 * of the process. Every source sample is folded in via update(). */
static br_sha256_context g_pool;
static int    g_pool_init = 0;
static UInt32 g_sample_count = 0;
static Point  g_last_mouse = { 0, 0 };
static int    g_seed_saved = 0;     /* auto-save fires once per process */

/* Persisted seed file: 32 bytes in the Preferences folder. */
#define OSTLS_SEED_BYTES 32

static void
pool_ensure_init(void)
{
    if (!g_pool_init) {
        br_sha256_init(&g_pool);
        g_pool_init = 1;            /* set BEFORE LoadSeed: LoadSeed mixes
                                     * via pool_update which re-enters here,
                                     * and this guard stops the recursion. */
        OSTLS_LoadSeed();           /* cold-start: fold persisted seed in */
    }
}

/* Fold one source sample into the pool. */
static void
pool_update(const void *data, size_t len)
{
    pool_ensure_init();
    br_sha256_update(&g_pool, data, len);
    g_sample_count++;
}

/* Extract a 32-byte seed under a caller-supplied domain-separation tag.
 * Clones the pool (br_sha256_out is const), mixes the tag, finalises,
 * then folds the result back into the live pool so the next extraction
 * is independent. out must hold OSTLS_SEED_BYTES. */
static void
pool_extract(const unsigned char *tag, size_t tag_len, unsigned char *out)
{
    br_sha256_context tmp;
    pool_ensure_init();
    tmp = g_pool;
    br_sha256_update(&tmp, tag, tag_len);
    br_sha256_out(&tmp, out);
    br_sha256_update(&g_pool, out, OSTLS_SEED_BYTES);
}

/* Resolve the seed file's FSSpec in the Preferences folder. Returns 1 if
 * the path resolved (file may or may not exist yet), 0 on failure. */
static int
seed_spec(FSSpec *out_spec)
{
    long pref_dir;
    short pref_vol;
    OSStatus err;
    const unsigned char seed_name[] = "\pmacTLS Entropy Seed";

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType,
                     0 /* don't create */, &pref_vol, &pref_dir);
    if (err != noErr) return 0;

    err = FSMakeFSSpec(pref_vol, pref_dir, seed_name, out_spec);
    /* fnfErr (-43) still populates out_spec so a later FSpCreate works. */
    if (err != noErr && err != -43) return 0;
    return 1;
}

void
OSTLS_LoadSeed(void)
{
    FSSpec spec;
    short ref;
    OSStatus err;
    long count;
    unsigned char buf[OSTLS_SEED_BYTES];

    if (!seed_spec(&spec)) {
        OSTLS_LogLine("macEntropy: seed path unresolved");
        return;
    }

    ref = 0;
    err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (err != noErr) {
        /* No file yet -- first run on this machine. Not an error. */
        OSTLS_LogLine("macEntropy: no seed file (first run)");
        return;
    }

    count = (long)sizeof buf;
    err = FSRead(ref, &count, buf);
    FSClose(ref);

    /* eofErr just means the file was shorter than we asked for; count
     * holds however many bytes were actually read. */
    if ((err == noErr || err == eofErr) && count > 0) {
        pool_update(buf, (size_t)count);
        OSTLS_LogLinef("macEntropy: seed file loaded (%ld bytes)", count);
    } else {
        OSTLS_LogLine("macEntropy: seed file empty/unreadable");
    }
}

void
OSTLS_SaveSeed(void)
{
    FSSpec spec;
    short ref;
    OSStatus err;
    long count;
    unsigned char seed[OSTLS_SEED_BYTES];
    /* Persistence tag -- distinct from the TLS-inject tag, so the file
     * never holds the exact bytes handed to a handshake. */
    static const unsigned char seed_tag[8] =
        { 'm', 'a', 'c', 'S', 'e', 'e', 'd', 0 };

    if (!seed_spec(&spec)) return;

    pool_extract(seed_tag, sizeof seed_tag, seed);

    /* Recreate the file fresh each save so stale length never lingers. */
    (void)FSpDelete(&spec);
    err = FSpCreate(&spec, 'MPLS', 'rSed', smRoman);
    if (err != noErr) {
        OSTLS_LogLinef("macEntropy: seed create failed (%ld)", (long)err);
        return;
    }

    ref = 0;
    err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
    if (err != noErr) {
        OSTLS_LogLinef("macEntropy: seed open-w failed (%ld)", (long)err);
        return;
    }

    (void)SetEOF(ref, 0);
    count = (long)sizeof seed;
    err = FSWrite(ref, &count, seed);
    FSClose(ref);
    FlushVol(NULL, spec.vRefNum);

    if (err == noErr) {
        OSTLS_LogLinef("macEntropy: seed file saved (%ld bytes)", count);
    } else {
        OSTLS_LogLinef("macEntropy: seed write failed (%ld)", (long)err);
    }
}

void
OSTLS_CollectEntropy(void)
{
    UInt32 ticks;
    UnsignedWide usec;
    Point mouse;

#ifdef __MWERKS__
    ticks = (UInt32)TickCount();
    Microseconds(&usec);
    GetMouse(&mouse);
#else
    ticks = 0;
    usec.hi = 0; usec.lo = 0;
    mouse.h = 0; mouse.v = 0;
#endif

    pool_update(&ticks, sizeof ticks);
    pool_update(&usec, sizeof usec);

    /* Only fold the mouse in when it moved -- a static position carries
     * no new entropy and would over-weight a constant value. */
    if (mouse.h != g_last_mouse.h || mouse.v != g_last_mouse.v) {
        pool_update(&mouse, sizeof mouse);
        g_last_mouse = mouse;
    }
}

void
OSTLS_StirTimer(unsigned long hint)
{
    UInt32 ticks;
    UnsignedWide usec;

#ifdef __MWERKS__
    ticks = (UInt32)TickCount();
    Microseconds(&usec);
#else
    ticks = 0;
    usec.hi = 0; usec.lo = 0;
#endif

    /* The value of the clocks matters less than *when* this runs: called
     * at packet-arrival time, the low bits capture network jitter. */
    pool_update(&ticks, sizeof ticks);
    pool_update(&usec, sizeof usec);
    pool_update(&hint, sizeof hint);
}

unsigned long
OSTLS_EntropySampleCount(void)
{
    return (unsigned long)g_sample_count;
}

void
OSTLS_StirEntropy(const void *data, unsigned long len)
{
    if (data != NULL && len > 0) {
        pool_update(data, (size_t)len);
    }
    /* Always fold a fresh timestamp. The bytes carry entropy, but so does
     * *when* the host called us -- event-arrival cadence, key-press
     * latency -- independent of the bytes themselves. */
    OSTLS_StirTimer(len);
}

int
OSTLS_EntropySelfTest(unsigned long *out_fp)
{
    unsigned char seed[OSTLS_SEED_BYTES];
    unsigned char prev[OSTLS_SEED_BYTES];
    long hist[256];
    br_sha256_context fp_ctx;
    unsigned char fp_out[32];
    unsigned long fp;
    long ones;
    long dups;
    long nonempty;
    long maxbucket;
    long total_bits;
    long half;
    long tol;
    int i;
    int j;
    int b;
    int n_seeds;
    int pass;
    static const unsigned char test_tag[8] =
        { 'm', 'a', 'c', 'S', 't', 's', 't', 0 };

    n_seeds = 64;

    /* Freshen the pool so the batch reflects live state, then draw a run
     * of seeds and tally byte/bit statistics. */
    pool_ensure_init();
    OSTLS_CollectEntropy();
    OSTLS_StirTimer(0x53454C46UL);      /* 'SELF' */

    for (i = 0; i < 256; i++) hist[i] = 0;
    ones = 0;
    dups = 0;
    br_sha256_init(&fp_ctx);

    for (i = 0; i < n_seeds; i++) {
        pool_extract(test_tag, sizeof test_tag, seed);
        if (i > 0) {
            int same = 1;
            for (j = 0; j < OSTLS_SEED_BYTES; j++) {
                if (seed[j] != prev[j]) { same = 0; break; }
            }
            if (same) dups++;        /* two identical seeds in a row = bug */
        }
        for (j = 0; j < OSTLS_SEED_BYTES; j++) {
            unsigned int c = (unsigned int)seed[j];
            hist[c]++;
            for (b = 0; b < 8; b++) {
                if (c & (1U << b)) ones++;
            }
            prev[j] = seed[j];
        }
        br_sha256_update(&fp_ctx, seed, OSTLS_SEED_BYTES);
    }

    total_bits = (long)n_seeds * OSTLS_SEED_BYTES * 8L;

    nonempty = 0;
    maxbucket = 0;
    for (i = 0; i < 256; i++) {
        if (hist[i] > 0) nonempty++;
        if (hist[i] > maxbucket) maxbucket = hist[i];
    }

    /* 32-bit fingerprint of the whole batch. Compare across separate
     * launches: it MUST differ. That cross-run difference is the real
     * entropy proof -- the within-run checks below only guard against
     * degenerate output, since a SHA-256 chain looks random regardless
     * of how much true entropy fed it. */
    br_sha256_out(&fp_ctx, fp_out);
    fp = ((unsigned long)fp_out[0] << 24) |
         ((unsigned long)fp_out[1] << 16) |
         ((unsigned long)fp_out[2] << 8)  |
          (unsigned long)fp_out[3];
    if (out_fp) *out_fp = fp;

    half = total_bits / 2;
    tol  = total_bits / 20;             /* +/-5% */

    pass = 1;
    if (dups != 0)        pass = 0;     /* successive seeds must differ */
    if (nonempty < 250)   pass = 0;     /* output must spread (exp ~256) */
    if (maxbucket > 40)   pass = 0;     /* no value dominates (exp ~8) */
    if (ones < half - tol || ones > half + tol) pass = 0;  /* bit balance */

    OSTLS_LogLinef("macEntropy selftest: seeds=%d dups=%ld buckets=%ld/256 maxbkt=%ld",
                   n_seeds, dups, nonempty, maxbucket);
    OSTLS_LogLinef("macEntropy selftest: ones=%ld/%ld fp=%08lX %s",
                   ones, total_bits, fp, pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}

int
OSTLS_InjectEntropy(br_ssl_engine_context *eng)
{
    int local_var;
    unsigned long stackaddr;
    UnsignedWide usec;
    unsigned char seed[OSTLS_SEED_BYTES];
    /* Domain-separation tag for the TLS seed. */
    static const unsigned char inject_tag[8] =
        { 'm', 'a', 'c', 'E', 'x', 't', 'r', 0 };

    if (eng == NULL) return -1;
    pool_ensure_init();

    /* Fold fresh volatile state into the pool just before extraction so
     * even a caller that never ran CollectEntropy gets some live noise. */
    stackaddr = (unsigned long)(void *)&local_var;
    pool_update(&stackaddr, sizeof stackaddr);
#ifdef __MWERKS__
    Microseconds(&usec);
#else
    usec.hi = 0; usec.lo = 0;
#endif
    pool_update(&usec, sizeof usec);
    pool_update(&g_sample_count, sizeof g_sample_count);

    pool_extract(inject_tag, sizeof inject_tag, seed);
    OSTLS_LogLinef("macEntropy: inject samples=%lu", (unsigned long)g_sample_count);
    br_ssl_engine_inject_entropy(eng, seed, sizeof seed);

    /* Roll the persisted seed once per process so the next cold boot
     * starts warm. The host should also call OSTLS_SaveSeed() at a clean
     * shutdown to capture the full session's accumulated entropy. */
    if (!g_seed_saved) {
        OSTLS_SaveSeed();
        g_seed_saved = 1;
    }
    return 0;
}

/* fixes1069 — arbitrary-length extraction for callers outside the TLS
 * handshake, added for crypto.getRandomValues()/randomUUID().
 *
 * Those two shipped on a clock-seeded xorshift whose own comment said it was
 * "NOT cryptographic-grade" and pointed here for the real thing. A weak PRNG
 * behind crypto.* is not a missing feature, it is a WRONG ANSWER: a page
 * generating a session token or a v4 UUID gets predictable bytes and no
 * indication anything is off.
 *
 * Own domain-separation tag, so this stream is independent of the TLS seed
 * ('macExtr') and of the persisted seed. pool_extract already ratchets --
 * it folds each output back into the live pool -- so successive blocks are
 * independent without any extra stirring here. Fresh stack/clock noise is
 * still folded in once per call for callers that never ran CollectEntropy. */
void
OSTLS_RandomBytes(void *out, unsigned long len)
{
    static const unsigned char rand_tag[8] =
        { 'm', 'a', 'c', 'E', 'r', 'n', 'd', 0 };
    unsigned char blk[OSTLS_SEED_BYTES];
    unsigned char *p;
    unsigned long i, n;
    int local_var;
    unsigned long stackaddr;
    UnsignedWide usec;

    if (out == NULL || len == 0UL) return;
    p = (unsigned char *)out;
    pool_ensure_init();

    stackaddr = (unsigned long)(void *)&local_var;
    pool_update(&stackaddr, sizeof stackaddr);
#ifdef __MWERKS__
    Microseconds(&usec);
#else
    usec.hi = 0; usec.lo = 0;
#endif
    pool_update(&usec, sizeof usec);

    while (len > 0UL) {
        pool_extract(rand_tag, sizeof rand_tag, blk);
        n = (len < (unsigned long) OSTLS_SEED_BYTES) ?
                len : (unsigned long) OSTLS_SEED_BYTES;
        for (i = 0UL; i < n; i++) p[i] = blk[i];
        p += n;
        len -= n;
    }
    for (i = 0UL; i < (unsigned long) OSTLS_SEED_BYTES; i++) blk[i] = 0;
}
