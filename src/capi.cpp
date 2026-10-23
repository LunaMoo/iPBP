// SPDX-License-Identifier: see LICENSE
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

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

}  // extern "C"

namespace {
std::vector<ipbp::PsxDisc> psxDiscs(const ipbp_psx_disc* d, unsigned n, std::vector<std::unique_ptr<CallbackInput>>& keep) {
    std::vector<ipbp::PsxDisc> out;
    for (unsigned i = 0; i < n; i++) {
        keep.emplace_back(new CallbackInput(d[i].image));
        ipbp::PsxDisc x;
        x.image = keep.back().get();
        for (uint32_t t = 0; t < d[i].tracks && d[i].track; t++)
            x.tracks.push_back(ipbp::PsxTrack{d[i].track[t].audio != 0, d[i].track[t].start, d[i].track[t].index1});
        if (d[i].disc_id) x.discId = d[i].disc_id;
        out.push_back(std::move(x));
    }
    return out;
}
ipbp::PsxToPbpOptions psxOptions(const ipbp_psx_options* o) {
    ipbp_psx_options def;
    if (!o) {
        ipbp_psx_options_init(&def);
        o = &def;
    }
    ipbp::PsxToPbpOptions opt;
    opt.level = o->level;
    if (o->title) opt.title = o->title;
    if (o->base_pbp) opt.basePbpPath = o->base_pbp;
    auto bytes = [](const uint8_t* p, size_t n) { return p && n ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>(); };
    opt.icon0 = bytes(o->icon0, o->icon0_size);
    opt.pic0 = bytes(o->pic0, o->pic0_size);
    opt.pic1 = bytes(o->pic1, o->pic1_size);
    opt.bootPng = bytes(o->boot_png, o->boot_png_size);
    opt.threads = o->threads;
    return opt;
}
bool validDiscs(const ipbp_psx_disc* d, unsigned n) {
    if (!d || n == 0) return false;
    for (unsigned i = 0; i < n; i++)
        if (!d[i].image.read_at) return false;
    return true;
}
}  // namespace

extern "C" {

int ipbp_pbp_kind(const char* pbp) {
    std::unique_ptr<ipbp::InputStream> in = ipbp::openInputFile(pbp ? pbp : "");
    if (!in) return IPBP_PBP_UNKNOWN;
    switch (ipbp::pbpKind(*in)) {
    case ipbp::PbpKind::Psp: return IPBP_PBP_PSP;
    case ipbp::PbpKind::Psx: return IPBP_PBP_PSX;
    default: return IPBP_PBP_UNKNOWN;
    }
}

void ipbp_psx_options_init(ipbp_psx_options* opt) {
    memset(opt, 0, sizeof(*opt));
    opt->level = 9;
}

int ipbp_psx_to_pbp(ipbp_job* job, const ipbp_psx_disc* discs, unsigned count, const char* pbp, const ipbp_psx_options* o) {
    job->cancel.reset();
    if (!validDiscs(discs, count) || !pbp)
        return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "missing discs or output path"));
    std::vector<std::unique_ptr<CallbackInput>> keep;
    return job->finish(ipbp::psxToPbp(psxDiscs(discs, count, keep), pbp, psxOptions(o), job->callbacks()));
}

int ipbp_psx_to_pbp_io(ipbp_job* job, const ipbp_psx_disc* discs, unsigned count, const ipbp_writer* pbp,
                       const ipbp_psx_options* o) {
    job->cancel.reset();
    if (!validDiscs(discs, count) || !pbp || !pbp->write_at)
        return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "missing discs or writer"));
    std::vector<std::unique_ptr<CallbackInput>> keep;
    CallbackOutput out(*pbp);
    return job->finish(ipbp::psxToPbp(psxDiscs(discs, count, keep), out, psxOptions(o), job->callbacks()));
}

int ipbp_read_psx_info(ipbp_job* job, const char* pbp, ipbp_psx_info* info) {
    memset(info, 0, sizeof(*info));
    std::string err;
    std::unique_ptr<ipbp::InputStream> in = ipbp::openInputFile(pbp ? pbp : "", &err);
    if (!in) return job->finish(ipbp::Result::fail(ipbp::Status::IoError, err));
    ipbp::PsxInfo i;
    ipbp::Result r = ipbp::readPsxInfo(*in, i);
    if (r) {
        strncpy(info->title, i.title.c_str(), sizeof(info->title) - 1);
        strncpy(info->disc_id, i.discId.c_str(), sizeof(info->disc_id) - 1);
        info->has_data_psp = i.hasDataPsp;
        info->discs = (uint32_t)std::min<size_t>(i.discs.size(), IPBP_PSX_MAX_DISCS);
        for (uint32_t d = 0; d < info->discs; d++) {
            const ipbp::PsxDiscInfo& s = i.discs[d];
            ipbp_psx_disc_info& o = info->disc[d];
            strncpy(o.disc_id, s.discId.c_str(), sizeof(o.disc_id) - 1);
            o.frames = s.frames;
            o.blocks = s.blocks;
            o.stored_bytes = s.storedBytes;
            o.tracks = (uint32_t)std::min<size_t>(s.tracks.size(), IPBP_PSX_MAX_TRACKS);
            for (uint32_t t = 0; t < o.tracks; t++) {
                o.track[t].audio = s.tracks[t].audio;
                o.track[t].start = s.tracks[t].start;
                o.track[t].index1 = s.tracks[t].index1;
                o.data_mode[t] = t < s.dataModes.size() ? s.dataModes[t] : 0;
            }
        }
    }
    return job->finish(r);
}

int ipbp_pbp_to_psx(ipbp_job* job, const char* pbp, unsigned disc, const ipbp_writer* image) {
    job->cancel.reset();
    if (!pbp || !image || !image->write_at)
        return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "missing input path or writer"));
    std::string err;
    std::unique_ptr<ipbp::InputStream> in = ipbp::openInputFile(pbp, &err);
    if (!in) return job->finish(ipbp::Result::fail(ipbp::Status::IoError, err));
    ipbp::PsxInfo info;
    ipbp::Result r = ipbp::readPsxInfo(*in, info);
    if (!r) return job->finish(r);
    if (disc >= info.discs.size())
        return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "the EBOOT has no disc " + std::to_string(disc + 1)));
    if (image->begin && image->begin(image->user, (uint64_t)info.discs[disc].frames * 2352) != 0)
        return job->finish(ipbp::Result::fail(ipbp::Status::IoError, "the output refused the image size"));
    CallbackOutput out(*image);
    return job->finish(ipbp::pbpToPsx(*in, disc, out, job->callbacks()));
}

int ipbp_psx_disc_id(ipbp_job* job, const ipbp_reader* image, char id[16]) {
    memset(id, 0, 16);
    if (!image || !image->read_at) return job->finish(ipbp::Result::fail(ipbp::Status::InvalidInput, "missing reader"));
    CallbackInput in(*image);
    std::string s = ipbp::psxDiscId(in);
    strncpy(id, s.c_str(), 15);
    return job->finish(ipbp::Result::success());
}

const char* ipbp_version(void) { return "1.4.0"; }

}  // extern "C"
