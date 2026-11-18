#ifndef MUFLEX_RUNTIME_HOTSPOT_HPP
#define MUFLEX_RUNTIME_HOTSPOT_HPP

// V20: bounded, observational analysis of the V19 GameServer hot page.
// No change to guest behavior; no reconstructed PE is run or written.

#include <unicorn/unicorn.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <tuple>
#include <vector>

#include "../emulator/c_emulator_ctx.hpp"
#include "../pe/c_pe_image.hpp"

namespace muflex {

class runtime_hotspot final {
public:
    static constexpr std::uint64_t k_hotspot_rva = 0xA392000;
    static constexpr std::uint64_t k_page_size = 0x1000;
    static constexpr std::size_t k_max_keys = 8192;

    runtime_hotspot(const std::shared_ptr<c_pe_image>& pe,
                    const std::shared_ptr<c_emulator_ctx>& context,
                    const std::uint64_t image_base,
                    const std::filesystem::path& directory)
        : context_(context), image_base_(image_base), directory_(directory) {
        // These RVAs are specific to the supplied MUDevs S21 GameServer x64 PE.
        if (!pe->is_x64() || pe->get_entry_rva() != 0xAF07058) return;
        const auto begin = k_hotspot_rva;
        for (const auto& section : pe->get_sections()) {
            if (section.name == ".themida" &&
                begin >= section.virtual_address &&
                begin - section.virtual_address <= section.virtual_size &&
                k_page_size <= section.virtual_size - (begin - section.virtual_address)) {
                enabled_ = true;
                return;
            }
        }
    }

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] std::uint64_t write_count() const { return write_count_; }
    [[nodiscard]] std::uint64_t distinct_keys() const { return stats_.size(); }
    [[nodiscard]] std::uint64_t dropped_keys() const { return dropped_keys_; }
    [[nodiscard]] std::uint64_t changed_bytes() const { return changed_bytes_; }

    void begin() {
        if (!enabled_) return;
        before_status_ = uc_mem_read(context_->get_uc(), image_base_ + k_hotspot_rva,
                                     before_.data(), before_.size());
        if (before_status_ != UC_ERR_OK) {
            std::cerr << "[MUFLEX_V20] hotspot before read failed: "
                      << uc_strerror(before_status_) << '\n';
        }
    }

    void observe_write(const std::uint64_t instr_index, const std::uint64_t pc,
                       const std::uint64_t dest, const int size, const std::uint64_t value) {
        if (!enabled_ || size <= 0) return;
        const auto start = image_base_ + k_hotspot_rva;
        if (dest >= start + k_page_size ||
            (dest < start && static_cast<std::uint64_t>(size) <= start - dest)) return;

        ++write_count_;
        // Include the write width in the aggregation key: different widths
        // may target the same VA and must not be silently conflated.
        using key_t = std::tuple<std::uint64_t, std::uint64_t, int>;
        const key_t key{pc, dest, size};
        auto it = stats_.find(key);
        if (it == stats_.end()) {
            if (stats_.size() >= k_max_keys) {
                ++dropped_keys_;
                return;
            }
            it = stats_.emplace(key, entry{}).first;
            it->second.first_value = value;
            it->second.first_instruction = instr_index;
        }
        auto& st = it->second;
        ++st.count;
        st.last_value = value;
        st.last_instruction = instr_index;
    }

    void finish() {
        if (!enabled_) return;
        after_status_ = uc_mem_read(context_->get_uc(), image_base_ + k_hotspot_rva,
                                    after_.data(), after_.size());
        if (after_status_ != UC_ERR_OK) {
            std::cerr << "[MUFLEX_V20] hotspot after read failed: "
                      << uc_strerror(after_status_) << '\n';
        }

        save_page("before", before_, before_status_);
        save_page("after", after_, after_status_);
        std::ofstream csv(directory_ / "runtime_hotspot_writers.csv", std::ios::trunc);
        if (csv) {
            csv << "rank,writer_pc,writer_rva,dest_va,dest_rva,size,count,"
                   "first_instruction,last_instruction,first_value_hex,last_value_hex\n";
            struct ranked {
                std::tuple<std::uint64_t, std::uint64_t, int> key;
                entry stats;
            };
            std::vector<ranked> ordered;
            ordered.reserve(stats_.size());
            for (const auto& [key, value] : stats_) ordered.push_back({key,value});
            std::sort(ordered.begin(), ordered.end(), [](const ranked& a, const ranked& b) {
                return a.stats.count > b.stats.count;
            });
            std::uint64_t rank = 0;
            for (const auto& row : ordered) {
                const auto [pc, dest, size] = row.key;
                const auto& st = row.stats;
                csv << ++rank << ",0x" << std::hex << pc
                    << ",0x" << (pc >= image_base_ ? pc - image_base_ : 0)
                    << ",0x" << dest
                    << ",0x" << (dest >= image_base_ ? dest - image_base_ : 0)
                    << std::dec << ',' << size << ',' << st.count
                    << ',' << st.first_instruction << ',' << st.last_instruction
                    << ",0x" << std::hex << st.first_value
                    << ",0x" << st.last_value << std::dec << '\n';
            }
        }
        if (before_status_ == UC_ERR_OK && after_status_ == UC_ERR_OK) {
            for (std::size_t i = 0; i < before_.size(); ++i) {
                if (before_[i] != after_[i]) ++changed_bytes_;
            }
        }
        std::ofstream meta(directory_ / "runtime_hotspot_summary.txt", std::ios::trunc);
        if (meta) {
            meta << "page_rva=0x" << std::hex << k_hotspot_rva << std::dec << '\n'
                 << "page_size=" << k_page_size << '\n'
                 << "before_uc_status=" << static_cast<int>(before_status_) << '\n'
                 << "after_uc_status=" << static_cast<int>(after_status_) << '\n'
                 << "guest_write_events=" << write_count_ << '\n'
                 << "distinct_writer_destination_width_keys=" << stats_.size() << '\n'
                 << "unaggregated_events_due_to_key_limit=" << dropped_keys_ << '\n'
                 << "changed_bytes=" << changed_bytes_ << '\n'
                 << "changed_bytes_valid=" <<
                    (before_status_ == UC_ERR_OK && after_status_ == UC_ERR_OK ? "true" : "false") << '\n'
                 << "warning=Guest-write callback and host memory snapshots are different observation mechanisms.\n";
        }
    }

private:
    struct entry {
        std::uint64_t count = 0;
        std::uint64_t first_instruction = 0;
        std::uint64_t last_instruction = 0;
        std::uint64_t first_value = 0;
        std::uint64_t last_value = 0;
    };

    void save_page(const char* stage, const std::array<std::uint8_t,k_page_size>& bytes,
                   const uc_err status) {
        // Never present a zero-filled buffer from a failed uc_mem_read as valid data.
        if (status != UC_ERR_OK) return;
        const auto name = std::string("runtime_hotspot_") + stage + ".bin";
        std::ofstream out(directory_ / name, std::ios::binary | std::ios::trunc);
        if (out) out.write(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
    }

    std::shared_ptr<c_emulator_ctx> context_;
    std::uint64_t image_base_ = 0;
    std::filesystem::path directory_;
    std::array<std::uint8_t,k_page_size> before_{};
    std::array<std::uint8_t,k_page_size> after_{};
    std::map<std::tuple<std::uint64_t,std::uint64_t,int>,entry> stats_;
    std::uint64_t write_count_ = 0;
    std::uint64_t dropped_keys_ = 0;
    std::uint64_t changed_bytes_ = 0;
    uc_err before_status_ = UC_ERR_HANDLE;
    uc_err after_status_ = UC_ERR_HANDLE;
    bool enabled_ = false;
};

} // namespace muflex

#endif // MUFLEX_RUNTIME_HOTSPOT_HPP
