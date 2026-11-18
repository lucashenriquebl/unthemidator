#ifndef MUFLEX_RUNTIME_BRANCH_PROBE_HPP
#define MUFLEX_RUNTIME_BRANCH_PROBE_HPP

// V24: Observe two verified JNE instructions in the S21 GameServer x64
// Unicorn startup. Read-only observation: no guest registers/bytes modified.

#include <unicorn/unicorn.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include "../emulator/c_emulator_ctx.hpp"
#include "../pe/c_pe_image.hpp"

namespace muflex {

class runtime_branch_probe final {
public:
    static constexpr std::size_t k_max_rows = 16384;
    static constexpr std::uint64_t k_image_entry_rva = 0xAF07058;

    runtime_branch_probe(const std::shared_ptr<c_pe_image>& image,
                         const std::shared_ptr<c_emulator_ctx>& ctx,
                         std::uint64_t image_base,
                         std::filesystem::path out)
        : context_(ctx), image_base_(image_base), output_(std::move(out)) {
        enabled_ = image->is_x64() && image->get_entry_rva() == k_image_entry_rva;
    }

    void begin() {
        if (!enabled_) return;
        const auto uc = context_->get_uc();
        for (const auto& probe : probes_) {
            std::array<std::uint8_t, 6> bytes{};
            if (uc_mem_read(uc, image_base_ + probe.rva, bytes.data(), bytes.size()) != UC_ERR_OK ||
                std::memcmp(bytes.data(), probe.jne_bytes.data(), bytes.size()) != 0) {
                enabled_ = false;
                std::cerr << "[MUFLEX_V24] JNE signature mismatch; branch probe disabled.\n";
                return;
            }
        }
        std::cout << "[MUFLEX_V24] two GameServer JNE signatures verified; branch observation enabled.\n";
    }

    void observe(uc_engine* uc, const std::uint64_t pc,
                 const std::uint64_t instruction_index) {
        if (!enabled_) return;

        // Resolve the previously observed JNE using the next instruction
        // reported by Unicorn. This does not infer a taken branch from ZF.
        if (pending_ >= 0) {
            const auto index = static_cast<std::size_t>(pending_);
            const auto& probe = probes_[index];
            auto& state = states_[index];
            const auto taken_pc = image_base_ + probe.taken_rva;
            const auto fallthrough_pc = image_base_ + probe.fallthrough_rva;
            std::uint8_t decision = 2; // Other/resumed/unknown
            if (pc == taken_pc) {
                ++state.taken;
                decision = 1;
            } else if (pc == fallthrough_pc) {
                ++state.fallthrough;
                decision = 0;
            } else {
                ++state.other;
            }

            // JNE should take when ZF=0. Only check observed known outcomes.
            if (decision != 2 && (pending_row_.zf == 0) != (decision == 1)) {
                ++state.flag_decision_mismatches;
            }
            if (index == 1 && decision != 2) {
                // The immediate predecessor is DEC DH via an unconditional JMP.
                auto& histogram = decision == 1 ? dh_taken_ : dh_fallthrough_;
                ++histogram[pending_row_.dh];
            }
            pending_row_.destination = pc;
            pending_row_.decision = decision;
            if (rows_.size() < k_max_rows) {
                rows_.push_back(pending_row_);
            } else {
                ++discarded_rows_;
            }
            pending_ = -1;
        }

        for (std::size_t index = 0; index < probes_.size(); ++index) {
            const auto& probe = probes_[index];
            if (pc != image_base_ + probe.rva) continue;
            auto& state = states_[index];
            ++state.observed;
            probe_row row{};
            row.probe_index = index;
            row.occurrence = state.observed;
            row.instruction_index = instruction_index;
            row.pc = pc;
            row.destination = 0;
            row.decision = 3; // pending
            read_reg(uc, UC_X86_REG_RFLAGS, row.rflags);
            read_reg(uc, UC_X86_REG_RAX, row.rax);
            read_reg(uc, UC_X86_REG_RDX, row.rdx);
            read_reg(uc, UC_X86_REG_RDI, row.rdi);
            read_reg(uc, UC_X86_REG_RBX, row.rbx);
            read_reg(uc, UC_X86_REG_RSP, row.rsp);
            row.zf = (row.rflags & (1ULL << 6)) ? 1 : 0;
            row.al = static_cast<std::uint8_t>(row.rax & 0xFF);
            row.dh = static_cast<std::uint8_t>((row.rdx >> 8) & 0xFF);

            if (index == 0) {
                row.memory_readable = (uc_mem_read(uc, row.rdi, &row.mem_rdi, 1) == UC_ERR_OK);
                if (row.memory_readable) {
                    ++state.readable_rdi;
                    if ((row.al == row.mem_rdi) != (row.zf == 1)) {
                        ++state.cmp_flag_mismatches;
                    }
                } else {
                    ++state.failed_rdi_reads;
                }
            }
            pending_row_ = row;
            pending_ = static_cast<int>(index);
            break;
        }
    }

    void finish() {
        if (!enabled_) {
            std::ofstream summary(output_ / "runtime_branch_summary.txt", std::ios::trunc);
            if (summary) summary << "enabled=false\nreason=Unexpected PE or JNE signature\n";
            return;
        }

        // The last JNE may be pending if execution hit the instruction budget
        // on that exact instruction. Keep it as unresolved, not a false exit.
        if (pending_ >= 0) {
            ++states_[static_cast<std::size_t>(pending_)].unresolved;
            pending_row_.decision = 3;
            if (rows_.size() < k_max_rows) rows_.push_back(pending_row_);
            else ++discarded_rows_;
            pending_ = -1;
        }

        std::ofstream decisions(output_ / "runtime_branch_decisions.csv", std::ios::trunc);
        if (decisions) {
            decisions << "branch,occurrence,instruction_index,branch_pc,next_pc,"
                         "decision,zf,dh,al,rdi,mem_rdi_readable,mem_rdi,rax,rdx,rbx,rsp,rflags\n";
            for (const auto& row : rows_) {
                const auto& name = probes_[row.probe_index].name;
                decisions << name << ',' << row.occurrence << ',' << row.instruction_index
                          << ",0x" << std::hex << row.pc
                          << ",0x" << row.destination << std::dec << ','
                          << decision_string(row.decision)
                          << ',' << static_cast<unsigned>(row.zf)
                          << ',' << static_cast<unsigned>(row.dh)
                          << ',' << static_cast<unsigned>(row.al)
                          << ",0x" << std::hex << row.rdi << std::dec << ','
                          << (row.memory_readable ? "true" : "false") << ','
                          << (row.memory_readable ? std::to_string(row.mem_rdi) : "")
                          << ",0x" << std::hex << row.rax
                          << ",0x" << row.rdx
                          << ",0x" << row.rbx
                          << ",0x" << row.rsp
                          << ",0x" << row.rflags
                          << std::dec << '\n';
            }
        }

        std::ofstream hist(output_ / "runtime_branch_dh_histogram.csv", std::ios::trunc);
        if (hist) {
            hist << "dh_at_jne,taken,fallthrough\n";
            for (std::size_t i = 0; i < 256; ++i) {
                if (dh_taken_[i] || dh_fallthrough_[i]) {
                    hist << i << ',' << dh_taken_[i] << ',' << dh_fallthrough_[i] << '\n';
                }
            }
        }

        std::ofstream summary(output_ / "runtime_branch_summary.txt", std::ios::trunc);
        if (summary) {
            summary << "mode=READ_ONLY_UNICORN_JNE_DECISION_OBSERVATION\n"
                    << "enabled=true\n"
                    << "recorded_rows=" << rows_.size() << '\n'
                    << "discarded_rows=" << discarded_rows_ << '\n';
            for (std::size_t i = 0; i < probes_.size(); ++i) {
                const auto& state = states_[i];
                const auto& probe = probes_[i];
                summary << "branch=" << probe.name << '\n'
                        << "branch_rva=0x" << std::hex << probe.rva << std::dec << '\n'
                        << "observed=" << state.observed << '\n'
                        << "taken=" << state.taken << '\n'
                        << "fallthrough=" << state.fallthrough << '\n'
                        << "other=" << state.other << '\n'
                        << "unresolved=" << state.unresolved << '\n'
                        << "zf_decision_mismatches=" << state.flag_decision_mismatches << '\n'
                        << "cmp_flag_mismatches=" << state.cmp_flag_mismatches << '\n'
                        << "readable_rdi=" << state.readable_rdi << '\n'
                        << "failed_rdi_reads=" << state.failed_rdi_reads << '\n';
            }
            summary << "warning=Observing a JNE loop in Themida does not identify original game logic.\n";
        }
        std::cout << "[MUFLEX_V24] JNE observations: cmp="
                  << states_[0].observed << ", decDH=" << states_[1].observed << '\n';
    }

    [[nodiscard]] bool enabled() const { return enabled_; }

private:
    struct branch_spec {
        const char* name;
        std::uint64_t rva;
        std::uint64_t taken_rva;
        std::uint64_t fallthrough_rva;
        std::array<std::uint8_t,6> jne_bytes;
    };
    struct branch_state {
        std::uint64_t observed = 0, taken = 0, fallthrough = 0;
        std::uint64_t other = 0, unresolved = 0;
        std::uint64_t flag_decision_mismatches = 0, cmp_flag_mismatches = 0;
        std::uint64_t readable_rdi = 0, failed_rdi_reads = 0;
    };
    struct probe_row {
        std::size_t probe_index = 0;
        std::uint64_t occurrence = 0, instruction_index = 0, pc = 0, destination = 0;
        std::uint64_t rflags = 0, rax = 0, rdx = 0, rdi = 0, rbx = 0, rsp = 0;
        std::uint8_t zf = 0, dh = 0, al = 0, mem_rdi = 0;
        std::uint8_t decision = 3;
        bool memory_readable = false;
    };

    static bool read_reg(uc_engine* uc, const int reg, std::uint64_t& out) {
        return uc_reg_read(uc, reg, &out) == UC_ERR_OK;
    }
    static const char* decision_string(const std::uint8_t decision) {
        switch (decision) {
            case 0: return "fallthrough";
            case 1: return "taken";
            case 2: return "other";
            default: return "unresolved";
        }
    }

    // Static six-byte JNE signatures validated against GameServer Stage0 V17.
    const std::array<branch_spec,2> probes_{{
        {"cmp_AL_RDI_JNE",0xA3B0FC1,0xA8D1FA8,0xA3B0FC7,
         {{0x0F,0x85,0xE1,0x0F,0x52,0x00}}},
        {"dec_DH_JNE",0xA396DD2,0xA384857,0xA396DD8,
         {{0x0F,0x85,0x7F,0xDA,0xFE,0xFF}}}
    }};
    std::shared_ptr<c_emulator_ctx> context_;
    std::uint64_t image_base_ = 0;
    std::filesystem::path output_;
    std::array<branch_state,2> states_{};
    std::array<std::uint64_t,256> dh_taken_{};
    std::array<std::uint64_t,256> dh_fallthrough_{};
    std::vector<probe_row> rows_;
    probe_row pending_row_{};
    int pending_ = -1;
    std::uint64_t discarded_rows_ = 0;
    bool enabled_ = false;
};

} // namespace muflex

#endif // MUFLEX_RUNTIME_BRANCH_PROBE_HPP
