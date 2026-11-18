#ifndef MUFLEX_RUNTIME_FLOW_HPP
#define MUFLEX_RUNTIME_FLOW_HPP

// V23: bounded observation of non-sequential PC transitions in the Unicorn
// startup trace. A non-sequential transition is not necessarily a CALL/JMP:
// emulator callbacks, resumes and skipped instructions also qualify.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace muflex {

class runtime_flow final {
public:
    static constexpr std::uint64_t k_window_size = 100000;
    static constexpr std::size_t k_global_limit = 131072;
    static constexpr std::size_t k_window_limit = 32768;
    static constexpr std::size_t k_top_global = 256;
    static constexpr std::size_t k_top_per_window = 24;

    explicit runtime_flow(const std::filesystem::path& directory)
        : directory_(directory) {
        global_.reserve(16384);
        window_edges_.reserve(8192);
    }

    void observe(const std::uint64_t pc, const std::uint32_t size) {
        ++total_;
        if (window_.instructions == 0) {
            window_.first_instruction = total_;
            window_.first_pc = pc;
        }
        window_.last_instruction = total_;
        window_.last_pc = pc;
        ++window_.instructions;

        // Check the exact expected fall-through position of the previous
        // observed instruction. Do not label a discontinuity as a proven jump.
        if (have_previous_) {
            const bool no_overflow = previous_pc_ <=
                std::numeric_limits<std::uint64_t>::max() - previous_size_;
            const bool sequential = no_overflow && pc == previous_pc_ + previous_size_;
            if (sequential) {
                ++window_.sequential_transitions;
                ++sequential_transitions_;
            } else {
                ++window_.nonsequential_transitions;
                ++nonsequential_transitions_;
                const edge_key key{previous_pc_, pc};
                const auto w = window_edges_.find(key);
                if (w != window_edges_.end()) {
                    ++w->second;
                    ++window_.repeated_edges_in_window;
                } else if (window_edges_.size() < k_window_limit) {
                    window_edges_.emplace(key, 1);
                    ++window_.distinct_edges;
                } else {
                    ++window_.untracked_window_edges;
                    ++untracked_window_events_;
                }

                const auto g = global_.find(key);
                if (g != global_.end()) {
                    ++g->second;
                    ++window_.previously_seen_global_edges;
                } else if (global_.size() < k_global_limit) {
                    global_.emplace(key, 1);
                    ++window_.new_global_edges;
                } else {
                    ++window_.untracked_global_edges;
                    ++untracked_global_events_;
                }
            }
        }
        previous_pc_ = pc;
        previous_size_ = size;
        have_previous_ = true;

        if (window_.instructions == k_window_size) flush_window();
    }

    void finish() {
        if (window_.instructions) flush_window();

        std::ofstream windows(directory_ / "runtime_flow_windows.csv", std::ios::trunc);
        if (windows) {
            windows << "window,first_instruction,last_instruction,instructions,"
                       "first_pc,last_pc,sequential_transitions,nonsequential_transitions,"
                       "distinct_edges,repeated_edges_in_window,new_global_edges,"
                       "previously_seen_global_edges,untracked_window_edges,"
                       "untracked_global_edges\n";
            for (std::size_t i = 0; i < history_.size(); ++i) {
                const auto& w = history_[i];
                windows << (i+1) << ',' << w.first_instruction
                        << ',' << w.last_instruction << ',' << w.instructions
                        << ",0x" << std::hex << w.first_pc << ",0x" << w.last_pc << std::dec
                        << ',' << w.sequential_transitions << ',' << w.nonsequential_transitions
                        << ',' << w.distinct_edges << ',' << w.repeated_edges_in_window
                        << ',' << w.new_global_edges << ',' << w.previously_seen_global_edges
                        << ',' << w.untracked_window_edges << ',' << w.untracked_global_edges << '\n';
            }
        }
        std::ofstream by_window(directory_ / "runtime_flow_window_top_edges.csv", std::ios::trunc);
        if (by_window) {
            by_window << "window,rank,source_pc,destination_pc,transitions\n";
            for (std::size_t i = 0; i < hot_by_window_.size(); ++i) {
                std::uint64_t rank = 0;
                for (const auto& e : hot_by_window_[i]) {
                    by_window << (i+1) << ',' << ++rank << ",0x" << std::hex
                              << e.first.from << ",0x" << e.first.to << std::dec
                              << ',' << e.second << '\n';
                }
            }
        }
        std::ofstream global(directory_ / "runtime_flow_global_top_edges.csv", std::ios::trunc);
        if (global) {
            global << "rank,source_pc,destination_pc,transitions\n";
            const auto top = get_top(global_, k_top_global);
            std::uint64_t rank = 0;
            for (const auto& e : top) {
                global << ++rank << ",0x" << std::hex << e.first.from
                       << ",0x" << e.first.to << std::dec << ',' << e.second << '\n';
            }
        }
        std::ofstream summary(directory_ / "runtime_flow_summary.txt", std::ios::trunc);
        if (summary) {
            summary << "mode=GUEST_PC_NON_SEQUENTIAL_FLOW_UNICORN\n"
                    << "instructions_observed=" << total_ << '\n'
                    << "windows=" << history_.size() << '\n'
                    << "sequential_transitions=" << sequential_transitions_ << '\n'
                    << "nonsequential_transitions=" << nonsequential_transitions_ << '\n'
                    << "distinct_global_flow_edges=" << global_.size() << '\n'
                    << "global_edge_limit=" << k_global_limit << '\n'
                    << "window_edge_limit=" << k_window_limit << '\n'
                    << "untracked_global_edge_events=" << untracked_global_events_ << '\n'
                    << "untracked_window_edge_events=" << untracked_window_events_ << '\n'
                    << "warning=Flow discontinuity is not proof of branch/call or OEP.\n"
                    << "warning=Only Unicorn code-hook instructions were observed.\n";
        }
    }

    [[nodiscard]] std::size_t edges() const { return global_.size(); }
    [[nodiscard]] std::uint64_t nonlinear() const { return nonsequential_transitions_; }
    [[nodiscard]] std::uint64_t untracked() const { return untracked_global_events_; }

private:
    struct edge_key {
        std::uint64_t from = 0;
        std::uint64_t to = 0;
        bool operator==(const edge_key&) const = default;
    };
    struct edge_hash {
        std::size_t operator()(const edge_key& e) const noexcept {
            const auto a = std::hash<std::uint64_t>{}(e.from);
            const auto b = std::hash<std::uint64_t>{}(e.to);
            return a ^ (b + std::size_t{0x9e3779b9} + (a << 6) + (a >> 2));
        }
    };
    using edge_map = std::unordered_map<edge_key, std::uint64_t, edge_hash>;
    using edge_pair = std::pair<edge_key, std::uint64_t>;

    struct window_stats {
        std::uint64_t first_instruction = 0;
        std::uint64_t last_instruction = 0;
        std::uint64_t first_pc = 0;
        std::uint64_t last_pc = 0;
        std::uint64_t instructions = 0;
        std::uint64_t sequential_transitions = 0;
        std::uint64_t nonsequential_transitions = 0;
        std::uint64_t distinct_edges = 0;
        std::uint64_t repeated_edges_in_window = 0;
        std::uint64_t new_global_edges = 0;
        std::uint64_t previously_seen_global_edges = 0;
        std::uint64_t untracked_window_edges = 0;
        std::uint64_t untracked_global_edges = 0;
    };

    static std::vector<edge_pair> get_top(const edge_map& map,
                                          const std::size_t limit) {
        std::vector<edge_pair> result;
        result.reserve(map.size());
        for (const auto& e : map) result.emplace_back(e);
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            if (a.first.from != b.first.from) return a.first.from < b.first.from;
            return a.first.to < b.first.to;
        });
        if (result.size() > limit) result.resize(limit);
        return result;
    }

    void flush_window() {
        history_.push_back(window_);
        hot_by_window_.push_back(get_top(window_edges_, k_top_per_window));
        window_ = {};
        window_edges_.clear();
    }

    std::filesystem::path directory_;
    std::uint64_t total_ = 0;
    std::uint64_t previous_pc_ = 0;
    std::uint32_t previous_size_ = 0;
    std::uint64_t sequential_transitions_ = 0;
    std::uint64_t nonsequential_transitions_ = 0;
    std::uint64_t untracked_global_events_ = 0;
    std::uint64_t untracked_window_events_ = 0;
    bool have_previous_ = false;
    window_stats window_{};
    edge_map global_;
    edge_map window_edges_;
    std::vector<window_stats> history_;
    std::vector<std::vector<edge_pair>> hot_by_window_;
};

}  // namespace muflex

#endif // MUFLEX_RUNTIME_FLOW_HPP
