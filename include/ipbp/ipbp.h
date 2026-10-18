/* SPDX-License-Identifier: see LICENSE
 *
 * iPBP C API - convert between PSP UMD ISO images and PSN-style EBOOT.PBP (NPUMDIMG) files.
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

IPBP_API const char* ipbp_version(void);

#ifdef __cplusplus
}
#endif

#endif /* IPBP_H */
