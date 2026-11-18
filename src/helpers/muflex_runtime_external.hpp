#ifndef MUFLEX_RUNTIME_EXTERNAL_HPP
#define MUFLEX_RUNTIME_EXTERNAL_HPP

// V21: summarize guest writes outside the original mapped PE image.
// Observational only. No native execution, no PE rebuilding, bounded memory.

#include <unicorn/unicorn.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "../emulator/c_emulator_ctx.hpp"

namespace muflex {

class runtime_external final {
public:
    static constexpr std::size_t k_max_pages = 8192;
    static constexpr std::size_t k_max_samples = 2048;

    runtime_external(const std::shared_ptr<c_emulator_ctx>& ctx,
                     const std::uint64_t image_base,
                     const std::uint64_t image_size,
                     const std::filesystem::path& directory)
        : ctx_(ctx), image_base_(image_base), image_size_(image_size),
          directory_(directory) {}

    [[nodiscard]] bool is_external(const std::uint64_t address) const {
        return address < image_base_ || address - image_base_ >= image_size_;
    }

    void observe(const std::uint64_t instruction_index, const std::uint64_t pc,
                 const std::uint64_t address, const int size,
                 const std::uint64_t value) {
        if (size <= 0 || !is_external(address)) return;
        ++total_writes_;
        const auto page = address & ~std::uint64_t{0xFFF};
        auto it = pages_.find(page);
        bool first_page_write = false;
        if (it == pages_.end()) {
            if (pages_.size() >= k_max_pages) {
                ++untracked_page_writes_;
            } else {
                it = pages_.try_emplace(page, page_stats{}).first;
                first_page_write = true;
            }
        }
        if (it != pages_.end()) {
            auto& st = it->second;
            if (!st.count) {
                st.first_pc = pc;
                st.first_instruction = instruction_index;
                // Heuristic only: page near current RSP, not a verified stack mapping.
                std::uint64_t rsp = 0;
                if (uc_reg_read(ctx_->get_uc(), UC_X86_REG_RSP, &rsp) == UC_ERR_OK) {
                    st.rsp_at_first = rsp;
                }
            }
            ++st.count;
            st.last_pc = pc;
            st.last_instruction = instruction_index;
        }

        if (samples_.size() < k_max_samples &&
            (total_writes_ <= 128 || first_page_write || (total_writes_ % 16384 == 0))) {
            samples_.push_back({total_writes_, instruction_index, pc, address, size, value});
        }
    }

    void finish() {
        // Access permissions at the END of the run. These may differ from the
        // permissions that applied when a particular guest write occurred.
        uc_mem_region* regions = nullptr;
        std::uint32_t region_count = 0;
        const uc_err status = uc_mem_regions(ctx_->get_uc(), &regions, &region_count);

        struct ranked_page {
            std::uint64_t va;
            page_stats stats;
            bool mapped;
            std::uint32_t perms;
        };
        std::vector<ranked_page> ranking;
        ranking.reserve(pages_.size());
        if (status == UC_ERR_OK) {
            for (const auto& [page, st] : pages_) {
                bool mapped = false;
                std::uint32_t perms = 0;
                for (std::uint32_t i = 0; i < region_count; ++i) {
                    if (regions[i].begin <= page && page <= regions[i].end) {
                        mapped = true;
                        perms = regions[i].perms;
                        break;
                    }
                }
                ranking.push_back({page, st, mapped, perms});
            }
        } else {
            for (const auto& [page, st] : pages_)
                ranking.push_back({page, st, false, 0});
        }
        if (regions) uc_free(regions);

        std::sort(ranking.begin(), ranking.end(), [](const ranked_page& a, const ranked_page& b) {
            return a.stats.count > b.stats.count;
        });
        std::uint64_t events_on_executable_end = 0;
        std::uint64_t executable_pages_end = 0;
        for (const auto& r : ranking) {
            if (r.mapped && (r.perms & UC_PROT_EXEC)) {
                events_on_executable_end += r.stats.count;
                ++executable_pages_end;
            }
        }

        std::ofstream pages_csv(directory_ / "runtime_external_pages.csv", std::ios::trunc);
        if (pages_csv) {
            pages_csv << "rank,page_va,write_events,first_writer_pc,last_writer_pc,"
                         "first_instruction,last_instruction,mapped_at_end,permissions_at_end,"
                         "rsp_at_first,near_rsp_first_1mb\n";
            std::uint64_t rank = 0;
            for (const auto& row : ranking) {
                const auto& st = row.stats;
                const std::uint64_t distance = st.rsp_at_first > row.va ?
                    st.rsp_at_first - row.va : row.va - st.rsp_at_first;
                pages_csv << ++rank << ",0x" << std::hex << row.va << std::dec
                          << ',' << st.count
                          << ",0x" << std::hex << st.first_pc
                          << ",0x" << st.last_pc << std::dec
                          << ',' << st.first_instruction << ',' << st.last_instruction
                          << ',' << (row.mapped ? "true" : "false") << ','
                          << (row.mapped ? permissions(row.perms) : "unmapped")
                          << ",0x" << std::hex << st.rsp_at_first << std::dec
                          << ',' << (st.rsp_at_first && distance <= 0x100000 ? "true" : "false")
                          << '\n';
            }
        }
        std::ofstream samples_csv(directory_ / "runtime_external_samples.csv", std::ios::trunc);
        if (samples_csv) {
            samples_csv << "write_index,instruction_index,writer_pc,dest_va,size,value_low64_hex\n";
            for (const auto& row : samples_) {
                samples_csv << row.write_index << ',' << row.instruction_index
                            << ",0x" << std::hex << row.writer_pc
                            << ",0x" << row.dest_va << std::dec << ',' << row.size
                            << ",0x" << std::hex << row.value << std::dec << '\n';
            }
        }
        std::ofstream summary(directory_ / "runtime_external_summary.txt", std::ios::trunc);
        if (summary) {
            summary << "mode=GUEST_WRITE_OUTSIDE_ORIGINAL_PE_IMAGE\n"
                    << "image_base=0x" << std::hex << image_base_
                    << "\nimage_size=0x" << image_size_ << std::dec
                    << "\nexternal_guest_write_events=" << total_writes_
                    << "\ntracked_external_pages=" << pages_.size()
                    << "\nuntracked_events_due_to_page_limit=" << untracked_page_writes_
                    << "\ntrace_samples=" << samples_.size()
                    << "\nuc_mem_regions_status=" << static_cast<int>(status)
                    << "\nexecutable_pages_at_end=" << executable_pages_end
                    << "\nwrites_into_pages_executable_at_end=" << events_on_executable_end
                    << "\nwarning=Executable-at-end does NOT imply executable-at-write-time; near-RSP is heuristic.\n"
                    << "warning=Only guest writes seen by Unicorn; does not include host uc_mem_write or untracked writes across image boundary.\n";
        }
        std::cout << "[MUFLEX_V21] external writes=" << total_writes_
                  << " pages=" << pages_.size()
                  << " pages_executable_at_end=" << executable_pages_end << '\n';
    }

    [[nodiscard]] std::uint64_t total_writes() const { return total_writes_; }
    [[nodiscard]] std::size_t tracked_pages() const { return pages_.size(); }

private:
    struct page_stats {
        std::uint64_t count = 0;
        std::uint64_t first_pc = 0;
        std::uint64_t last_pc = 0;
        std::uint64_t first_instruction = 0;
        std::uint64_t last_instruction = 0;
        std::uint64_t rsp_at_first = 0;
    };
    struct sample {
        std::uint64_t write_index;
        std::uint64_t instruction_index;
        std::uint64_t writer_pc;
        std::uint64_t dest_va;
        int size;
        std::uint64_t value;
    };

    static std::string permissions(const std::uint32_t perms) {
        std::string s;
        s.push_back((perms & UC_PROT_READ) ? 'r' : '-');
        s.push_back((perms & UC_PROT_WRITE) ? 'w' : '-');
        s.push_back((perms & UC_PROT_EXEC) ? 'x' : '-');
        return s;
    }

    std::shared_ptr<c_emulator_ctx> ctx_;
    std::uint64_t image_base_ = 0;
    std::uint64_t image_size_ = 0;
    std::filesystem::path directory_;
    std::map<std::uint64_t, page_stats> pages_;
    std::vector<sample> samples_;
    std::uint64_t total_writes_ = 0;
    std::uint64_t untracked_page_writes_ = 0;
};

} // namespace muflex

#endif // MUFLEX_RUNTIME_EXTERNAL_HPP
