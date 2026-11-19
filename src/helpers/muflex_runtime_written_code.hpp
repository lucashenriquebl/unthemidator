#ifndef MUFLEX_RUNTIME_WRITTEN_CODE_HPP
#define MUFLEX_RUNTIME_WRITTEN_CODE_HPP

// MuFlex V28: passive post-export trace of guest memory writes and later code
// fetches. No guest state is modified. A write/fetch overlap is not a game OEP.
#include <unicorn/unicorn.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace muflex {
class runtime_written_code final {
public:
    static constexpr std::uint64_t k_compare_rva = 0xA3B0FC1ULL;
    static constexpr std::uint64_t k_themida_start = 0xA34F000ULL;
    static constexpr std::uint64_t k_themida_end = 0xAF07000ULL;
    static constexpr std::uint64_t k_window_size = 65536ULL;
    static constexpr std::size_t k_page_size = 4096;
    static constexpr std::size_t k_max_pages = 4096;
    static constexpr std::size_t k_max_events = 256;

    runtime_written_code(std::filesystem::path output, std::uint64_t image_base,
                         std::uint64_t image_size)
        : output_(std::move(output)), base_(image_base), image_size_(image_size) {}

    bool begin() {
        windows_.open(output_ / "v28_post_export_windows.csv", std::ios::trunc);
        pages_file_.open(output_ / "v28_written_pages.csv", std::ios::trunc);
        events_.open(output_ / "v28_written_code_events.csv", std::ios::trunc);
        flags_.open(output_ / "v28_post_export_flags.txt", std::ios::trunc);
        if (!windows_ || !pages_file_ || !events_ || !flags_) return false;
        windows_ << "window,first_instruction,last_instruction,instructions,first_pc,last_pc,"
                    "distinct_pcs,untracked_pc_hits,nonsequential_transitions,guest_image_writes,"
                    "guest_external_writes,code_pc_on_written_page,code_overlap_written_bytes,"
                    "first_outside_themida_pc,first_rsp,last_rsp\n";
        pages_file_ << "page_va,page_rva,guest_write_events,bytes_marked_written,"
                       "first_write_instruction,last_write_instruction,first_writer_pc,"
                       "code_pc_on_page_after_write,code_overlap_written_bytes,"
                       "first_overlapping_execution_instruction,first_overlapping_execution_pc\n";
        events_ << "instruction_index,window,pc,pc_rva,instruction_size,"
                   "overlap_bytes,first_write_instruction,page_va,first_writer_pc,opcode_hex\n";
        enabled_ = true;
        return true;
    }

    void observe(uc_engine* uc, std::uint64_t idx, std::uint64_t pc, std::uint32_t size) {
        if (!enabled_) return;
        if (!active_) {
            if (pc != base_ + k_compare_rva) return;
            if (reg(uc, UC_X86_REG_RAX) % 256 != 'L' ||
                !(reg(uc, UC_X86_REG_EFLAGS) & (std::uint64_t{1} << 6))) return;
            const auto rdi = reg(uc, UC_X86_REG_RDI);
            std::array<char, sizeof("LoadLibraryA")> name{};
            if (uc_mem_read(uc, rdi, name.data(), name.size()) != UC_ERR_OK ||
                std::strcmp(name.data(), "LoadLibraryA") != 0) return;
            active_ = true;
            start_index_ = idx;
            start_pc_ = pc;
            event_rdi_ = rdi;
        }
        if (window_.count == k_window_size) flush_window();
        if (window_.count == 0) {
            window_.first_index = idx;
            window_.first_pc = pc;
            window_.first_rsp = reg(uc, UC_X86_REG_RSP);
        }
        if (window_.count && previous_pc_ + previous_size_ != pc) ++window_.nonsequential;
        window_.last_index = idx;
        window_.last_pc = pc;
        window_.last_rsp = reg(uc, UC_X86_REG_RSP);
        ++window_.count;
        ++instructions_;
        if (window_.pc_hits.size() < k_max_pages * 2 || window_.pc_hits.count(pc)) {
            ++window_.pc_hits[pc];
        } else {
            ++window_.untracked_pc_hits;
        }

        if (!inside_themida(pc)) {
            ++outside_themida_instructions_;
            if (!first_outside_pc_) first_outside_pc_ = pc;
            if (!window_.first_outside_pc) window_.first_outside_pc = pc;
        }
        const auto page_address = pc & ~std::uint64_t{0xfff};
        const auto page_it = written_pages_.find(page_address);
        if (page_it != written_pages_.end()) {
            ++page_it->second.code_hits_on_page_after_write;
            ++window_.code_hits_on_page_after_write;
        }

        // Test overlap with any previously *guest-written instruction bytes*.
        // Reading the whole instruction is unnecessary; no PE dump is generated.
        std::uint32_t overlap = 0;
        const page_state* first_written = nullptr;
        const auto inspect_size = std::min<std::uint32_t>(size, 15);
        for (std::uint32_t offset = 0; offset < inspect_size; ++offset) {
            const auto byte_pc = pc + offset;
            const auto it = written_pages_.find(byte_pc & ~std::uint64_t{0xfff});
            if (it == written_pages_.end()) continue;
            if (it->second.bytes_written.test(static_cast<std::size_t>(byte_pc & 0xfff))) {
                ++overlap;
                if (!first_written) first_written = &it->second;
            }
        }
        if (overlap) {
            ++window_.executions_overlapping_written_bytes;
            ++exact_overlap_events_;
            auto& ps = written_pages_[pc & ~std::uint64_t{0xfff}];
            ++ps.exec_written_byte_events;
            if (!ps.first_exec_instruction) {
                ps.first_exec_instruction = idx;
                ps.first_exec_pc = pc;
            }
            if (logged_pcs_.size() < k_max_events && logged_pcs_.insert(pc).second) {
                std::array<std::uint8_t, 16> opcode{};
                const auto amount = std::min<std::size_t>(size, opcode.size());
                const bool readable = amount > 0 &&
                    uc_mem_read(uc, pc, opcode.data(), amount) == UC_ERR_OK;
                std::ostringstream bytes;
                if (readable) for (std::size_t i = 0; i < amount; ++i)
                    bytes << std::hex << std::setfill('0') << std::setw(2)
                          << static_cast<unsigned>(opcode[i]);
                events_ << idx << ',' << (completed_windows_+1) << ',' << hx(pc) << ','
                        << (within_image(pc) ? hx(pc-base_) : "") << ','
                        << size << ',' << overlap << ','
                        << (first_written ? first_written->first_write_instruction : 0) << ','
                        << hx(pc & ~std::uint64_t{0xfff}) << ','
                        << (first_written ? hx(first_written->first_writer_pc) : "") << ','
                        << (readable ? bytes.str() : "unreadable") << '\n';
            }
        }
        previous_pc_ = pc;
        previous_size_ = size;
    }

    void observe_write(std::uint64_t idx, std::uint64_t writer_pc,
                       std::uint64_t destination, int length) {
        if (!enabled_ || !active_ || length <= 0) return;
        if (!within_image(destination)) {
            ++external_writes_;
            ++window_.external_writes;
            return;
        }
        ++image_writes_;
        ++window_.image_writes;
        // The Unicorn guest write callback may span pages. Bound the byte
        // bitmap update and record truncations explicitly.
        const auto bounded = std::min<int>(length, 4096);
        if (length > bounded) ++truncated_write_events_;
        std::uint64_t cursor = destination;
        std::size_t remaining = static_cast<std::size_t>(bounded);
        while (remaining > 0 && within_image(cursor)) {
            const auto page_va = cursor & ~std::uint64_t{0xfff};
            auto it = written_pages_.find(page_va);
            if (it == written_pages_.end()) {
                if (written_pages_.size() >= k_max_pages) {
                    ++dropped_page_events_;
                    return;
                }
                it = written_pages_.emplace(page_va, page_state{}).first;
                ++window_.new_written_pages;
            }
            auto& page = it->second;
            if (!page.first_write_instruction) {
                page.first_write_instruction = idx;
                page.first_writer_pc = writer_pc;
            }
            page.last_write_instruction = idx;
            ++page.guest_write_events;
            const auto local_offset = static_cast<std::size_t>(cursor & 0xfff);
            const auto count = std::min<std::size_t>(remaining, k_page_size - local_offset);
            for (std::size_t i = 0; i < count; ++i)
                page.bytes_written.set(local_offset + i);
            remaining -= count;
            cursor += count;
        }
    }

    void finish() {
        if (!enabled_) return;
        if (window_.count) flush_window();
        for (const auto& [page_va, s] : written_pages_) {
            pages_file_ << hx(page_va) << ','
                        << (within_image(page_va) ? hx(page_va-base_) : "") << ','
                        << s.guest_write_events << ',' << s.bytes_written.count() << ','
                        << s.first_write_instruction << ',' << s.last_write_instruction << ','
                        << hx(s.first_writer_pc) << ',' << s.code_hits_on_page_after_write
                        << ',' << s.exec_written_byte_events << ','
                        << s.first_exec_instruction << ','
                        << (s.first_exec_pc ? hx(s.first_exec_pc) : "") << '\n';
        }
        flags_ << "mode=PASSIVE_GUEST_WRITE_AND_CODE_FETCH_CORRELATION\n"
               << "activated=" << (active_?1:0) << '\n'
               << "trigger_observed_name=LoadLibraryA\n"
               << "trigger_is_not_proof_of_API_CALL=true\n"
               << "trigger_instruction=" << start_index_ << '\n'
               << "trigger_pc=" << hx(start_pc_) << '\n'
               << "trigger_rdi=" << hx(event_rdi_) << '\n'
               << "instructions_after_trigger_including_trigger=" << instructions_ << '\n'
               << "windows=" << completed_windows_ << '\n'
               << "guest_image_write_events=" << image_writes_ << '\n'
               << "guest_external_write_events=" << external_writes_ << '\n'
               << "tracked_written_pages=" << written_pages_.size() << '\n'
               << "dropped_page_events=" << dropped_page_events_ << '\n'
               << "truncated_write_events=" << truncated_write_events_ << '\n'
               << "executions_overlapping_prior_guest_written_bytes=" << exact_overlap_events_ << '\n'
               << "first_pc_outside_themida=" << (first_outside_pc_?hx(first_outside_pc_):"none") << '\n'
               << "instructions_outside_themida=" << outside_themida_instructions_ << '\n'
               << "recorded_distinct_written_code_pcs=" << logged_pcs_.size() << '\n'
               << "warning=Guest memory writes are not host writes; overlap is not evidence of game OEP or recovered imports.\n"
               << "warning=Bitmaps only cover writes after the LoadLibraryA name comparison and do not prove that writes changed bytes.\n";
        windows_.flush(); pages_file_.flush(); events_.flush(); flags_.flush();
        enabled_ = false;
    }
    ~runtime_written_code() { finish(); }
    [[nodiscard]] bool enabled() const { return enabled_; }
private:
    struct page_state {
        std::bitset<k_page_size> bytes_written;
        std::uint64_t guest_write_events=0, first_write_instruction=0;
        std::uint64_t last_write_instruction=0, first_writer_pc=0;
        std::uint64_t code_hits_on_page_after_write=0, exec_written_byte_events=0;
        std::uint64_t first_exec_instruction=0, first_exec_pc=0;
    };
    struct window_state {
        std::uint64_t count=0, first_index=0, last_index=0;
        std::uint64_t first_pc=0, last_pc=0, first_rsp=0, last_rsp=0;
        std::uint64_t nonsequential=0, untracked_pc_hits=0;
        std::uint64_t image_writes=0, external_writes=0, new_written_pages=0;
        std::uint64_t code_hits_on_page_after_write=0, executions_overlapping_written_bytes=0;
        std::uint64_t first_outside_pc=0;
        std::map<std::uint64_t, std::uint64_t> pc_hits;
    };
    static std::uint64_t reg(uc_engine* uc, int id) {
        std::uint64_t result=0;
        if (uc) (void) uc_reg_read(uc,id,&result);
        return result;
    }
    bool within_image(std::uint64_t a) const {
        return a >= base_ && a - base_ < image_size_;
    }
    bool inside_themida(std::uint64_t a) const {
        if (!within_image(a)) return false;
        const auto rva=a-base_;
        return rva >= k_themida_start && rva < k_themida_end;
    }
    static std::string hx(std::uint64_t value) {
        std::ostringstream out; out << "0x" << std::hex << value; return out.str();
    }
    void flush_window() {
        if (!window_.count) return;
        windows_ << completed_windows_+1 << ',' << window_.first_index << ','
                 << window_.last_index << ',' << window_.count << ','
                 << hx(window_.first_pc) << ',' << hx(window_.last_pc) << ','
                 << window_.pc_hits.size() << ',' << window_.untracked_pc_hits << ','
                 << window_.nonsequential << ',' << window_.image_writes << ','
                 << window_.external_writes << ','
                 << window_.code_hits_on_page_after_write << ','
                 << window_.executions_overlapping_written_bytes << ','
                 << (window_.first_outside_pc?hx(window_.first_outside_pc):"") << ','
                 << hx(window_.first_rsp) << ',' << hx(window_.last_rsp) << '\n';
        ++completed_windows_;
        window_ = window_state{};
        // Next window begins with the transition from its own first PC.
        previous_pc_ = 0;
        previous_size_ = 0;
    }
    std::filesystem::path output_;
    std::uint64_t base_=0,image_size_=0;
    std::ofstream windows_,pages_file_,events_,flags_;
    std::map<std::uint64_t,page_state> written_pages_;
    std::set<std::uint64_t> logged_pcs_;
    window_state window_;
    std::uint64_t start_index_=0,start_pc_=0,event_rdi_=0;
    std::uint64_t previous_pc_=0;
    std::uint32_t previous_size_=0;
    std::uint64_t instructions_=0, completed_windows_=0;
    std::uint64_t image_writes_=0,external_writes_=0;
    std::uint64_t exact_overlap_events_=0, first_outside_pc_=0;
    std::uint64_t outside_themida_instructions_=0;
    std::uint64_t truncated_write_events_=0,dropped_page_events_=0;
    bool enabled_=false,active_=false;
};
} // namespace muflex
#endif // MUFLEX_RUNTIME_WRITTEN_CODE_HPP
