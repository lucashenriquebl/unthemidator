#ifndef MUFLEX_STAGE0_DIAGNOSTICS_HPP
#define MUFLEX_STAGE0_DIAGNOSTICS_HPP

// Opt-in forensic snapshots for the exact MUDevs S21 GameServer Stage0 layout.
// This does not start the target program or claim full Themida removal.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#include "../emulator/c_emulator_ctx.hpp"
#include "../memory/c_memory_manager.hpp"
#include "../pe/c_pe_image.hpp"

namespace muflex {

class stage0_diagnostics final {
public:
    stage0_diagnostics(const std::shared_ptr<c_pe_image>& pe,
                       const std::shared_ptr<c_emulator_ctx>& context,
                       const std::shared_ptr<c_memory_manager>& memory)
        : context_(context), image_base_(memory->get_image_base()) {
        const char* env_dir = std::getenv("MUFLEX_STAGE0_DIR");
        if (!env_dir || !*env_dir) {
            return;
        }

        // Never probe unrelated binaries: these constants describe only the
        // supplied MUDevs GameServer SHA-256 ec083496...e7884 PE layout.
        if (!pe->is_x64() || pe->get_entry_rva() != 0xAF07058) {
            std::cerr << "[MUFLEX_V14] unsupported input layout; diagnostics skipped\n";
            return;
        }
        bool known_section = false;
        for (const auto& section : pe->get_sections()) {
            if (section.name == ".themida" && section.virtual_address == k_themida_rva) {
                known_section = true;
                break;
            }
        }
        if (!known_section) {
            std::cerr << "[MUFLEX_V14] expected .themida section not found; diagnostics skipped\n";
            return;
        }

        output_dir_ = std::filesystem::path(env_dir);
        std::error_code error;
        std::filesystem::create_directories(output_dir_, error);
        if (error) {
            std::cerr << "[MUFLEX_V14] cannot create diagnostics directory: " << error.message() << '\n';
            return;
        }
        enabled_ = true;
    }

    stage0_diagnostics(const stage0_diagnostics&) = delete;
    stage0_diagnostics& operator=(const stage0_diagnostics&) = delete;

    ~stage0_diagnostics() { remove_watch(); }

    void begin() {
        if (!enabled_) return;
        capture_window("before");
        csv_.open(output_dir_ / "GameServer.prelude.LZ_READS.csv", std::ios::trunc);
        if (!csv_) {
            std::cerr << "[MUFLEX_V14] cannot open watch CSV\n";
            return;
        }
        csv_ << "type,va,rip,read_size,value,first_two_bytes,status\n";
        const std::uint64_t watched_va = image_base_ + k_themida_rva - k_backref_distance;
        const auto result = uc_hook_add(context_->get_uc(), &watch_hook_,
                                        UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                                        reinterpret_cast<void*>(&stage0_diagnostics::memory_event),
                                        this, watched_va - 16, watched_va + 16);
        if (result != UC_ERR_OK) {
            watch_hook_ = 0;
            std::cerr << "[MUFLEX_V14] memory watch unavailable: " << uc_strerror(result) << '\n';
        }

        // Observe writes to the actual output, not only a presumed LZ source.
        writes_csv_.open(output_dir_ / "GameServer.prelude.LZ_WRITES.csv", std::ios::trunc);
        if (writes_csv_) {
            writes_csv_ << "event,va,rva,dest_offset,rip,rip_rva,write_size,value_u64,rsi,rdi\\n";
            const std::uint64_t destination = image_base_ + k_themida_rva;
            const auto write_status = uc_hook_add(context_->get_uc(), &write_hook_,
                               UC_HOOK_MEM_WRITE,
                               reinterpret_cast<void*>(&stage0_diagnostics::write_event),
                               this, destination, destination + k_trace_size - 1);
            if (write_status != UC_ERR_OK) {
                write_hook_ = 0;
                std::cerr << "[MUFLEX_V14] output write trace unavailable: "
                          << uc_strerror(write_status) << '\\n';
            }
        } else {
            std::cerr << "[MUFLEX_V14] cannot open output write CSV\\n";
        }
    }

    void finish(const char* stage) {
        if (!enabled_) return;
        remove_watch();
        if (csv_) csv_.flush();
        if (writes_csv_) writes_csv_.flush();
        capture_window(stage);
    }

private:
    static constexpr std::uint64_t k_themida_rva = 0xA34F000;
    static constexpr std::uint64_t k_backref_distance = 165;
    static constexpr std::uint64_t k_window_rva = k_themida_rva - 0x1000;
    static constexpr std::size_t k_page_size = 0x1000;
    static constexpr std::size_t k_max_events = 256;
    static constexpr std::uint64_t k_trace_size = 0x200;
    static constexpr std::size_t k_max_write_events = 4096;

    static void memory_event(uc_engine* uc, uc_mem_type type, std::uint64_t address,
                             int size, std::int64_t value, void* user_data) {
        auto* self = static_cast<stage0_diagnostics*>(user_data);
        if (!self || !self->csv_ || self->event_count_ >= k_max_events) return;
        const std::uint64_t watched_va = self->image_base_ + k_themida_rva - k_backref_distance;
        // Log only accesses that actually overlap the two suspect bytes.
        if (size <= 0 || address >= watched_va + 2 ||
            (address < watched_va && static_cast<std::uint64_t>(size) <= watched_va - address)) {
            return;
        }
        ++self->event_count_;
        std::uint64_t rip = 0;
        const auto rip_result = uc_reg_read(uc, UC_X86_REG_RIP, &rip);
        std::array<std::uint8_t, 2> first_two{};
        const auto read_result = uc_mem_read(uc, watched_va, first_two.data(), first_two.size());
        self->csv_ << static_cast<int>(type) << ",0x" << std::hex << address
                   << ",0x" << (rip_result == UC_ERR_OK ? rip : 0) << std::dec
                   << ',' << size << ',' << value << ','
                   << static_cast<unsigned>(first_two[0]) << '-'
                   << static_cast<unsigned>(first_two[1]) << ','
                   << static_cast<int>(read_result) << '\n';
    }

    static void write_event(uc_engine* uc, uc_mem_type type, std::uint64_t address,
                            int size, std::int64_t value, void* user_data) {
        auto* self = static_cast<stage0_diagnostics*>(user_data);
        if (!self || !self->writes_csv_ || type != UC_MEM_WRITE ||
            self->write_event_count_ >= k_max_write_events || size <= 0) return;
        const std::uint64_t destination = self->image_base_ + k_themida_rva;
        if (address >= destination + k_trace_size ||
            (address < destination && static_cast<std::uint64_t>(size) <= destination - address)) return;
        std::uint64_t rip = 0, rsi = 0, rdi = 0;
        const auto pc_status = uc_reg_read(uc, UC_X86_REG_RIP, &rip);
        uc_reg_read(uc, UC_X86_REG_RSI, &rsi);
        uc_reg_read(uc, UC_X86_REG_RDI, &rdi);
        const auto rip_rva = pc_status == UC_ERR_OK && rip >= self->image_base_
            ? rip - self->image_base_ : 0;
        ++self->write_event_count_;
        self->writes_csv_ << self->write_event_count_ << ",0x" << std::hex << address
                          << ",0x" << (address >= self->image_base_ ? address - self->image_base_ : 0)
                          << ",0x" << (address >= destination ? address - destination : 0)
                          << ",0x" << (pc_status == UC_ERR_OK ? rip : 0)
                          << ",0x" << rip_rva << std::dec << ',' << size
                          << ",0x" << std::hex << static_cast<std::uint64_t>(value)
                          << ",0x" << rsi << ",0x" << rdi << std::dec << '\\n';
    }

    void remove_watch() {
        if (write_hook_) {
            const auto result = uc_hook_del(context_->get_uc(), write_hook_);
            if (result != UC_ERR_OK) {
                std::cerr << "[MUFLEX_V14] output write watch removal failed: " << uc_strerror(result) << '\\n';
            }
            write_hook_ = 0;
        }
        if (watch_hook_) {
            const auto result = uc_hook_del(context_->get_uc(), watch_hook_);
            if (result != UC_ERR_OK) {
                std::cerr << "[MUFLEX_V14] memory watch removal failed: " << uc_strerror(result) << '\n';
            }
            watch_hook_ = 0;
        }
    }

    void capture_window(const char* stage) {
        const std::string basename = std::string("GameServer.prelude.") + stage;
        std::ofstream bin(output_dir_ / (basename + ".bin"), std::ios::binary | std::ios::trunc);
        std::ofstream metadata(output_dir_ / (basename + ".txt"), std::ios::trunc);
        if (!bin || !metadata) {
            std::cerr << "[MUFLEX_V14] cannot open snapshot " << basename << '\n';
            return;
        }
        metadata << "window_start_rva=0x" << std::hex << k_window_rva
                 << "\nsource_rva=0x" << (k_themida_rva - k_backref_distance)
                 << "\nwindow_size=0x2000\n" << std::dec;
        // Read each page independently so an unmapped preceding page does not
        // obscure the readable .themida page. An unsuccessful page is zeroed
        // and explicitly flagged in the metadata (not a validated memory dump).
        for (std::size_t i = 0; i != 2; ++i) {
            std::array<std::uint8_t, k_page_size> page{};
            const auto rva = k_window_rva + i * k_page_size;
            const auto status = uc_mem_read(context_->get_uc(), image_base_ + rva,
                                            page.data(), page.size());
            bin.write(reinterpret_cast<const char*>(page.data()),
                      static_cast<std::streamsize>(page.size()));
            metadata << "page_" << i << "_rva=0x" << std::hex << rva << std::dec
                     << "\npage_" << i << "_uc_status=" << static_cast<int>(status) << '\n';
        }
        metadata << "warning=observational_unverified_not_executable\n";
        std::cout << "[MUFLEX_V14] prelude " << stage << ": "
                  << (output_dir_ / (basename + ".bin")).string() << '\n';
    }

    std::shared_ptr<c_emulator_ctx> context_;
    std::uint64_t image_base_ = 0;
    std::filesystem::path output_dir_;
    std::ofstream csv_;
    std::ofstream writes_csv_;
    uc_hook watch_hook_ = 0;
    uc_hook write_hook_ = 0;
    std::size_t write_event_count_ = 0;
    std::size_t event_count_ = 0;
    bool enabled_ = false;
};

}  // namespace muflex

#endif // MUFLEX_STAGE0_DIAGNOSTICS_HPP
