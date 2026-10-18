// SPDX-License-Identifier: see LICENSE
#include <cstring>
#include <memory>
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

}  // extern "C"

namespace {

class CallbackInput : public ipbp::InputStream {
public:
    explicit CallbackInput(const ipbp_reader& r) : r_(r) {}
    uint64_t size() const override { return r_.size; }
    bool readAt(uint64_t offset, void* buf, size_t len) override {
        if (offset > r_.size || len > r_.size - offset) return false;
        return r_.read_at(r_.user, offset, buf, len) == 0;
    }

private:
    ipbp_reader r_;
};

class CallbackOutput : public ipbp::OutputStream {
public:
    explicit CallbackOutput(const ipbp_writer& w) : w_(w) {}
    bool writeAt(uint64_t offset, const void* buf, size_t len) override {
        return w_.write_at(w_.user, offset, buf, len) == 0;
    }
    bool finish(uint64_t finalSize) override { return !w_.finish || w_.finish(w_.user, finalSize) == 0; }

private:
    ipbp_writer w_;
};

ipbp::IsoToPbpOptions isoToPbpOptions(const ipbp_iso2pbp_options* o) {
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
    return opt;
}

ipbp::PbpToIsoOptions pbpToIsoOptions(const ipbp_pbp2iso_options* o) {
    ipbp_pbp2iso_options def;
    if (!o) {
        ipbp_pbp2iso_options_init(&def);
        o = &def;
    }
    ipbp::PbpToIsoOptions opt;
    opt.verify = o->verify != 0;
    opt.trimToVolumeSize = o->trim != 0;
    opt.threads = o->threads;
    return opt;
}

}  // namespace

extern "C" {

int ipbp_iso_to_pbp(ipbp_job* job, const char* iso, const char* pbp, const ipbp_iso2pbp_options* o) {
    job->cancel.reset();
    return job->finish(ipbp::isoToPbp(iso, pbp, isoToPbpOptions(o), job->callbacks()));
}

int ipbp_iso_to_pbp_reader(ipbp_job* job, const ipbp_reader* iso, const char* pbp, const ipbp_iso2pbp_options* o) {
    job->cancel.reset();
    if (!iso || !iso->read_at || !pbp)
        return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "missing reader or output path"));
    CallbackInput in(*iso);
    return job->finish(ipbp::isoToPbp(in, pbp, isoToPbpOptions(o), job->callbacks()));
}

int ipbp_iso_to_pbp_io(ipbp_job* job, const ipbp_reader* iso, const ipbp_writer* pbp, const ipbp_iso2pbp_options* o) {
    job->cancel.reset();
    if (!iso || !iso->read_at || !pbp || !pbp->write_at)
        return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "missing reader or writer"));
    CallbackInput in(*iso);
    CallbackOutput out(*pbp);
    return job->finish(ipbp::isoToPbp(in, out, isoToPbpOptions(o), job->callbacks()));
}

int ipbp_pbp_to_iso(ipbp_job* job, const char* pbp, const char* iso, const ipbp_pbp2iso_options* o) {
    job->cancel.reset();
    return job->finish(ipbp::pbpToIso(pbp, iso, pbpToIsoOptions(o), job->callbacks()));
}

int ipbp_pbp_to_iso_writer(ipbp_job* job, const char* pbp, const ipbp_writer* iso, const ipbp_pbp2iso_options* o) {
    job->cancel.reset();
    if (!pbp || !iso || !iso->write_at)
        return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "missing input path or writer"));
    std::string err;
    std::unique_ptr<ipbp::InputStream> in = ipbp::openInputFile(pbp, &err);
    if (!in) return job->finish(ipbp::Result::fail(ipbp::Status::IoError, err));
    ipbp::PbpInfo info;
    ipbp::Result r = ipbp::readPbpInfo(*in, info);
    if (!r) return job->finish(r);
    if (iso->begin && iso->begin(iso->user, info.isoSize) != 0)
        return job->finish(ipbp::Result::fail(ipbp::Status::IoError, "the output refused the image size"));
    CallbackOutput out(*iso);
    return job->finish(ipbp::pbpToIso(*in, out, pbpToIsoOptions(o), job->callbacks()));
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

const char* ipbp_version(void) { return "1.3.0"; }

}  // extern "C"
