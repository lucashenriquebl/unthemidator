#ifndef MUFLEX_RUNTIME_COVERAGE_HPP
#define MUFLEX_RUNTIME_COVERAGE_HPP

// V22: bounded instruction-address coverage of the Unicorn startup phase.
// Purely observational: never modifies guest memory or control flow.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace muflex {

class runtime_coverage final {
public:
    static constexpr std::uint64_t k_window_instructions = 100000;
    static constexpr std::size_t k_max_global_pcs = 262144;
    static constexpr std::size_t k_max_window_pcs = 100000;
    static constexpr std::size_t k_top_pcs = 512;

    explicit runtime_coverage(std::filesystem::path out) : out_(std::move(out)) {
        all_pcs_.reserve(32768);
        window_pcs_.reserve(8192);
    }

    void observe(const std::uint64_t pc) {
        ++total_;
        if (window_.instruction_count == 0) {
            window_.first_index = total_;
            window_.first_pc = pc;
        }
        window_.last_index = total_;
        window_.last_pc = pc;
        ++window_.instruction_count;

        if (window_pcs_.contains(pc)) {
            ++window_.repeated_pc_hits;
        } else if (window_pcs_.size() < k_max_window_pcs) {
            window_pcs_.insert(pc);
            ++window_.unique_pcs;
        } else {
            ++window_.untracked_pc_hits;
        }

        const auto it = all_pcs_.find(pc);
        if (it != all_pcs_.end()) {
            ++it->second;
            ++window_.previously_seen_pc_hits;
        } else if (all_pcs_.size() < k_max_global_pcs) {
            all_pcs_.emplace(pc, 1);
            ++window_.new_global_pcs;
        } else {
            ++untracked_global_pc_hits_;
            ++window_.untracked_global_pc_hits;
        }

        if (window_.instruction_count >= k_window_instructions) {
            flush_window();
        }
    }

    void finish() {
        if (window_.instruction_count) flush_window();

        std::ofstream windows(out_ / "runtime_coverage_windows.csv", std::ios::trunc);
        if (windows) {
            windows << "window,first_instruction,last_instruction,instructions,first_pc,last_pc,"
                       "unique_pcs,repeat_hits_in_window,new_global_pcs,previously_seen_global_hits,"
                       "untracked_window_hits,untracked_global_hits\n";
            for (std::size_t i = 0; i < history_.size(); ++i) {
                const auto& w = history_[i];
                windows << (i + 1) << ',' << w.first_index << ',' << w.last_index
                        << ',' << w.instruction_count
                        << ",0x" << std::hex << w.first_pc
                        << ",0x" << w.last_pc << std::dec
                        << ',' << w.unique_pcs << ',' << w.repeated_pc_hits
                        << ',' << w.new_global_pcs << ',' << w.previously_seen_pc_hits
                        << ',' << w.untracked_pc_hits << ',' << w.untracked_global_pc_hits << '\n';
            }
        }

        std::vector<std::pair<std::uint64_t, std::uint64_t>> top;
        top.reserve(all_pcs_.size());
        for (const auto& [pc, count] : all_pcs_) top.emplace_back(pc, count);
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });

        std::ofstream pcs(out_ / "runtime_coverage_top_pcs.csv", std::ios::trunc);
        if (pcs) {
            pcs << "rank,pc,executions,percent_of_observed\n";
            const std::size_t bound = (top.size() < k_top_pcs ? top.size() : k_top_pcs);
            for (std::size_t i = 0; i < bound; ++i) {
                const auto [pc, count] = top[i];
                pcs << (i+1) << ",0x" << std::hex << pc << std::dec << ',' << count
                    << ',' << std::fixed << std::setprecision(6)
                    << (total_ ? 100.0 * static_cast<double>(count) / static_cast<double>(total_) : 0.0) << '\n';
            }
        }

        std::ofstream summary(out_ / "runtime_coverage_summary.txt", std::ios::trunc);
        if (summary) {
            summary << "mode=GUEST_PC_COVERAGE_UNICORN\n"
                    << "instructions_observed=" << total_ << '\n'
                    << "windows=" << history_.size() << '\n'
                    << "window_size=" << k_window_instructions << '\n'
                    << "distinct_pc_keys=" << all_pcs_.size() << '\n'
                    << "max_distinct_pc_keys=" << k_max_global_pcs << '\n'
                    << "untracked_global_pc_hits=" << untracked_global_pc_hits_ << '\n'
                    << "window_key_limit=" << k_max_window_pcs << '\n'
                    << "warning=PC frequency/coverage alone does not establish whether code is original or VM.\n"
                    << "warning=Coverage only includes instructions executed during the bounded emulation.\n";
        }
    }

    [[nodiscard]] std::size_t distinct_pcs() const { return all_pcs_.size(); }
    [[nodiscard]] std::size_t windows() const { return history_.size(); }
    [[nodiscard]] std::uint64_t untracked_global_hits() const { return untracked_global_pc_hits_; }

private:
    struct window_stats {
        std::uint64_t first_index = 0;
        std::uint64_t last_index = 0;
        std::uint64_t first_pc = 0;
        std::uint64_t last_pc = 0;
        std::uint64_t instruction_count = 0;
        std::uint64_t unique_pcs = 0;
        std::uint64_t repeated_pc_hits = 0;
        std::uint64_t new_global_pcs = 0;
        std::uint64_t previously_seen_pc_hits = 0;
        std::uint64_t untracked_pc_hits = 0;
        std::uint64_t untracked_global_pc_hits = 0;
    };

    void flush_window() {
        history_.push_back(window_);
        window_ = window_stats{};
        window_pcs_.clear();
    }

    std::filesystem::path out_;
    std::uint64_t total_ = 0;
    std::uint64_t untracked_global_pc_hits_ = 0;
    window_stats window_{};
    std::vector<window_stats> history_;
    std::unordered_map<std::uint64_t, std::uint64_t> all_pcs_;
    std::unordered_set<std::uint64_t> window_pcs_;
};

} // namespace muflex

#endif // MUFLEX_RUNTIME_COVERAGE_HPP
