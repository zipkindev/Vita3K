// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <codec/state.h>
#include <cstring>
#include <vector>

int main() {
    // Synthetic mono, four frames per superframe. No game audio is included.
    const uint8_t config[]{ 0xfe, 0x70, 0x7f, 0xf0 };
    uint32_t config_word;
    std::memcpy(&config_word, config, sizeof(config_word));
    Atrac9DecoderState decoder(config_word);
    assert(decoder.get(DecoderQuery::AT9_FRAMES_IN_SUPERFRAME) == 4);
    const uint32_t bytes = decoder.get(DecoderQuery::AT9_SUPERFRAME_SIZE);
    std::vector<uint8_t> input(bytes);
    for (unsigned frame = 0; frame < 4; ++frame) {
        input[frame * 13] = frame ? 0x80 : 0; // Not-first flag after frame zero.
        input[frame * 13 + 4] = 4; // CLC scale factors, zero spectra.
    }
    assert(!decoder.send(input.data(), 0)); // Former NGS placeholder is invalid.
    const auto samples = decoder.get(DecoderQuery::AT9_SAMPLE_PER_FRAME);
    std::vector<uint8_t> pcm(samples * decoder.get(DecoderQuery::CHANNELS) * sizeof(int16_t));
    for (unsigned superframe = 0; superframe < 2; ++superframe) {
        uint32_t left = bytes;
        const uint8_t *position = input.data();
        for (unsigned frame = 0; frame < 4; ++frame) {
            assert(decoder.send(position, left));
            const uint32_t consumed = decoder.get_es_size();
            assert(consumed > 0 && consumed <= left);
            if (frame < 3)
                assert(consumed == 13);
            DecoderSize size;
            std::fill(pcm.begin(), pcm.end(), 0x5a);
            assert(decoder.receive(pcm.data(), &size));
            assert(size.samples == samples);
            assert(std::all_of(pcm.begin(), pcm.end(), [](uint8_t v) { return v == 0; }));
            position += consumed;
            left -= consumed;
        }
        // The final frame accounts for padding and resets the next superframe.
        assert(left == 0 && position == input.data() + bytes);
    }
}
