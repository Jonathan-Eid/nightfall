#include "meteor_projection_margin.h"
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    const std::array<float, 22> original = {
        -.032f, 0, 0, 0, 0, 0, 1, -.8f, .7f, .75f, -.65f,
         .032f, 0, 0, 0, 0, 0, 1, -.7f, .8f, .75f, -.65f
    };
    for (const char *setting : {"1", "1.15", "1.4", ""}) {
        auto eyes = original;
        float scale = 0;
        assert(meteor_projection_margin(setting, eyes.data(), &scale));
        for (unsigned eye = 0; eye < 2; ++eye) {
            for (unsigned i = 0; i < 7; ++i) assert(eyes[eye * 11 + i] == original[eye * 11 + i]);
            for (unsigned i = 7; i < 11; ++i) {
                const auto at = eye * 11 + i;
                assert(std::abs(std::tan(eyes[at]) / std::tan(original[at]) - scale) < .00001f);
                if (scale == 1) assert(eyes[at] == original[at]);
            }
        }
    }
    for (const char *setting : {"0", ".99", "1.41", "nan", "inf", "garbage", "1.15junk"}) {
        auto eyes = original;
        assert(!meteor_projection_margin(setting, eyes.data()));
        assert(eyes == original);
    }
    auto invalid = original;
    invalid[21] = -2.f;
    const auto saved = invalid;
    assert(!meteor_projection_margin("1.15", invalid.data()));
    assert(invalid == saved);
    std::puts("PASS: asymmetric FOV margin, unchanged eye poses, compatibility and invalid input rejection");
}
