// Phase 5 tests: parameter mapping/formatting, dirty dispatch, serialisation,
// settings file round-trip, mixer parameter set and its binding to the matrix.
#include "TestHarness.hpp"

#include "hybrid/dsp/StemMatrix.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/params/Settings.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace hc::test {

namespace {
bool close(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }
}

void testParams() {
    std::printf("[parameters]\n");
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    const std::size_t expected = dsp::StemMatrix::kStems * 9 + dsp::StemMatrix::kBuses * 2;
    CHECK(tree.size() == expected);
    std::printf("  mixer parameters: %zu (13 stems x 9 + 4 buses x 2)\n", tree.size());

    params::Parameter* main1 = tree.find("stem01.main");
    params::Parameter* pan1  = tree.find("stem01.pan");
    params::Parameter* mute1 = tree.find("stem01.mute");
    params::Parameter* aux3  = tree.find("stem13.aux3");
    params::Parameter* busG  = tree.find("aux2.gain");
    CHECK(main1 && pan1 && mute1 && aux3 && busG);
    if (!main1 || !pan1 || !mute1 || !aux3 || !busG) return;

    // Fader taper: unity at 3/4 travel, bottom = -inf.
    CHECK(close(main1->toNormalized(0.0f), 0.75f));
    CHECK(close(main1->toPlain(0.75f), 0.0f, 1e-3f));
    CHECK(close(main1->toPlain(1.0f), 6.0f));
    main1->setNormalized(0.0f);
    CHECK(main1->isOff() && main1->effectiveDb() <= -144.0f && main1->format() == "-inf");
    main1->setPlain(-6.0f);
    CHECK(main1->format() == "-6.0 dB");
    CHECK(close(main1->toPlain(main1->toNormalized(-17.5f)), -17.5f, 1e-3f));   // round trip
    main1->setPlain(1000.0f);  CHECK(main1->plain() == 6.0f);                  // clamped
    main1->setPlain(NAN);      CHECK(std::isfinite(main1->plain()));
    CHECK(aux3->plain() == -60.0f && aux3->isOff());                          // aux sends default off

    pan1->setPlain(-0.35f); CHECK(pan1->format() == "L35");
    pan1->setPlain(0.0f);   CHECK(pan1->format() == "C");
    pan1->setPlain(1.0f);   CHECK(pan1->format() == "R100");
    mute1->setPlain(0.7f);  CHECK(mute1->plain() == 1.0f && mute1->format() == "ON");

    // Dispatch: listeners run once per changed parameter, on the dispatching thread.
    tree.dispatchChanges();   // flush everything dirty from construction
    int calls = 0;
    tree.bind(*busG, [&calls](const params::Parameter&) { ++calls; });
    const auto v0 = tree.version();
    busG->setPlain(-3.0f);
    busG->setPlain(-3.0f);   // same value: no new change
    CHECK(tree.version() > v0);
    CHECK(tree.dispatchChanges() == 1 && calls == 1);
    CHECK(tree.dispatchChanges() == 0 && calls == 1);

    // Writes from another thread are picked up by the message-thread dispatch.
    std::thread t([&] { for (int i = 0; i < 1000; ++i) busG->setPlain(-10.0f + static_cast<float>(i % 10)); });
    t.join();
    CHECK(tree.dispatchChanges() == 1 && calls == 2);

    // Serialise / deserialise.
    const std::string text = tree.serialize();
    params::ParameterTree copy;
    params::addMixerParameters(copy);
    CHECK(copy.deserialize(text) == tree.size());
    bool same = true;
    for (std::size_t i = 0; i < tree.size(); ++i)
        if (!close(tree.at(i).plain(), copy.at(i).plain(), 1e-4f)) same = false;
    CHECK(same);
    CHECK(copy.deserialize("garbage\nunknown.id=3\nstem02.trim=abc\nstem02.trim = 5.5\n") == 1);
    CHECK(close(copy.find("stem02.trim")->plain(), 5.5f));
}

void testSettings() {
    std::printf("[settings]\n");
    params::Settings s;
    s.set("app", "theme", "pearl");
    s.set("app", "device", "ASIO: Focusrite USB ASIO");
    s.setSectionText("params", "stem01.trim=3\nmain.gain=-6\n");
    const auto file = std::filesystem::temp_directory_path() / "hc_settings_test" / "settings.ini";
    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
    CHECK(s.save(file));
    CHECK(s.save(file));   // overwrite existing (atomic replace path)
    params::Settings r;
    CHECK(r.load(file));
    CHECK(r.get("app", "theme") == "pearl");
    CHECK(r.get("app", "device") == "ASIO: Focusrite USB ASIO");
    CHECK(r.get("app", "missing", "fallback") == "fallback");
    CHECK(r.sectionText("params") == "main.gain=-6\nstem01.trim=3\n");
    CHECK(!std::filesystem::exists(file.string() + ".tmp"));
    std::filesystem::remove_all(file.parent_path(), ec);
}

void testMixerBinding() {
    std::printf("[mixer parameter binding]\n");
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    auto matrix = std::make_unique<dsp::StemMatrix>();
    constexpr std::uint32_t F = 64;
    matrix->prepare(F);
    params::MixerBindings bindings(tree);
    params::bindMixerParameters(tree, bindings, *matrix);
    CHECK(tree.dispatchChanges() == tree.size());   // defaults pushed once

    std::vector<float> one(F, 1.0f);
    std::array<const float*, dsp::StemMatrix::kStems> L{}, R{};
    L[0] = R[0] = one.data();
    auto run = [&] { matrix->process(L.data(), R.data(), F); matrix->process(L.data(), R.data(), F); };

    run();
    CHECK(close(matrix->busChannel(0, 0)[F - 1], 1.0f));

    tree.find("stem01.main")->setPlain(-6.0f);
    tree.dispatchChanges(); run();
    CHECK(close(matrix->busChannel(0, 0)[F - 1], 0.5011872f));

    tree.find("stem01.aux2")->setPlain(0.0f);
    tree.dispatchChanges(); run();
    CHECK(close(matrix->busChannel(2, 1)[F - 1], 1.0f));

    tree.find("main.gain")->setNormalized(0.0f);   // bottom of travel = off
    tree.dispatchChanges(); run();
    CHECK(matrix->busChannel(0, 0)[F - 1] == 0.0f);

    tree.find("main.gain")->setPlain(0.0f);
    tree.find("stem01.solo")->setPlain(1.0f);
    tree.find("stem01.invert")->setPlain(1.0f);
    tree.dispatchChanges(); run();
    CHECK(close(matrix->busChannel(0, 0)[F - 1], -0.5011872f));

    // Audio-thread path used by the plug-in: applyAll must not allocate.
    std::uint64_t allocs = 0;
    {
        AllocScope scope;
        for (int k = 0; k < 1000; ++k) bindings.applyAll(tree, *matrix);
        allocs = scope.count();
    }
    CHECK(allocs == 0);
    std::printf("  1,000 x applyAll(%zu params): %llu heap allocations\n", tree.size(),
                static_cast<unsigned long long>(allocs));
}

} // namespace hc::test
