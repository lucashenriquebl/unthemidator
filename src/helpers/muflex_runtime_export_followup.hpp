#ifndef MUFLEX_RUNTIME_EXPORT_FOLLOWUP_HPP
#define MUFLEX_RUNTIME_EXPORT_FOLLOWUP_HPP

// V26: passive bounded follow-up after the observed 'L' matches in a
// synthetic kernel32 export-name scan. Does not mutate Unicorn state.
#include <unicorn/unicorn.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace muflex {
class runtime_export_followup final {
public:
    static constexpr std::uint64_t k_cmp_jne_rva = 0xA3B0FC1ULL;
    static constexpr std::uint64_t k_themida_begin_rva = 0xA34F000ULL;
    static constexpr std::uint64_t k_themida_end_rva = 0xAF07000ULL;
    static constexpr std::size_t k_max_matches = 32;
    static constexpr std::size_t k_window_instructions = 128;

    runtime_export_followup(std::filesystem::path output, std::uint64_t image_base,
                            std::uint64_t image_size)
        : output_(std::move(output)), base_(image_base), size_(image_size) {}

    bool begin() {
        trace_.open(output_ / "v26_L_followup.csv", std::ios::trunc);
        flags_.open(output_ / "v26_runtime_flags.csv", std::ios::trunc);
        if (!trace_ || !flags_) return false;
        trace_ << "event_id,name,insn_index,pc,size,opcode_hex,next_pc,flow_type,"
                  "rax,rbx,rcx,rdx,rdi,rsi,r8,r9,rsp,rflags,read_status\n";
        flags_ << "insn_index,type,pc,details\n";
        enabled_ = true;
        return true;
    }

    void observe(uc_engine* uc, std::uint64_t insn_index,
                 std::uint64_t pc, std::uint32_t size) {
        if (!enabled_) return;
        if (!outside_seen_ && pc >= base_ && pc - base_ < size_ &&
            !(pc - base_ >= k_themida_begin_rva && pc - base_ < k_themida_end_rva)) {
            outside_seen_ = true;
            event(insn_index, "FIRST_PC_OUTSIDE_THEMIDA", pc, "Not confirmed game OEP");
        }

        // Each match owns an independent 128-instruction window, including
        // overlapping windows when the table is scanned in quick succession.
        if (!windows_.empty()) {
            for (auto& w : windows_) {
                const auto flow = (w.previous.pc + w.previous.size == pc)
                                  ? "sequential" : "nonsequential";
                write(w.id, w.name, w.previous, pc, flow);
                ++w.steps;
            }
            windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
                [](const window& w) { return w.steps >= k_window_instructions; }), windows_.end());
            if (!windows_.empty()) {
                const auto current = sample(uc, insn_index, pc, size);
                for (auto& w : windows_) w.previous = current;
            }
        }

        if (pc != base_ + k_cmp_jne_rva) return;
        const auto rax = reg(uc, UC_X86_REG_RAX);
        const auto rdi = reg(uc, UC_X86_REG_RDI);
        const auto flags = reg(uc, UC_X86_REG_EFLAGS);
        std::uint8_t first = 0;
        if (!(flags & (1ULL << 6)) || (rax & 0xffULL) != 'L' ||
            uc_mem_read(uc, rdi, &first, 1) != UC_ERR_OK || first != 'L') return;
        const auto name = read_name(uc, rdi);
        if (!name) {
            event(insn_index, "MATCH_NAME_UNREADABLE", pc, "RDI name could not be read");
            return;
        }
        ++matches_;
        if (matches_ > k_max_matches) {
            ++dropped_;
            return;
        }
        windows_.push_back(window{matches_, *name, 0, sample(uc, insn_index, pc, size)});
    }

    void finish() {
        if (!enabled_) return;
        for (const auto& w : windows_) write(w.id, w.name, w.previous, 0, "unknown_next_pc");
        windows_.clear();
        flags_ << "0,SUMMARY,0x0,matched_L_events=" << matches_ << '\n';
        flags_ << "0,SUMMARY,0x0,windows_dropped=" << dropped_ << '\n';
        flags_ << "0,SUMMARY,0x0,first_outside_themida=" << (outside_seen_ ? 1 : 0) << '\n';
        trace_.flush();
        flags_.flush();
        enabled_ = false;
    }

    ~runtime_export_followup() { finish(); }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] std::uint64_t matches() const { return matches_; }

private:
    struct snapshot {
        std::uint64_t insn=0, pc=0;
        std::uint32_t size=0;
        std::array<std::uint64_t,10> registers{};
        std::string opcode;
        bool readable=false;
    };
    struct window {
        std::uint64_t id=0;
        std::string name;
        std::size_t steps=0;
        snapshot previous;
    };

    static std::uint64_t reg(uc_engine* uc, int which) {
        std::uint64_t result=0;
        if (uc_reg_read(uc, which, &result) != UC_ERR_OK) return 0;
        return result;
    }
    static std::string hex(std::uint64_t value) {
        std::ostringstream s;
        s << "0x" << std::hex << value;
        return s.str();
    }
    static std::optional<std::string> read_name(uc_engine* uc, std::uint64_t rdi) {
        std::string result;
        for (unsigned n=0; n<96; ++n) {
            std::uint8_t c=0;
            if (uc_mem_read(uc, rdi+n, &c, 1) != UC_ERR_OK) return std::nullopt;
            if (c == 0) return result.empty() ? std::nullopt : std::optional<std::string>(result);
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '@' || c == '.')) return std::nullopt;
            result.push_back(static_cast<char>(c));
        }
        return std::nullopt;
    }
    static snapshot sample(uc_engine* uc, std::uint64_t idx,
                           std::uint64_t pc, std::uint32_t length) {
        snapshot s{};
        s.insn=idx; s.pc=pc; s.size=length;
        constexpr std::array<int,10> regs{{UC_X86_REG_RAX,UC_X86_REG_RBX,UC_X86_REG_RCX,
            UC_X86_REG_RDX,UC_X86_REG_RDI,UC_X86_REG_RSI,UC_X86_REG_R8,UC_X86_REG_R9,
            UC_X86_REG_RSP,UC_X86_REG_EFLAGS}};
        for (std::size_t i=0; i<regs.size(); ++i) s.registers[i]=reg(uc,regs[i]);
        std::array<std::uint8_t,16> bytes{};
        const auto n=std::min<std::uint32_t>(length,static_cast<std::uint32_t>(bytes.size()));
        s.readable = n > 0 && uc_mem_read(uc,pc,bytes.data(),n)==UC_ERR_OK;
        if (s.readable) {
            std::ostringstream stream;
            for (unsigned i=0; i<n; ++i)
                stream << std::hex << std::setfill('0') << std::setw(2) << unsigned(bytes[i]);
            s.opcode=stream.str();
        }
        return s;
    }
    void write(std::uint64_t id, const std::string& name, const snapshot& s,
               std::uint64_t next_pc, const char* flow) {
        trace_ << id << ',' << name << ',' << s.insn << ',' << hex(s.pc) << ',' << s.size
               << ',' << s.opcode << ',' << (next_pc ? hex(next_pc) : "") << ',' << flow;
        for (const auto v : s.registers) trace_ << ',' << hex(v);
        trace_ << ',' << (s.readable ? "ok" : "unreadable") << '\n';
    }
    void event(std::uint64_t idx, const char* kind, std::uint64_t pc, const char* details) {
        flags_ << idx << ',' << kind << ',' << hex(pc) << ',' << details << '\n';
    }
    std::filesystem::path output_;
    std::uint64_t base_=0, size_=0;
    std::ofstream trace_,flags_;
    std::vector<window> windows_;
    std::uint64_t matches_=0,dropped_=0;
    bool outside_seen_=false,enabled_=false;
};
} // namespace muflex
#endif // MUFLEX_RUNTIME_EXPORT_FOLLOWUP_HPP
