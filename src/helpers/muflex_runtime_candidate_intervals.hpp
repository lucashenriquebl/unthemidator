#ifndef MUFLEX_RUNTIME_CANDIDATE_INTERVALS_HPP
#define MUFLEX_RUNTIME_CANDIDATE_INTERVALS_HPP

// V27: bounded, passive Unicorn observation from each matching 'L' comparison
// to the next candidate comparison. Only reads Unicorn state; never patches it.
#include <unicorn/unicorn.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace muflex {

class runtime_candidate_intervals final {
public:
    static constexpr std::uint64_t k_cmp_rva = 0xA3B0FC1ULL;
    static constexpr std::uint64_t k_themida_begin = 0xA34F000ULL;
    static constexpr std::uint64_t k_themida_end = 0xAF07000ULL;
    static constexpr std::uint64_t k_max_instructions = 131072ULL;
    static constexpr std::size_t k_max_segments = 32;
    static constexpr std::size_t k_max_pcs = 4096;

    runtime_candidate_intervals(std::filesystem::path folder,
                                std::uint64_t base, std::uint64_t image_size)
        : folder_(std::move(folder)), base_(base), image_size_(image_size) {}

    bool begin() {
        intervals_.open(folder_/"v27_candidate_intervals.csv", std::ios::trunc);
        checkpoints_.open(folder_/"v27_candidate_checkpoints.csv", std::ios::trunc);
        top_.open(folder_/"v27_candidate_top_pcs.csv", std::ios::trunc);
        flags_.open(folder_/"v27_candidate_flags.txt", std::ios::trunc);
        if (!intervals_ || !checkpoints_ || !top_ || !flags_) return false;
        intervals_ << "id,name,start_instruction,last_instruction,insns,termination,"
                       "next_compare_pc,nonsequential,distinct_pcs,untracked_pc_hits,"
                       "guest_image_writes,guest_external_writes,first_outside_themida,"
                       "first_pc,last_pc,start_rdi,end_rax,end_rcx,end_rdx,end_rdi,end_rsp\n";
        checkpoints_ << "id,name,insn_index,offset,pc,size,rax,rcx,rdx,rdi,rsi,rsp,rflags\n";
        top_ << "id,name,pc,hits\n";
        enabled_=true;
        return true;
    }

    void observe(uc_engine* uc, std::uint64_t idx,
                 std::uint64_t pc, std::uint32_t size) {
        if (!enabled_) return;

        // Close before including the next comparison in the previous segment.
        // This includes mismatches after the final observed 'L' candidate.
        if (active_ && pc == base_ + k_cmp_rva && idx != active_->start) {
            complete(uc, "next_cmp", pc);
        }

        if (active_) {
            add(uc, idx, pc, size);
            if (active_ && active_->insns >= k_max_instructions)
                complete(uc, "instruction_cap", 0);
        }

        if (pc != base_ + k_cmp_rva) return;
        const auto rax = reg(uc, UC_X86_REG_RAX);
        const auto rdi = reg(uc, UC_X86_REG_RDI);
        const auto ef = reg(uc, UC_X86_REG_EFLAGS);
        std::uint8_t first=0;
        if (!(ef & (1ULL<<6)) || (rax & 0xffULL) != 'L' ||
            uc_mem_read(uc,rdi,&first,1) != UC_ERR_OK || first != 'L') return;
        auto name=read_name(uc,rdi);
        if (!name) {
            ++name_errors_;
            return;
        }
        ++matches_;
        if (matches_ > k_max_segments) {
            ++dropped_segments_;
            return;
        }
        active_.emplace();
        active_->id=matches_;
        active_->name=std::move(*name);
        active_->start=idx;
        active_->start_rdi=rdi;
        add(uc,idx,pc,size); // the matching comparison belongs to this segment
    }

    void observe_write(std::uint64_t dest) {
        if (!enabled_ || !active_) return;
        if (dest >= base_ && dest-base_ < image_size_)
            ++active_->image_writes;
        else
            ++active_->external_writes;
    }

    void finish() {
        if (!enabled_) return;
        if (active_) complete(nullptr,"probe_end",0);
        flags_ << "matches=" << matches_ << '\n'
               << "segments_completed=" << completed_ << '\n'
               << "segments_dropped=" << dropped_segments_ << '\n'
               << "invalid_names=" << name_errors_ << '\n'
               << "max_instructions_per_segment=" << k_max_instructions << '\n'
               << "mode=PASSIVE_UNICORN_OBSERVATION_ONLY\n"
               << "warning=An observed name is not evidence of a resolved or called API.\n";
        intervals_.flush();
        checkpoints_.flush();
        top_.flush();
        flags_.flush();
        enabled_=false;
    }

    ~runtime_candidate_intervals() { finish(); }
    [[nodiscard]] bool enabled() const { return enabled_; }

private:
    struct segment {
        std::uint64_t id=0,start=0,last_index=0,start_rdi=0;
        std::uint64_t insns=0,nonseq=0,image_writes=0,external_writes=0;
        std::uint64_t untracked=0,first_pc=0,last_pc=0;
        std::uint32_t last_size=0;
        bool first_outside=false;
        std::string name;
        std::map<std::uint64_t,std::uint64_t> pcs;
    };

    static std::uint64_t reg(uc_engine* uc,int which) {
        std::uint64_t v=0;
        if (uc) (void) uc_reg_read(uc,which,&v);
        return v;
    }
    static std::string hx(std::uint64_t n) {
        std::ostringstream x;
        x << "0x" << std::hex << n;
        return x.str();
    }
    static std::optional<std::string> read_name(uc_engine* uc, std::uint64_t addr) {
        std::string value;
        for (unsigned i=0;i<96;++i) {
            std::uint8_t c=0;
            if (uc_mem_read(uc,addr+i,&c,1)!=UC_ERR_OK) return std::nullopt;
            if (c==0) return value.empty()?std::nullopt:std::optional<std::string>(value);
            const bool safe=(c>='A' && c<='Z') || (c>='a' && c<='z') ||
                            (c>='0' && c<='9') || c=='_' || c=='@' || c=='.';
            if (!safe) return std::nullopt;
            value.push_back(static_cast<char>(c));
        }
        return std::nullopt;
    }

    static bool checkpoint(std::uint64_t n) {
        return n<=128 && (n&(n-1))==0 || n>128 && n%1024==0;
    }
    void sample(uc_engine* uc,const segment& s, std::uint64_t idx,
                std::uint64_t pc,std::uint32_t size) {
        if (!uc) return;
        checkpoints_ << s.id << ',' << s.name << ',' << idx << ',' << s.insns
                     << ',' << hx(pc) << ',' << size;
        for (const int r : {UC_X86_REG_RAX,UC_X86_REG_RCX,UC_X86_REG_RDX,
             UC_X86_REG_RDI,UC_X86_REG_RSI,UC_X86_REG_RSP,UC_X86_REG_EFLAGS})
            checkpoints_ << ',' << hx(reg(uc,r));
        checkpoints_ << '\n';
    }
    void add(uc_engine* uc, std::uint64_t idx,std::uint64_t pc,std::uint32_t size) {
        if (!active_) return;
        auto& s=*active_;
        if (s.insns && s.last_pc + s.last_size != pc) ++s.nonseq;
        if (!s.insns) s.first_pc=pc;
        s.last_index=idx;
        s.last_pc=pc;
        s.last_size=size;
        ++s.insns;
        if (pc>=base_ && pc-base_<image_size_) {
            const auto rva=pc-base_;
            if (rva<k_themida_begin || rva>=k_themida_end) s.first_outside=true;
        } else {
            s.first_outside=true;
        }
        const auto it=s.pcs.find(pc);
        if (it!=s.pcs.end()) ++it->second;
        else if (s.pcs.size()<k_max_pcs) s.pcs.emplace(pc,1);
        else ++s.untracked;
        if (checkpoint(s.insns)) sample(uc,s,idx,pc,size);
    }
    void complete(uc_engine* uc, const char* reason, std::uint64_t next_cmp_pc) {
        if (!active_) return;
        auto& s=*active_;
        if (uc) sample(uc,s,s.last_index,s.last_pc,s.last_size); // final endpoint
        intervals_ << s.id << ',' << s.name << ',' << s.start << ',' << s.last_index
                   << ',' << s.insns << ',' << reason << ','
                   << (next_cmp_pc ? hx(next_cmp_pc) : "") << ','
                   << s.nonseq << ',' << s.pcs.size() << ',' << s.untracked << ','
                   << s.image_writes << ',' << s.external_writes << ','
                   << (s.first_outside?1:0) << ',' << hx(s.first_pc)
                   << ',' << hx(s.last_pc) << ',' << hx(s.start_rdi)
                   << ',' << hx(reg(uc,UC_X86_REG_RAX))
                   << ',' << hx(reg(uc,UC_X86_REG_RCX))
                   << ',' << hx(reg(uc,UC_X86_REG_RDX))
                   << ',' << hx(reg(uc,UC_X86_REG_RDI))
                   << ',' << hx(reg(uc,UC_X86_REG_RSP)) << '\n';
        std::vector<std::pair<std::uint64_t,std::uint64_t>> peaks(s.pcs.begin(),s.pcs.end());
        std::sort(peaks.begin(),peaks.end(),[](const auto& a,const auto& b){
            return a.second!=b.second?a.second>b.second:a.first<b.first;
        });
        for (std::size_t k=0; k<peaks.size() && k<12;++k)
            top_ << s.id << ',' << s.name << ',' << hx(peaks[k].first)
                 << ',' << peaks[k].second << '\n';
        ++completed_;
        active_.reset();
    }
    std::filesystem::path folder_;
    std::uint64_t base_=0,image_size_=0,matches_=0,completed_=0;
    std::uint64_t dropped_segments_=0,name_errors_=0;
    std::optional<segment> active_;
    std::ofstream intervals_,checkpoints_,top_,flags_;
    bool enabled_=false;
};
} // namespace muflex
