// SPDX-License-Identifier: see LICENSE
#include <cstring>
#include <string>

#include "ipbp/ipbp.h"
#include "ipbp/ipbp.hpp"

struct ipbp_job {
    ipbp::CancelToken cancel;
    ipbp_progress_fn progress = nullptr;
    void* progressUser = nullptr;
    ipbp_log_fn log = nullptr;
    void* logUser = nullptr;
    std::string error;

    ipbp::Callbacks callbacks() {
        ipbp::Callbacks cb;
        cb.cancel = cancel;
        if (progress) cb.progress = [this](uint64_t d, uint64_t t) { progress(progressUser, d, t); };
        if (log) cb.log = [this](const std::string& s) { log(logUser, s.c_str()); };
        return cb;
    }
    int finish(const ipbp::Result& r) {
        error = r.ok() ? std::string() : r.message;
        return static_cast<int>(r.status);
    }
};

extern "C" {

ipbp_job* ipbp_job_create(void) { return new ipbp_job(); }
void ipbp_job_destroy(ipbp_job* job) { delete job; }
void ipbp_job_set_progress(ipbp_job* job, ipbp_progress_fn fn, void* user) {
    job->progress = fn;
    job->progressUser = user;
}
void ipbp_job_set_log(ipbp_job* job, ipbp_log_fn fn, void* user) {
    job->log = fn;
    job->logUser = user;
}
void ipbp_job_cancel(ipbp_job* job) { job->cancel.cancel(); }
const char* ipbp_job_error(const ipbp_job* job) { return job->error.c_str(); }

void ipbp_iso2pbp_options_init(ipbp_iso2pbp_options* opt) {
    memset(opt, 0, sizeof(*opt));
    opt->compress = 1;
}
void ipbp_pbp2iso_options_init(ipbp_pbp2iso_options* opt) {
    memset(opt, 0, sizeof(*opt));
    opt->verify = 1;
    opt->trim = 0;
}

int ipbp_iso_to_pbp(ipbp_job* job, const char* iso, const char* pbp, const ipbp_iso2pbp_options* o) {
    ipbp_iso2pbp_options def;
    if (!o) {
        ipbp_iso2pbp_options_init(&def);
        o = &def;
    }
    ipbp::IsoToPbpOptions opt;
    opt.compress = o->compress != IPBP_COMPRESS_NONE;
    opt.maxCompression = o->compress == IPBP_COMPRESS_MAX;
    if (o->content_id) opt.contentId = o->content_id;
    if (o->version_key) {
        std::array<uint8_t, 16> k;
        memcpy(k.data(), o->version_key, 16);
        opt.versionKey = k;
    }
    if (o->startdat_png && o->startdat_png_size)
        opt.startdatPng.assign(o->startdat_png, o->startdat_png + o->startdat_png_size);
    opt.threads = o->threads;
    job->cancel.reset();
    return job->finish(ipbp::isoToPbp(iso, pbp, opt, job->callbacks()));
}

int ipbp_pbp_to_iso(ipbp_job* job, const char* pbp, const char* iso, const ipbp_pbp2iso_options* o) {
    ipbp_pbp2iso_options def;
    if (!o) {
        ipbp_pbp2iso_options_init(&def);
        o = &def;
    }
    ipbp::PbpToIsoOptions opt;
    opt.verify = o->verify != 0;
    opt.trimToVolumeSize = o->trim != 0;
    opt.threads = o->threads;
    job->cancel.reset();
    return job->finish(ipbp::pbpToIso(pbp, iso, opt, job->callbacks()));
}

int ipbp_read_pbp_info(ipbp_job* job, const char* pbp, ipbp_pbp_info* info) {
    ipbp::PbpInfo i;
    ipbp::Result r = ipbp::readPbpInfo(pbp, i);
    memset(info, 0, sizeof(*info));
    if (r) {
        strncpy(info->content_id, i.contentId.c_str(), sizeof(info->content_id) - 1);
        strncpy(info->disc_id, i.discId.c_str(), sizeof(info->disc_id) - 1);
        strncpy(info->title, i.title.c_str(), sizeof(info->title) - 1);
        info->np_flags = i.npFlags;
        info->block_basis = i.blockBasis;
        info->blocks = i.blocks;
        info->iso_size = i.isoSize;
        memcpy(info->version_key, i.versionKey.data(), 16);
        info->compressed = i.compressed;
    }
    return job->finish(r);
}

int ipbp_check_iso(ipbp_job* job, const char* iso, uint64_t* fileSize, uint64_t* volumeSize) {
    ipbp::IsoDumpCheck c;
    ipbp::Result r = ipbp::checkIsoDump(iso, c);
    if (fileSize) *fileSize = c.fileSize;
    if (volumeSize) *volumeSize = c.volumeSize;
    return job->finish(r);
}

const char* ipbp_version(void) { return "1.1.0"; }

}  // extern "C"
