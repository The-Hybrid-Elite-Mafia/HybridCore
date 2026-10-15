#include "hybrid/media/StemLoader.hpp"
#include "hybrid/media/AudioFile.hpp"
#include "hybrid/media/Resampler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hc::media {

namespace {
std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::wstring lowerName(const std::filesystem::path& p) {
    std::wstring s = p.filename().wstring();
    for (wchar_t& c : s) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}
} // namespace

bool naturalLess(const std::filesystem::path& pa, const std::filesystem::path& pb) {
    const std::wstring a = lowerName(pa), b = lowerName(pb);
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = a[i] >= L'0' && a[i] <= L'9', db = b[j] >= L'0' && b[j] <= L'9';
        if (da && db) {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == L'0') ++i2;
            while (j2 < b.size() && b[j2] == L'0') ++j2;
            std::size_t ie = i2, je = j2;
            while (ie < a.size() && a[ie] >= L'0' && a[ie] <= L'9') ++ie;
            while (je < b.size() && b[je] >= L'0' && b[je] <= L'9') ++je;
            if (ie - i2 != je - j2) return ie - i2 < je - j2;
            const int c = a.compare(i2, ie - i2, b, j2, je - j2);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return (a.size() - i) < (b.size() - j) || (a.size() - i == b.size() - j && pa < pb);
}

std::vector<std::filesystem::path> collectAudioFiles(const std::vector<std::filesystem::path>& inputs) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& p : inputs) {
        if (std::filesystem::is_directory(p, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(p, ec))
                if (e.is_regular_file(ec) && isAudioFileName(e.path())) out.push_back(e.path());
        } else if (isAudioFileName(p)) {
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end(), naturalLess);
    return out;
}

std::string describeStem(const engine::StemAudio& s) {
    // Codec, then the source rate when it was converted (else the bit depth), then the length.
    const std::string codec = s.format.substr(0, s.format.find(' '));
    std::string detail;
    if (s.sourceRate != hc::kSampleRate) {
        char r[24];
        const double k = s.sourceRate / 1000.0;
        std::snprintf(r, sizeof r, std::fmod(k, 1.0) == 0.0 ? "%.0fk" : "%.1fk", k);
        detail = r;
    } else if (const auto bit = s.format.find("-bit"); bit != std::string::npos) {
        const auto sp = s.format.rfind(' ', bit);
        detail = s.format.substr(sp + 1, bit - sp - 1) + (s.format.compare(bit, 10, "-bit float") == 0 ? "f" : "b");
    }
    const auto secs = s.frames / hc::kSampleRate;
    char len[32];
    std::snprintf(len, sizeof len, "%llu:%02llu", static_cast<unsigned long long>(secs / 60), static_cast<unsigned long long>(secs % 60));
    return codec + (detail.empty() ? "" : " " + detail) + " " + len;
}

StemLoader::StemLoader(unsigned workers, std::uint32_t outRate) : outRate_(outRate) {
    for (auto& p : progress_) p.store(-1.0f);
    workers = std::clamp(workers, 1u, 16u);
    for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { worker(); });
}

StemLoader::~StemLoader() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        queue_.clear();
    }
    for (auto& l : live_) l.store(0);   // running jobs see a dead generation and stop
    wake_.notify_all();
    for (auto& t : workers_) t.join();
}

void StemLoader::load(std::uint32_t slot, const std::filesystem::path& file) {
    if (slot >= kSlots) return;
    {
        std::lock_guard lock(mutex_);
        const std::uint64_t gen = ++generation_[slot];
        live_[slot].store(gen);
        std::erase_if(queue_, [slot](const Job& j) { return j.slot == slot; });
        queue_.push_back({slot, gen, file});
        progress_[slot].store(0.0f);
    }
    wake_.notify_one();
}

void StemLoader::cancel(std::uint32_t slot) {
    if (slot >= kSlots) return;
    std::lock_guard lock(mutex_);
    ++generation_[slot];
    live_[slot].store(0);
    std::erase_if(queue_, [slot](const Job& j) { return j.slot == slot; });
    std::erase_if(done_, [slot](const Result& r) { return r.slot == slot; });
    progress_[slot].store(-1.0f);
}

void StemLoader::cancelAll() {
    for (std::uint32_t s = 0; s < kSlots; ++s) cancel(s);
}

bool StemLoader::poll(Result& out) {
    std::lock_guard lock(mutex_);
    if (done_.empty()) return false;
    out = std::move(done_.front());
    done_.pop_front();
    return true;
}

float StemLoader::progress(std::uint32_t slot) const noexcept {
    return slot < kSlots ? progress_[slot].load(std::memory_order_relaxed) : -1.0f;
}

bool StemLoader::busy() const noexcept {
    for (const auto& p : progress_)
        if (p.load(std::memory_order_relaxed) >= 0.0f) return true;
    return false;
}

void StemLoader::worker() noexcept {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        run(job);
    }
}

void StemLoader::run(const Job& job) noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    auto alive = [&] { return live_[job.slot].load(std::memory_order_relaxed) == job.generation; };
    Result res;
    res.slot = job.slot;
    res.file = job.file;
    try {
        auto decoded = decodeFile(job.file, [&](float f) {
            progress_[job.slot].store(0.6f * f, std::memory_order_relaxed);
            return alive();
        });
        if (!decoded) {
            res.error = decoded.error();
        } else {
            DecodedAudio& d = *decoded;
            const std::uint32_t srcRate = d.sampleRate;
            if (!resampleTo(d, outRate_, [&](float f) {
                    progress_[job.slot].store(0.6f + 0.38f * f, std::memory_order_relaxed);
                    return alive();
                })) {
                res.error = "cancelled";
            } else {
                auto a = std::make_unique<engine::StemAudio>();
                a->frames = d.frames();
                a->left.allocate(static_cast<std::size_t>(a->frames));
                a->right.allocate(static_cast<std::size_t>(a->frames));
                std::memcpy(a->left.data(), d.left.data(), sizeof(float) * d.left.size());
                std::memcpy(a->right.data(), d.right.data(), sizeof(float) * d.right.size());
                a->name           = utf8(job.file.stem());
                a->path           = utf8(job.file);
                a->format         = d.format + (d.note.empty() ? "" : " (" + d.note + ")");
                a->sourceRate     = srcRate;
                a->sourceChannels = d.channels;
                res.audio = std::move(a);
            }
        }
    } catch (const std::exception& e) {   // e.g. out of memory on a huge file
        res.audio.reset();
        res.error = std::string("load failed: ") + e.what();
    }
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard lock(mutex_);
    if (live_[job.slot].load() != job.generation) return;   // replaced or cancelled: drop it
    progress_[job.slot].store(-1.0f);
    done_.push_back(std::move(res));
}

} // namespace hc::media
