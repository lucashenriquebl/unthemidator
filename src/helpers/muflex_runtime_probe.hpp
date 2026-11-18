#ifndef MUFLEX_RUNTIME_PROBE_HPP
#define MUFLEX_RUNTIME_PROBE_HPP

// Bounded, opt-in observation of Themida runtime *in Unicorn* after Stage0.
// No native launch and no claim that observed addresses are the game's OEP.

#include <unicorn/unicorn.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

#include "../emulator/c_emulator_ctx.hpp"
#include "../memory/c_memory_manager.hpp"
#include "../pe/c_pe_image.hpp"
#include "muflex_runtime_hotspot.hpp"

namespace muflex {

class runtime_probe final {
public:
    runtime_probe(const std::shared_ptr<c_pe_image>& image,
                  const std::shared_ptr<c_emulator_ctx>& context,
                  const std::shared_ptr<c_memory_manager>& memory,
                  const std::filesystem::path& directory,
                  const std::uint64_t budget)
        : image_(image), context_(context),
          image_base_(memory->get_image_base()), image_size_(memory->get_image_size()), output_(directory),
          hotspot_(image, context, memory->get_image_base(), directory), budget_(budget) {}

    runtime_probe(const runtime_probe&) = delete;
    runtime_probe& operator=(const runtime_probe&) = delete;
    ~runtime_probe() { detach(); }

    bool start() {
        std::error_code error;
        std::filesystem::create_directories(output_, error);
        if (error) {
            std::cerr << "[MUFLEX_V18] cannot create probe folder: " << error.message() << '\n';
            return false;
        }
        trace_.open(output_ / "runtime_trace.csv", std::ios::trunc);
        if (!trace_) {
            std::cerr << "[MUFLEX_V18] cannot open runtime_trace.csv\n";
            return false;
        }
        trace_ << "instruction_index,pc,rva,region,size,opcode16_hex,event\n";
        image_writes_.open(output_ / "runtime_image_writes.csv", std::ios::trunc);
        if (!image_writes_) {
            std::cerr << "[MUFLEX_V19] cannot open runtime_image_writes.csv\n";
            return false;
        }
        image_writes_ << "write_index,instruction_index,pc,pc_rva,dest_va,dest_rva,dest_region,page_rva,size,value_low64_hex\n";
        hotspot_.begin();

        const auto result = uc_hook_add(context_->get_uc(), &hook_, UC_HOOK_CODE,
                           reinterpret_cast<void*>(&runtime_probe::on_code), this, 1, 0);
        if (result != UC_ERR_OK) {
            hook_ = 0;
            std::cerr << "[MUFLEX_V18] failed to install code hook: " << uc_strerror(result) << '\n';
            return false;
        }
        const auto write_result = uc_hook_add(context_->get_uc(), &write_hook_, UC_HOOK_MEM_WRITE,
                            reinterpret_cast<void*>(&runtime_probe::on_memory), this,
                            image_base_, image_base_ + image_size_ - 1);
        if (write_result != UC_ERR_OK) {
            std::cerr << "[MUFLEX_V19] failed to install image-write hook: "
                      << uc_strerror(write_result) << '\n';
            detach();
            return false;
        }
        started_ = true;
        std::cout << "[MUFLEX_V18] runtime probe active, instruction limit=" << budget_ << '\n';
        return true;
    }

    void finish(const bool startup_returned, const std::string& error,
                const std::uint32_t entry_rva, const std::size_t recovered_imports,
                const std::size_t vm_trampolines) {
        detach();
        if (trace_) trace_.flush();
        if (image_writes_) image_writes_.flush();
        hotspot_.finish();

        std::ofstream summary(output_ / "runtime_summary.txt", std::ios::trunc);
        if (!summary) {
            std::cerr << "[MUFLEX_V18] cannot create runtime_summary.txt\n";
            return;
        }
        summary << "mode=UNICORN_RUNTIME_OBSERVATION_ONLY\n"
                << "game_server_or_client_was_not_launched_natively=true\n"
                << "limit=" << budget_ << "\n"
                << "instructions_observed=" << observed_ << "\n"
                << "instruction_limit_reached=" << (budget_reached_ ? "true" : "false") << "\n"
                << "startup_returned=" << (startup_returned ? "true" : "false") << "\n"
                << "error=" << sanitize(error) << "\n"
                << "entry_after_probe_rva=0x" << std::hex << entry_rva << std::dec << "\n"
                << "entry_is_confirmed_game_oep=false\n"
                << "imports_recovered_so_far=" << recovered_imports << "\n"
                << "vm_trampolines_so_far=" << vm_trampolines << "\n"
                << "last_observed_pc=0x" << std::hex << last_pc_ << std::dec << "\n"
                << "trace_rows=" << rows_ << "\n"
                << "stop_reason=" << (budget_reached_ ? "instruction_budget_reached" :
                    (startup_returned ? "startup_returned" : "startup_exception_or_unknown")) << "\n"
                << "image_write_events=" << total_image_writes_ << "\n"
                << "image_write_rows=" << write_rows_ << "\n"
                << "distinct_image_pages_written=" << page_counts_.size() << "\n"
                << "hotspot_enabled=" << (hotspot_.enabled() ? "true" : "false") << "\n"
                << "hotspot_guest_writes=" << hotspot_.write_count() << "\n"
                << "hotspot_distinct_keys=" << hotspot_.distinct_keys() << "\n"
                << "hotspot_dropped_key_events=" << hotspot_.dropped_keys() << "\n"
                << "hotspot_changed_bytes=" << hotspot_.changed_bytes() << "\n";
        for (const auto& [region, count] : region_counts_) {
            summary << "instructions_in_" << sanitize(region) << "=" << count << '\n';
        }
        for (const auto& [region, count] : write_region_counts_) {
            summary << "writes_in_" << sanitize(region) << "=" << count << '\n';
        }
        std::ofstream pages(output_ / "runtime_image_pages.csv", std::ios::trunc);
        if (pages) {
            pages << "page_rva,region,write_events\n";
            for (const auto& [page_rva, count] : page_counts_) {
                pages << "0x" << std::hex << page_rva << std::dec << ','
                      << region_at(image_base_ + page_rva) << ',' << count << '\n';
            }
        }
        summary << "image_write_hook_guest_instructions_only=true\n"
                << "write_values_larger_than_8_bytes_are_truncated=true\n"
                << "warning=Unverified observational data; do not execute any Stage0 PE.\n";
        std::cout << "[MUFLEX_V18] wrote " << rows_ << " trace rows; observed " << observed_
                  << " instructions; limit reached=" << (budget_reached_ ? "yes" : "no")
                  << "; output=" << output_.string() << '\n';
    }

    [[nodiscard]] bool limit_reached() const { return budget_reached_; }

private:
    static constexpr std::uint64_t k_first_instructions = 256;
    static constexpr std::uint64_t k_sample_period = 4096;
    static constexpr std::uint64_t k_max_rows = 4096;
    static constexpr std::uint64_t k_max_write_rows = 4096;

    static std::string sanitize(std::string s) {
        for (char& c : s) {
            if (c == '\n' || c == '\r' || c == ',' || c == '=') c = ' ';
        }
        return s;
    }

    std::string region_at(const std::uint64_t pc) const {
        if (pc < image_base_) return "external";
        const auto rva = pc - image_base_;
        for (const auto& section : image_->get_sections()) {
            const auto start = static_cast<std::uint64_t>(section.virtual_address);
            const auto end = start + (section.virtual_size > section.raw_size ? section.virtual_size : section.raw_size);
            if (rva >= start && rva < end) {
                return section.name.empty() ? "unnamed_section" : section.name;
            }
        }
        return "outside_sections";
    }

    static void on_code(uc_engine* uc, const std::uint64_t pc,
                        const std::uint32_t size, void* user_data) {
        auto* self = static_cast<runtime_probe*>(user_data);
        if (self && self->started_) self->observe(uc, pc, size);
    }


    static void on_memory(uc_engine* uc, uc_mem_type type,
                          std::uint64_t address, int size,
                          std::int64_t value, void* user_data) {
        auto* self = static_cast<runtime_probe*>(user_data);
        if (!self || !self->started_ || type != UC_MEM_WRITE || size <= 0) return;
        if (self->hotspot_.enabled()) {
            const auto hot_va = self->image_base_ + muflex::runtime_hotspot::k_hotspot_rva;
            if (address >= hot_va && address < hot_va + muflex::runtime_hotspot::k_page_size) {
                self->hotspot_.observe_write(self->observed_, self->context_->get_pc(),
                                             address, size, static_cast<std::uint64_t>(value));
            }
        }
        self->observe_write(uc, address, size, value);
    }

    void observe_write(uc_engine* uc, const std::uint64_t dest,
                       const int size, const std::int64_t value) {
        if (dest < image_base_ || dest - image_base_ >= image_size_) return;
        const auto dest_rva = dest - image_base_;
        const auto region = region_at(dest);
        ++total_image_writes_;
        ++write_region_counts_[region];
        const auto page_rva = dest_rva & ~std::uint64_t{0xFFF};
        const auto page_entry = page_counts_.try_emplace(page_rva, 0);
        ++page_entry.first->second;

        // Always save the first observed writes in each page/section, then
        // sample to prevent unbounded CSV output during heavy obfuscation.
        const bool useful = page_entry.second || page_entry.first->second <= 2 ||
                            total_image_writes_ <= 128 ||
                            total_image_writes_ % 4096 == 0;
        if (!useful || write_rows_ >= k_max_write_rows) return;

        // The callback PC is the address currently executing in the emulator.
        const auto pc = context_->get_pc();
        const auto pc_rva = pc >= image_base_ ? pc - image_base_ : 0;
        image_writes_ << total_image_writes_ << ',' << observed_
                      << ",0x" << std::hex << pc
                      << ",0x" << pc_rva
                      << ",0x" << dest
                      << ",0x" << dest_rva
                      << ',' << region
                      << ",0x" << page_rva
                      << std::dec << ',' << size
                      << ",0x" << std::hex << static_cast<std::uint64_t>(value)
                      << std::dec << '\n';
        ++write_rows_;
    }

    void observe(uc_engine* uc, const std::uint64_t pc, const std::uint32_t size) {
        ++observed_;
        last_pc_ = pc;
        const auto region = region_at(pc);
        ++region_counts_[region];
        const bool transition = region != last_region_;
        last_region_ = region;

        if (rows_ < k_max_rows && (observed_ <= k_first_instructions ||
                                   transition || observed_ % k_sample_period == 0 ||
                                   observed_ == budget_)) {
            std::array<std::uint8_t, 16> bytes{};
            const auto read_size = (size < bytes.size() ? size : static_cast<std::uint32_t>(bytes.size()));
            const auto read_status = uc_mem_read(uc, pc, bytes.data(), read_size);
            std::ostringstream hex;
            if (read_status == UC_ERR_OK) {
                for (std::uint32_t i = 0; i < read_size; ++i) {
                    hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]);
                }
            } else {
                hex << "unreadable";
            }
            const auto rva = pc >= image_base_ ? pc - image_base_ : 0;
            trace_ << observed_ << ",0x" << std::hex << pc << ",0x" << rva << std::dec
                   << ',' << region << ',' << size << ',' << hex.str()
                   << ',' << (transition ? "region_change" : "sample") << '\n';
            ++rows_;
        }

        if (observed_ >= budget_) {
            budget_reached_ = true;
            uc_emu_stop(uc);
        }
    }

    void detach() {
        if (write_hook_) {
            const auto result = uc_hook_del(context_->get_uc(), write_hook_);
            if (result != UC_ERR_OK) {
                std::cerr << "[MUFLEX_V19] failed to remove image-write hook: "
                          << uc_strerror(result) << '\n';
            }
            write_hook_ = 0;
        }
        if (hook_) {
            const auto result = uc_hook_del(context_->get_uc(), hook_);
            if (result != UC_ERR_OK) {
                std::cerr << "[MUFLEX_V18] failed to remove hook: " << uc_strerror(result) << '\n';
            }
            hook_ = 0;
        }
    }

    std::shared_ptr<c_pe_image> image_;
    std::shared_ptr<c_emulator_ctx> context_;
    std::uint64_t image_base_ = 0;
    std::uint64_t image_size_ = 0;
    std::filesystem::path output_;
    muflex::runtime_hotspot hotspot_;
    std::ofstream trace_;
    std::ofstream image_writes_;
    std::map<std::string, std::uint64_t> write_region_counts_;
    std::map<std::uint64_t, std::uint64_t> page_counts_;
    std::map<std::string, std::uint64_t> region_counts_;
    std::uint64_t budget_ = 0;
    std::uint64_t observed_ = 0;
    std::uint64_t last_pc_ = 0;
    std::uint64_t rows_ = 0;
    std::uint64_t total_image_writes_ = 0;
    std::uint64_t write_rows_ = 0;
    uc_hook hook_ = 0;
    uc_hook write_hook_ = 0;
    std::string last_region_;
    bool budget_reached_ = false;
    bool started_ = false;
};

}  // namespace muflex

#endif  // MUFLEX_RUNTIME_PROBE_HPP
