#pragma once

// Bounded host diagnostic planning; no device address is dereferenced by these helpers.
#include <array>
#include <cstdint>
#include <string>
#include <sstream>
#include <limits>

namespace Libs::Graphics {

inline constexpr uint64_t BdaResidentPageBits = 14;
inline constexpr uint64_t BdaResidentPageBytes = uint64_t{1} << BdaResidentPageBits;
inline constexpr uint64_t BdaResidentLowerBytes = uint64_t{1} << 40;
inline constexpr uint64_t BdaResidentExtendedBase = 0x080000000000ull;
inline constexpr uint64_t BdaResidentExtendedBytes = uint64_t{512} << 30;
inline constexpr uint64_t BdaResidentTableBytes = ((BdaResidentLowerBytes + BdaResidentExtendedBytes) >> BdaResidentPageBits) * 8;
inline constexpr uint64_t BdaResidentMaximumNodeBytes = 128;

enum class BdaResidentPlanStatus {
    InvalidRange, TooLarge, Misaligned, CrossesPage, TableOutOfBounds,
    TableOnlyMissingOwner, TableOnlyStaleOwner, TableOnlyOwnerNotCovering,
    TableOnlyOwnerAlignment, TableOnlyDeviceAddressOverflow, Ready
};

struct BdaResidentOwner {
    bool found = false;
    bool allocated = false;
    bool deleted = false;
    uint32_t index = std::numeric_limits<uint32_t>::max();
    uint32_t generation = 0;
    uint64_t guest_base = 0;
    uint64_t size = 0;
    uint64_t device_base = 0;
};

struct BdaResidentPlan {
    BdaResidentPlanStatus status = BdaResidentPlanStatus::InvalidRange;
    bool table_copy_allowed = false;
    bool owner_copy_allowed = false;
    uint64_t guest_address = 0;
    uint64_t node_bytes = 0;
    uint64_t guest_page = 0;
    uint64_t packed_page = 0;
    uint64_t table_offset = 0;
    uint64_t in_page_offset = 0;
    uint64_t owner_offset = 0;
    uint64_t expected_page_bda = 0;
    BdaResidentOwner owner{};
};

inline constexpr bool BdaResidentValidGuestRange(uint64_t address, uint64_t size) {
    if (address == 0 || size == 0) return false;
    if (address < BdaResidentLowerBytes) return size <= BdaResidentLowerBytes - address;
    return address >= BdaResidentExtendedBase && address - BdaResidentExtendedBase < BdaResidentExtendedBytes &&
           size <= BdaResidentExtendedBytes - (address - BdaResidentExtendedBase);
}

inline constexpr BdaResidentPlan BuildBdaResidentPlan(uint64_t address, uint64_t size,
                                uint64_t actual_table_bytes, BdaResidentOwner owner) {
    BdaResidentPlan result{};
    result.guest_address = address;
    result.node_bytes = size;
    result.owner = owner;
    if (!BdaResidentValidGuestRange(address, size)) return result;
    if (size > BdaResidentMaximumNodeBytes) { result.status = BdaResidentPlanStatus::TooLarge; return result; }
    if ((address & 3u) != 0 || (size & 3u) != 0) {
        result.status = BdaResidentPlanStatus::Misaligned; return result;
    }
    const uint64_t page_base = address & ~(BdaResidentPageBytes - 1);
    result.guest_page = address >> BdaResidentPageBits;
    result.in_page_offset = address - page_base;
    if (size > BdaResidentPageBytes - result.in_page_offset) {
        result.status = BdaResidentPlanStatus::CrossesPage; return result;
    }
    const uint64_t packed = address < BdaResidentLowerBytes ? address
        : address - BdaResidentExtendedBase + BdaResidentLowerBytes;
    result.packed_page = packed >> BdaResidentPageBits;
    result.table_offset = result.packed_page * 8;
    if (result.table_offset > actual_table_bytes ||
        8 > actual_table_bytes - result.table_offset) {
        result.status = BdaResidentPlanStatus::TableOutOfBounds; return result;
    }
    result.table_copy_allowed = true;
    if (!owner.found) { result.status = BdaResidentPlanStatus::TableOnlyMissingOwner; return result; }
    if (!owner.allocated || owner.deleted || owner.index == std::numeric_limits<uint32_t>::max() ||
        owner.generation == 0 || owner.device_base == 0) {
        result.status = BdaResidentPlanStatus::TableOnlyStaleOwner; return result;
    }
    if (address < owner.guest_base || size > owner.size ||
        address - owner.guest_base > owner.size - size) {
        result.status = BdaResidentPlanStatus::TableOnlyOwnerNotCovering; return result;
    }
    if ((owner.guest_base & (BdaResidentPageBytes - 1)) != 0 ||
        (owner.size & (BdaResidentPageBytes - 1)) != 0 || page_base < owner.guest_base) {
        result.status = BdaResidentPlanStatus::TableOnlyOwnerAlignment; return result;
    }
    const uint64_t page_offset = page_base - owner.guest_base;
    result.owner_offset = address - owner.guest_base;
    if (page_offset > std::numeric_limits<uint64_t>::max() - owner.device_base ||
        result.owner_offset > std::numeric_limits<uint64_t>::max() - owner.device_base ||
        size - 1 > std::numeric_limits<uint64_t>::max() - (owner.device_base + result.owner_offset)) {
        result.status = BdaResidentPlanStatus::TableOnlyDeviceAddressOverflow; return result;
    }
    result.expected_page_bda = owner.device_base + page_offset;
    result.owner_copy_allowed = true;
    result.status = BdaResidentPlanStatus::Ready;
    return result;
}

inline constexpr bool BdaResidentPageMappingMatches(const BdaResidentPlan& plan, uint64_t captured_table_entry) {
    return plan.owner_copy_allowed && captured_table_entry != 0 &&
           captured_table_entry == plan.expected_page_bda;
}

inline constexpr bool BdaResidentSameLiveOwner(const BdaResidentOwner& before, const BdaResidentOwner& after) {
    return before.found && after.found && before.allocated && after.allocated &&
           !before.deleted && !after.deleted && before.index == after.index &&
           before.generation == after.generation && before.generation != 0 &&
           before.guest_base == after.guest_base && before.size == after.size &&
           before.device_base == after.device_base && before.device_base != 0;
}

enum class BdaResidentAssessment {
    QueryRejected, TableReadFailed, NoReadableOwner, OwnerReadFailed,
    OwnerChanged, GpuEntryZero, MappingMismatch, Matched,
    DispatchNotComplete, CopyWaitNotComplete
};

inline constexpr BdaResidentAssessment AssessBdaResidentMapping(const BdaResidentPlan& plan, bool table_read_succeeded,
                                   bool owner_read_succeeded, uint64_t table_entry,
                                   const BdaResidentOwner& owner_after) {
    if (!plan.table_copy_allowed) return BdaResidentAssessment::QueryRejected;
    if (!table_read_succeeded) return BdaResidentAssessment::TableReadFailed;
    if (!plan.owner_copy_allowed) return BdaResidentAssessment::NoReadableOwner;
    if (!owner_read_succeeded) return BdaResidentAssessment::OwnerReadFailed;
    if (!BdaResidentSameLiveOwner(plan.owner, owner_after)) return BdaResidentAssessment::OwnerChanged;
    if (table_entry == 0) return BdaResidentAssessment::GpuEntryZero;
    return BdaResidentPageMappingMatches(plan, table_entry) ? BdaResidentAssessment::Matched : BdaResidentAssessment::MappingMismatch;
}

struct BdaResidentCapture {
    BdaResidentPlan plan{};
    BdaResidentOwner owner_after{};
    uint64_t dispatch_completed_tick = 0;
    uint64_t copy_wait_tick = 0;
    uint64_t requested_source_bytes = 0;
    bool dispatch_tick_confirmed = false;
    bool copy_wait_completed = false;
    bool table_copy_attempted = false;
    bool table_copied = false;
    bool owner_copy_attempted = false;
    bool owner_copied = false;
    bool owner_stable_after_wait = false;
    bool page_mapping_matches = false;
    uint64_t table_entry = 0;
    bool gpu_dirty_before = false;
    bool cpu_modified_before = false;
    std::array<uint8_t, 8> table_bytes{};
    std::array<uint8_t, 128> owner_bytes{};
    std::string error;
};

inline constexpr const char* BdaResidentPlanStatusName(BdaResidentPlanStatus status) {
    switch (status) {
    case BdaResidentPlanStatus::InvalidRange: return "invalid_range";
    case BdaResidentPlanStatus::TooLarge: return "too_large";
    case BdaResidentPlanStatus::Misaligned: return "misaligned";
    case BdaResidentPlanStatus::CrossesPage: return "crosses_page";
    case BdaResidentPlanStatus::TableOutOfBounds: return "table_out_of_bounds";
    case BdaResidentPlanStatus::TableOnlyMissingOwner: return "table_only_missing_owner";
    case BdaResidentPlanStatus::TableOnlyStaleOwner: return "table_only_stale_owner";
    case BdaResidentPlanStatus::TableOnlyOwnerNotCovering: return "table_only_owner_not_covering";
    case BdaResidentPlanStatus::TableOnlyOwnerAlignment: return "table_only_owner_alignment";
    case BdaResidentPlanStatus::TableOnlyDeviceAddressOverflow: return "table_only_device_address_overflow";
    case BdaResidentPlanStatus::Ready: return "ready";
    }
    return "unknown";
}

inline constexpr const char* BdaResidentAssessmentName(BdaResidentAssessment status) {
    switch (status) {
    case BdaResidentAssessment::QueryRejected: return "query_rejected";
    case BdaResidentAssessment::TableReadFailed: return "table_read_failed";
    case BdaResidentAssessment::NoReadableOwner: return "no_readable_owner";
    case BdaResidentAssessment::OwnerReadFailed: return "owner_read_failed";
    case BdaResidentAssessment::OwnerChanged: return "owner_changed";
    case BdaResidentAssessment::GpuEntryZero: return "gpu_entry_zero";
    case BdaResidentAssessment::MappingMismatch: return "mapping_mismatch";
    case BdaResidentAssessment::Matched: return "matched";
    case BdaResidentAssessment::DispatchNotComplete: return "dispatch_not_complete";
    case BdaResidentAssessment::CopyWaitNotComplete: return "copy_wait_not_complete";
    }
    return "unknown";
}

inline BdaResidentAssessment AssessBdaResidentCapture(const BdaResidentCapture& capture) {
    if (!capture.plan.table_copy_allowed) return BdaResidentAssessment::QueryRejected;
    if (!capture.dispatch_tick_confirmed) return BdaResidentAssessment::DispatchNotComplete;
    if (!capture.copy_wait_completed) return BdaResidentAssessment::CopyWaitNotComplete;
    const auto assessment = AssessBdaResidentMapping(capture.plan, capture.table_copied,
        capture.owner_copied, capture.table_entry, capture.owner_after);
    if (assessment == BdaResidentAssessment::Matched && !capture.owner_stable_after_wait)
        return BdaResidentAssessment::OwnerChanged;
    return assessment;
}

inline std::string Describe(const BdaResidentCapture& capture) {
    std::ostringstream text;
    text << std::boolalpha << "schema_version=1\n"
         << "scope=post_dispatch_resident_view_before_cpu_backing_read\n"
         << "event_time_mapping_proven=false\n"
         << "plan_status=" << BdaResidentPlanStatusName(capture.plan.status) << '\n'
         << "assessment=" << BdaResidentAssessmentName(AssessBdaResidentCapture(capture)) << '\n';
    const auto decimal = [&](const char* name, uint64_t value) {
        text << std::dec << name << '=' << value << '\n';
    };
    const auto hexadecimal = [&](const char* name, uint64_t value) {
        text << name << "=0x" << std::hex << value << std::dec << '\n';
    };
    const auto boolean = [&](const char* name, bool value) {
        text << name << '=' << value << '\n';
    };
    hexadecimal("guest_address", capture.plan.guest_address);
    decimal("node_bytes", capture.plan.node_bytes);
    hexadecimal("guest_page", capture.plan.guest_page);
    hexadecimal("packed_page", capture.plan.packed_page);
    hexadecimal("table_offset", capture.plan.table_offset);
    hexadecimal("in_page_offset", capture.plan.in_page_offset);
    hexadecimal("owner_offset", capture.plan.owner_offset);
    hexadecimal("expected_page_bda", capture.plan.expected_page_bda);
    decimal("dispatch_completed_tick", capture.dispatch_completed_tick);
    decimal("copy_wait_tick", capture.copy_wait_tick);
    decimal("requested_source_bytes", capture.requested_source_bytes);
    boolean("dispatch_tick_confirmed", capture.dispatch_tick_confirmed);
    boolean("copy_wait_completed", capture.copy_wait_completed);
    boolean("table_copy_allowed", capture.plan.table_copy_allowed);
    boolean("owner_copy_allowed", capture.plan.owner_copy_allowed);
    boolean("table_copy_attempted", capture.table_copy_attempted);
    boolean("table_copied", capture.table_copied);
    boolean("owner_copy_attempted", capture.owner_copy_attempted);
    boolean("owner_copied", capture.owner_copied);
    boolean("owner_stable_after_wait", capture.owner_stable_after_wait);
    boolean("page_mapping_matches", capture.page_mapping_matches);
    hexadecimal("table_entry", capture.table_entry);
    boolean("gpu_dirty_before", capture.gpu_dirty_before);
    boolean("cpu_modified_before", capture.cpu_modified_before);
    const auto owner = [&](const char* prefix, const BdaResidentOwner& value) {
        const std::string key(prefix);
        boolean((key + "_found").c_str(), value.found);
        boolean((key + "_allocated").c_str(), value.allocated);
        boolean((key + "_deleted").c_str(), value.deleted);
        decimal((key + "_index").c_str(), value.index);
        decimal((key + "_generation").c_str(), value.generation);
        hexadecimal((key + "_guest_base").c_str(), value.guest_base);
        decimal((key + "_size").c_str(), value.size);
        hexadecimal((key + "_device_base").c_str(), value.device_base);
    };
    owner("owner_before", capture.plan.owner);
    owner("owner_after", capture.owner_after);
    text << "error=" << capture.error << '\n';
    return text.str();
}

} // namespace Libs::Graphics
