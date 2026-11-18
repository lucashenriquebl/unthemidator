#ifndef MUFLEX_RUNTIME_EXPORT_NAME_PROBE_HPP
#define MUFLEX_RUNTIME_EXPORT_NAME_PROBE_HPP

// V25: read-only sampling of the candidate ASCII export name pointed to by
// RDI at the verified CMP AL,[RDI] branch inside a dummy DLL. No guest writes.

#include <unicorn/unicorn.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace muflex {

class runtime_export_name_probe final {
public:
    static constexpr std::size_t k_max_events = 2048;
    static constexpr std::size_t k_max_name_bytes = 96;

    explicit runtime_export_name_probe(std::filesystem::path output)
        : output_(std::move(output)) {
        entries_.reserve(1100);
    }

    void observe(uc_engine* uc, std::uint64_t instruction_index,
                 std::uint64_t rdi, std::uint8_t al, std::uint8_t mem_rdi) {
        ++total_observed_;
        if (al == mem_rdi) ++first_byte_equal_;
        if (have_previous_ && rdi <= previous_rdi_) ++nonincreasing_rdi_;
        have_previous_ = true;
        previous_rdi_ = rdi;

        if (entries_.size() >= k_max_events) {
            ++unrecorded_;
            return;
        }

        record row{};
        row.instruction_index = instruction_index;
        row.rdi = rdi;
        row.al = al;
        row.first_byte = mem_rdi;
        row.first_byte_match = al == mem_rdi;
        for (std::size_t n = 0; n < k_max_name_bytes; ++n) {
            std::uint8_t byte = 0;
            if (uc_mem_read(uc, rdi + n, &byte, 1) != UC_ERR_OK) {
                row.status = "unreadable";
                break;
            }
            if (byte == 0) {
                row.status = "nul_terminated";
                break;
            }
            if (byte < 0x20 || byte > 0x7e) {
                row.status = "non_printable";
                break;
            }
            // Write ASCII safely to CSV (URL-escape punctuation and whitespace);
            // names are never executed/interpreted by this tool.
            if ((byte >= 'A' && byte <= 'Z') ||
                (byte >= 'a' && byte <= 'z') ||
                (byte >= '0' && byte <= '9') ||
                byte == '_' || byte == '@' || byte == '.' || byte == '-') {
                row.name.push_back(static_cast<char>(byte));
            } else {
                std::ostringstream escaped;
                escaped << '%' << std::uppercase << std::hex
                        << std::setw(2) << std::setfill('0')
                        << static_cast<unsigned>(byte);
                row.name += escaped.str();
            }
            ++row.bytes;
        }
        if (row.status.empty()) row.status = "length_limit";
        ++status_counts_[row.status];
        if (row.first_byte_match) ++captured_matches_;
        entries_.push_back(std::move(row));
    }

    void finish() const {
        std::ofstream csv(output_ / "runtime_export_candidate_names.csv", std::ios::trunc);
        if (csv) {
            csv << "instruction_index,rdi,al_decimal,first_byte_decimal,first_byte_match,"
                   "captured_bytes,termination_status,candidate_name_ascii_escaped\n";
            for (const auto& row : entries_) {
                csv << row.instruction_index << ",0x" << std::hex << row.rdi << std::dec
                    << ',' << static_cast<unsigned>(row.al)
                    << ',' << static_cast<unsigned>(row.first_byte)
                    << ',' << (row.first_byte_match ? "true" : "false")
                    << ',' << row.bytes << ',' << row.status
                    << ',' << row.name << '\n';
            }
        }
        std::ofstream summary(output_ / "runtime_export_candidate_summary.txt", std::ios::trunc);
        if (summary) {
            summary << "mode=READ_ONLY_CANDIDATE_EXPORT_NAME_OBSERVATION\n"
                    << "observed_cmp_events=" << total_observed_ << '\n'
                    << "first_byte_equal=" << first_byte_equal_ << '\n'
                    << "nonincreasing_rdi_events=" << nonincreasing_rdi_ << '\n'
                    << "captured_records=" << entries_.size() << '\n'
                    << "captured_matches=" << captured_matches_ << '\n'
                    << "unrecorded_events=" << unrecorded_ << '\n'
                    << "name_read_limit_bytes=" << k_max_name_bytes << '\n';
            for (const auto& [status,count] : status_counts_) {
                summary << "names_" << status << '=' << count << '\n';
            }
            summary << "warning=RDI is not independently proven to point at the beginning of a PE export name.\n"
                    << "warning=Observed memory belongs to the Unicorn emulation and may be an artificial DLL.\n"
                    << "warning=No executable or guest state is modified by this tracer.\n";
        }
    }

    [[nodiscard]] std::uint64_t observed() const { return total_observed_; }

private:
    struct record {
        std::uint64_t instruction_index = 0;
        std::uint64_t rdi = 0;
        std::uint8_t al = 0, first_byte = 0;
        bool first_byte_match = false;
        std::size_t bytes = 0;
        std::string status{};
        std::string name{};
    };

    std::filesystem::path output_;
    std::vector<record> entries_;
    std::map<std::string, std::uint64_t> status_counts_;
    std::uint64_t total_observed_ = 0;
    std::uint64_t first_byte_equal_ = 0;
    std::uint64_t captured_matches_ = 0;
    std::uint64_t nonincreasing_rdi_ = 0;
    std::uint64_t unrecorded_ = 0;
    std::uint64_t previous_rdi_ = 0;
    bool have_previous_ = false;
};

}  // namespace muflex

#endif // MUFLEX_RUNTIME_EXPORT_NAME_PROBE_HPP
