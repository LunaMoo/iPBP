/* SPDX-License-Identifier: see LICENSE
 *
 * iPBP C API - convert between PSP UMD ISO images and PSN-style EBOOT.PBP (NPUMDIMG) files,
 * and between PlayStation (PSOne) disc images and PSOne EBOOT.PBPs (PSISOIMG / PSTITLEIMG).
 *
 * Typical use:
 *     ipbp_job* job = ipbp_job_create();
 *     ipbp_job_set_progress(job, my_progress, my_ctx);
 *     // from any other thread: ipbp_job_cancel(job);
 *     int rc = ipbp_iso_to_pbp(job, "game.iso", "EBOOT.PBP", NULL);
 *     if (rc != IPBP_OK) puts(ipbp_job_error(job));
 *     ipbp_job_destroy(job);
 */
#ifndef IPBP_H
#define IPBP_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(IPBP_SHARED)
#ifdef IPBP_BUILDING
#define IPBP_API __declspec(dllexport)
#else
#define IPBP_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(IPBP_SHARED)
#define IPBP_API __attribute__((visibility("default")))
#else
#define IPBP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    IPBP_OK = 0,
    IPBP_CANCELLED = 1,
    IPBP_IO_ERROR = 2,
    IPBP_INVALID_INPUT = 3,
    IPBP_UNSUPPORTED = 4,
    IPBP_VERIFY_FAILED = 5,
};

/* Values for ipbp_iso2pbp_options.compress. Any other non-zero value means IPBP_COMPRESS_LZRC.
 * MAX searches much harder for the smallest LZRC (optimal parsing, about 5-10x slower): still
 * plain LZRC that real PSPs decode, but no longer byte-identical to sign_np's output. */
#define IPBP_COMPRESS_NONE 0
#define IPBP_COMPRESS_LZRC 1
#define IPBP_COMPRESS_MAX 2

typedef struct ipbp_job ipbp_job;

/* Progress callback: bytes done / total. Called on the converting thread. */
typedef void (*ipbp_progress_fn)(void* user, uint64_t done, uint64_t total);
/* Log callback: one informational line (UTF-8, no trailing newline). */
typedef void (*ipbp_log_fn)(void* user, const char* line);

typedef struct ipbp_iso2pbp_options {
    int compress;                /* IPBP_COMPRESS_NONE / _LZRC (default) / _MAX, see below */
    const char* content_id;      /* NULL: "UP0000-<DISC_ID>_00-0000000000000000" */
    const uint8_t* version_key;  /* 16 bytes, NULL: fixed key derived from content id */
    const uint8_t* startdat_png; /* optional boot screen PNG (480x272) */
    size_t startdat_png_size;
    unsigned threads;            /* 0 = auto */
} ipbp_iso2pbp_options;

typedef struct ipbp_pbp2iso_options {
    int verify;     /* non-zero: verify block MACs (default 1) */
    int trim;       /* non-zero: cut the image at the ISO9660 volume size (default 0) */
    unsigned threads;
} ipbp_pbp2iso_options;

typedef struct ipbp_pbp_info {
    char content_id[49];
    char disc_id[16];
    char title[129];
    uint32_t np_flags;
    uint32_t block_basis;
    uint32_t blocks;
    uint64_t iso_size;
    uint8_t version_key[16];
    int compressed;
} ipbp_pbp_info;

IPBP_API ipbp_job* ipbp_job_create(void);
IPBP_API void ipbp_job_destroy(ipbp_job* job);
IPBP_API void ipbp_job_set_progress(ipbp_job* job, ipbp_progress_fn fn, void* user);
IPBP_API void ipbp_job_set_log(ipbp_job* job, ipbp_log_fn fn, void* user);
/* Thread-safe. The running conversion stops within one block and returns IPBP_CANCELLED;
 * the partially written output file is deleted. */
IPBP_API void ipbp_job_cancel(ipbp_job* job);
/* Last error message for this job ("" if none). Valid until the next call on the job. */
IPBP_API const char* ipbp_job_error(const ipbp_job* job);

IPBP_API void ipbp_iso2pbp_options_init(ipbp_iso2pbp_options* opt);
IPBP_API void ipbp_pbp2iso_options_init(ipbp_pbp2iso_options* opt);

/* Paths are UTF-8. Options may be NULL for defaults. */
IPBP_API int ipbp_iso_to_pbp(ipbp_job* job, const char* iso_path, const char* pbp_path,
                             const ipbp_iso2pbp_options* opt);
IPBP_API int ipbp_pbp_to_iso(ipbp_job* job, const char* pbp_path, const char* iso_path,
                             const ipbp_pbp2iso_options* opt);
/* The image on the caller's side instead of in a file, e.g. to repack a game through memory
 * without a temporary ISO on disk. Available since 1.2.0. The callbacks run on the converting
 * thread, and return 0 on success. */
typedef struct ipbp_reader {
    void* user;
    uint64_t size;
    /* Read exactly len bytes at offset. */
    int (*read_at)(void* user, uint64_t offset, void* buf, size_t len);
} ipbp_reader;

typedef struct ipbp_writer {
    void* user;
    /* Optional. Called once with the image size before anything is written; nonzero fails
     * with IPBP_IO_ERROR (e.g. when that much memory isn't available). */
    int (*begin)(void* user, uint64_t size);
    /* Write len bytes at offset (any order, within the size). */
    int (*write_at)(void* user, uint64_t offset, const void* buf, size_t len);
    /* Optional. Called once at the end with the final size, at most the one begin got. */
    int (*finish)(void* user, uint64_t final_size);
} ipbp_writer;

/* ipbp_iso_to_pbp reading the ISO through iso. */
IPBP_API int ipbp_iso_to_pbp_reader(ipbp_job* job, const ipbp_reader* iso, const char* pbp_path,
                                    const ipbp_iso2pbp_options* opt);
/* ipbp_pbp_to_iso writing the ISO through iso. On failure or cancellation what was written stays
 * with the caller. */
IPBP_API int ipbp_pbp_to_iso_writer(ipbp_job* job, const char* pbp_path, const ipbp_writer* iso,
                                    const ipbp_pbp2iso_options* opt);

/* ISO (through iso) -> EBOOT.PBP written through pbp, e.g. to build the PBP in memory and put it
 * into an archive. Available since 1.3.0. The PBP's size isn't known up front: pbp->begin is not
 * called, write_at may extend the output in any order, and finish gets the final size. On failure
 * or cancellation what was written stays with the caller. */
IPBP_API int ipbp_iso_to_pbp_io(ipbp_job* job, const ipbp_reader* iso, const ipbp_writer* pbp,
                                const ipbp_iso2pbp_options* opt);

IPBP_API int ipbp_read_pbp_info(ipbp_job* job, const char* pbp_path, ipbp_pbp_info* info);

/* ISO dump size check (file size vs ISO9660 volume size). Returns IPBP_OK when the image could
 * be inspected; the dump is good when *file_size == *volume_size. */
IPBP_API int ipbp_check_iso(ipbp_job* job, const char* iso_path, uint64_t* file_size, uint64_t* volume_size);


/* ---- PSOne EBOOT.PBP (POPS), since 1.4.0 -----------------------------------------------------
 *
 * PlayStation discs as EBOOT.PBPs as popstation makes them: raw 2352-byte sectors in deflated
 * 16-sector blocks, with the disc's TOC; up to 5 discs in one file. PS1 emulators (DuckStation,
 * Beetle PSX, PCSX-ReARMed) and Adrenaline read them; a real PSP also needs DATA.PSP and STARTDAT,
 * which come from a PSOne Classic EBOOT given as base_pbp. Needs zlib: a build without it
 * returns IPBP_UNSUPPORTED from these. */

#define IPBP_PBP_UNKNOWN 0
#define IPBP_PBP_PSP 1 /* NPUMDIMG: a PSP game */
#define IPBP_PBP_PSX 2 /* PSISOIMG / PSTITLEIMG: a PSOne game */
#define IPBP_PSX_MAX_DISCS 5
#define IPBP_PSX_MAX_TRACKS 99

/* A track, in frames (2352 bytes) from the start of the disc image. */
typedef struct ipbp_psx_track {
    int audio;       /* 0: data track, 1: audio */
    uint32_t start;  /* the track's first frame in the image (its INDEX 00 when the pregap is there) */
    uint32_t index1; /* INDEX 01 */
} ipbp_psx_track;

typedef struct ipbp_psx_disc {
    ipbp_reader image;            /* raw 2352-byte frames, every track back to back (one .bin) */
    uint32_t tracks;              /* 0: one data track */
    const ipbp_psx_track* track;
    const char* disc_id;          /* "SLUS00594"; NULL: from the disc's SYSTEM.CNF */
} ipbp_psx_disc;

typedef struct ipbp_psx_options {
    int level;                    /* deflate 0 (stored) .. 9, default 9 */
    const char* title;            /* NULL: the first disc's ID */
    const char* base_pbp;         /* PSOne Classic EBOOT.PBP for DATA.PSP / STARTDAT (real PSPs), or NULL */
    const uint8_t* icon0; size_t icon0_size;  /* optional PNGs: ICON0 80x80, PIC0, PIC1 480x272 */
    const uint8_t* pic0;  size_t pic0_size;
    const uint8_t* pic1;  size_t pic1_size;
    const uint8_t* boot_png; size_t boot_png_size;  /* boot screen in STARTDAT (with base_pbp) */
    unsigned threads;
} ipbp_psx_options;

typedef struct ipbp_psx_disc_info {
    char disc_id[16];
    uint32_t frames;              /* image length in 2352-byte frames */
    uint32_t tracks;
    ipbp_psx_track track[IPBP_PSX_MAX_TRACKS];
    int data_mode[IPBP_PSX_MAX_TRACKS];  /* 1 / 2 for data tracks, 0 for audio */
    uint32_t blocks;
    uint64_t stored_bytes;
} ipbp_psx_disc_info;

typedef struct ipbp_psx_info {
    char title[129];
    char disc_id[16];
    uint32_t discs;
    int has_data_psp;             /* a DATA.PSP is there: it can run on a real PSP */
    ipbp_psx_disc_info disc[IPBP_PSX_MAX_DISCS];
} ipbp_psx_info;

/* IPBP_PBP_PSP, IPBP_PBP_PSX or IPBP_PBP_UNKNOWN (not a PBP, or one this can't read). */
IPBP_API int ipbp_pbp_kind(const char* pbp_path);
IPBP_API void ipbp_psx_options_init(ipbp_psx_options* opt);
IPBP_API int ipbp_psx_to_pbp(ipbp_job* job, const ipbp_psx_disc* discs, unsigned count, const char* pbp_path,
                             const ipbp_psx_options* opt);
/* The PBP through a writer (its size isn't known up front, as with ipbp_iso_to_pbp_io). */
IPBP_API int ipbp_psx_to_pbp_io(ipbp_job* job, const ipbp_psx_disc* discs, unsigned count, const ipbp_writer* pbp,
                                const ipbp_psx_options* opt);
IPBP_API int ipbp_read_psx_info(ipbp_job* job, const char* pbp_path, ipbp_psx_info* info);
/* Disc `disc` (0-based) as raw frames through image (begin gets frames * 2352). */
IPBP_API int ipbp_pbp_to_psx(ipbp_job* job, const char* pbp_path, unsigned disc, const ipbp_writer* image);
/* The disc ID from SYSTEM.CNF into id[16] ("" when there is none). */
IPBP_API int ipbp_psx_disc_id(ipbp_job* job, const ipbp_reader* image, char id[16]);

IPBP_API const char* ipbp_version(void);

#ifdef __cplusplus
}
#endif

#endif /* IPBP_H */
