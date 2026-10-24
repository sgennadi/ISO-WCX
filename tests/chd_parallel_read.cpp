#include "iso_wcx/chd_stream.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
struct Probe {
    bool audio = false;
    std::uint32_t track = 0;
    std::uint64_t offset = 0;
    std::size_t length = 0;
    std::vector<std::uint8_t> expected;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

// Several Total Commander operations may share one opened Disc/ChdStream.
// Sample the same stream serially, then race audio and filesystem reads
// against independently saved bytes. A shared libchdr hunk cache must never
// let a different caller replace its contents before a copy completes.
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: chd_parallel_read_tests image.chd");
        iso_wcx::ChdStream chd{std::filesystem::path(argv[1])};
        require(!chd.audio_tracks().empty(), "fixture must contain CDDA");

        std::vector<Probe> probes;
        for (const auto& track : chd.audio_tracks()) {
            const auto pcm = track.frames * std::uint64_t(2352);
            require(pcm >= 2352, "test audio track is empty");
            for (std::uint64_t i = 0; i < 24; ++i) {
                const auto start = ((pcm * i / 24u) / 2u) * 2u;
                auto length = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        pcm - start, 2048u + ((i * 977u) % 12288u)));
                length &= ~std::size_t(1);
                require(length >= 2, "invalid audio test probe");
                Probe probe;
                probe.audio = true;
                probe.track = track.number;
                probe.offset = start;
                probe.length = length;
                probe.expected = chd.read_audio(track.number, start, length);
                require(probe.expected.size() == length,
                        "serial CDDA read returned incorrect length");
                probes.push_back(std::move(probe));
            }
        }

        if (chd.size() != 0) {
            for (std::uint64_t i = 0; i < 24; ++i) {
                const auto start = chd.size() * i / 24u;
                const auto length = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        chd.size() - start, 2048u + ((i * 811u) % 8192u)));
                require(length != 0, "invalid data-track test probe");
                Probe probe;
                probe.offset = start;
                probe.length = length;
                probe.expected = chd.read(start, length);
                require(probe.expected.size() == length,
                        "serial data-track read returned incorrect length");
                probes.push_back(std::move(probe));
            }
        }

        require(!probes.empty(), "no CHD test probes");
        constexpr unsigned thread_count = 8;
        constexpr unsigned rounds = 16;
        std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false};
        std::atomic<bool> failed{false};
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (unsigned worker = 0; worker < thread_count; ++worker) {
            threads.emplace_back([&, worker] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (unsigned round = 0; round < rounds &&
                                          !failed.load(std::memory_order_acquire);
                     ++round) {
                    for (std::size_t i = 0; i < probes.size(); ++i) {
                        if (failed.load(std::memory_order_acquire)) return;
                        const auto& sample =
                            probes[(i + std::size_t(worker) * 7u +
                                    std::size_t(round) * 13u) % probes.size()];
                        try {
                            auto actual = sample.audio ?
                                chd.read_audio(sample.track, sample.offset,
                                               sample.length) :
                                chd.read(sample.offset, sample.length);
                            if (actual != sample.expected) {
                                failed.store(true, std::memory_order_release);
                                return;
                            }
                        } catch (...) {
                            failed.store(true, std::memory_order_release);
                            return;
                        }
                    }
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != thread_count)
            std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
        require(!failed.load(std::memory_order_acquire),
                "concurrent CHD audio/data reads differed from serial baseline");
        std::cout << "PASS: eight-thread CHD audio/data shared-cache regression ("
                  << probes.size() << " distinct probes)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: CHD concurrent-read test: " << e.what() << '\n';
        return 1;
    }
}
