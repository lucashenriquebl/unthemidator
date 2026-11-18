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

namespace muflex {

class runtime_probe final {
public:
    runtime_probe(const std::shared_ptr<c_pe_image>& image,
                  const std::shared_ptr<c_emulator_ctx>& context,
                  const std::shared_ptr<c_memory_manager>& memory,
                  const std::filesystem::path& directory,
                  const std::uint64_t budget)
        : image_(image), context_(context),
          image_base_(memory->get_image_base()), output_(directory),
          budget_(budget) {}

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

        const auto result = uc_hook_add(context_->get_uc(), &hook_, UC_HOOK_CODE,
                           reinterpret_cast<void*>(&runtime_probe::on_code), this, 1, 0);
        if (result != UC_ERR_OK) {
            hook_ = 0;
            std::cerr << "[MUFLEX_V18] failed to install code hook: " << uc_strerror(result) << '\n';
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
                << "trace_rows=" << rows_ << "\n";
        for (const auto& [region, count] : region_counts_) {
            summary << "instructions_in_" << sanitize(region) << "=" << count << '\n';
        }
        summary << "warning=Unverified observational data; do not execute any Stage0 PE.\n";
        std::cout << "[MUFLEX_V18] wrote " << rows_ << " trace rows; observed " << observed_
                  << " instructions; limit reached=" << (budget_reached_ ? "yes" : "no")
                  << "; output=" << output_.string() << '\n';
    }

    [[nodiscard]] bool limit_reached() const { return budget_reached_; }

private:
    static constexpr std::uint64_t k_first_instructions = 256;
    static constexpr std::uint64_t k_sample_period = 4096;
    static constexpr std::uint64_t k_max_rows = 4096;

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
    std::filesystem::path output_;
    std::ofstream trace_;
    std::map<std::string, std::uint64_t> region_counts_;
    std::uint64_t budget_ = 0;
    std::uint64_t observed_ = 0;
    std::uint64_t last_pc_ = 0;
    std::uint64_t rows_ = 0;
    uc_hook hook_ = 0;
    std::string last_region_;
    bool budget_reached_ = false;
    bool started_ = false;
};

}  // namespace muflex

#endif  // MUFLEX_RUNTIME_PROBE_HPP
