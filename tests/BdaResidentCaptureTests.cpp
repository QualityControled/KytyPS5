// Pure bounded resident-mapping and metadata tests; no GPU or emulator execution.
#include "graphics/host_gpu/renderer/cache/bdaResidentCapture.h"
#include <cstdlib>
#include <iostream>

using namespace Libs::Graphics;

static void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    const BdaResidentOwner owner{true, true, false, 23, 7, 0x143bc90000ull, 0x8000, 0x100000000ull};
    const auto actual = BuildBdaResidentPlan(0x143bc94100ull, 128, BdaResidentTableBytes, owner);
    Check(actual.status == BdaResidentPlanStatus::Ready && actual.guest_page == 0x50ef25 &&
          actual.packed_page == 0x50ef25 && actual.table_offset == 0x2877928 &&
          actual.in_page_offset == 0x100 && actual.owner_offset == 0x4100 &&
          actual.expected_page_bda == 0x100004000ull, "actual-family page math");
    Check(BdaResidentPageMappingMatches(actual, 0x100004000ull) &&
          !BdaResidentPageMappingMatches(actual, 0) && !BdaResidentPageMappingMatches(actual, 0x100000000ull),
          "zero or mismatched GPU entry must not count as resident proof");
    const auto extended = BuildBdaResidentPlan(BdaResidentExtendedBase + 0x100, 128, BdaResidentTableBytes,
        {true, true, false, 9, 1, BdaResidentExtendedBase, BdaResidentPageBytes, 0x200000000ull});
    Check(extended.status == BdaResidentPlanStatus::Ready && extended.packed_page == (BdaResidentLowerBytes >> BdaResidentPageBits) &&
          extended.table_offset == (BdaResidentLowerBytes >> BdaResidentPageBits) * 8 && extended.in_page_offset == 0x100,
          "extended packing must agree with emitter mapping");
    Check(!BuildBdaResidentPlan(BdaResidentLowerBytes, 4, BdaResidentTableBytes, owner).table_copy_allowed &&
          !BuildBdaResidentPlan(BdaResidentExtendedBase - 4, 4, BdaResidentTableBytes, owner).table_copy_allowed &&
          !BuildBdaResidentPlan(BdaResidentExtendedBase + BdaResidentExtendedBytes, 4, BdaResidentTableBytes, owner).table_copy_allowed &&
          !BuildBdaResidentPlan(UINT64_MAX - 3, 8, BdaResidentTableBytes, owner).table_copy_allowed &&
          !BuildBdaResidentPlan(0, 4, BdaResidentTableBytes, owner).table_copy_allowed, "invalid ranges");
    Check(BuildBdaResidentPlan(0x143bc94100ull, 132, BdaResidentTableBytes, owner).status == BdaResidentPlanStatus::TooLarge,
          "strict128B cap");
    Check(BuildBdaResidentPlan(0x143bc94101ull, 128, BdaResidentTableBytes, owner).status == BdaResidentPlanStatus::Misaligned &&
          BuildBdaResidentPlan(0x143bc94100ull, 127, BdaResidentTableBytes, owner).status == BdaResidentPlanStatus::Misaligned,
          "alignment");
    Check(BuildBdaResidentPlan(0x143bc97fc0ull, 128, BdaResidentTableBytes, owner).status == BdaResidentPlanStatus::CrossesPage,
          "minimal single-entry diagnostic must reject cross-page128B");
    Check(BuildBdaResidentPlan(0x143bc94100ull, 128, actual.table_offset + 7, owner).status == BdaResidentPlanStatus::TableOutOfBounds,
          "table extent must include all8bytes");
    const auto missing = BuildBdaResidentPlan(0x143bc94100ull, 128, BdaResidentTableBytes, {});
    Check(missing.status == BdaResidentPlanStatus::TableOnlyMissingOwner && missing.table_copy_allowed &&
          !missing.owner_copy_allowed, "missing owner may copy table only, never create cache owner");
    auto stale_owner = owner; stale_owner.deleted = true;
    Check(BuildBdaResidentPlan(0x143bc94100ull, 128, BdaResidentTableBytes, stale_owner).status == BdaResidentPlanStatus::TableOnlyStaleOwner,
          "deleted owner");
    auto short_owner = owner; short_owner.size = 0x4000;
    Check(BuildBdaResidentPlan(0x143bc94100ull, 128, BdaResidentTableBytes, short_owner).status == BdaResidentPlanStatus::TableOnlyOwnerNotCovering,
          "owner must cover the full payload");
    auto unaligned_owner = owner; unaligned_owner.guest_base += 4;
    Check(BuildBdaResidentPlan(0x143bc94100ull, 128, BdaResidentTableBytes, unaligned_owner).status == BdaResidentPlanStatus::TableOnlyOwnerAlignment,
          "registered owner page alignment");
    auto overflow_owner = owner; overflow_owner.device_base = UINT64_MAX - 0x40ff;
    Check(BuildBdaResidentPlan(0x143bc94100ull, 128, BdaResidentTableBytes, overflow_owner).status == BdaResidentPlanStatus::TableOnlyDeviceAddressOverflow,
          "device address addition cannot wrap");
    const BdaResidentOwner latest_owner{true, true, false, 31, 9, 0x125cab0000ull, 0x8000, 0x300000000ull};
    const auto latest = BuildBdaResidentPlan(0x125cab4100ull, 128, BdaResidentTableBytes, latest_owner);
    Check(latest.status == BdaResidentPlanStatus::Ready && latest.guest_page == 0x4972ad &&
          latest.packed_page == 0x4972ad && latest.table_offset == 0x24b9568 &&
          latest.in_page_offset == 0x100 && latest.owner_offset == 0x4100 &&
          latest.expected_page_bda == 0x300004000ull,
          "actual856 GT7 shader input node-family math (synthetic owner metadata)");
    Check(AssessBdaResidentMapping(latest, false, true, latest.expected_page_bda, latest_owner) == BdaResidentAssessment::TableReadFailed &&
          AssessBdaResidentMapping(latest, true, false, latest.expected_page_bda, latest_owner) == BdaResidentAssessment::OwnerReadFailed,
          "read failures never become mapping proof, even when stale payload appears to match");
    Check(AssessBdaResidentMapping(latest, true, true, 0, latest_owner) == BdaResidentAssessment::GpuEntryZero &&
          AssessBdaResidentMapping(missing, true, false, 0, {}) == BdaResidentAssessment::NoReadableOwner &&
          AssessBdaResidentMapping(missing, true, true, latest.expected_page_bda, latest_owner) == BdaResidentAssessment::NoReadableOwner,
          "zero GPU entry or no owner cannot be repaired by a synthetic successful-copy flag");
    auto new_generation = latest_owner; ++new_generation.generation;
    auto moved_owner = latest_owner; moved_owner.device_base += 0x4000;
    Check(AssessBdaResidentMapping(latest, true, true, latest.expected_page_bda, new_generation) == BdaResidentAssessment::OwnerChanged &&
          AssessBdaResidentMapping(latest, true, true, latest.expected_page_bda, moved_owner) == BdaResidentAssessment::OwnerChanged &&
          AssessBdaResidentMapping(latest, true, true, latest.expected_page_bda, latest_owner) == BdaResidentAssessment::Matched &&
          AssessBdaResidentMapping(latest, true, true, latest.expected_page_bda + 4, latest_owner) == BdaResidentAssessment::MappingMismatch,
          "full generation/metadata stability and exact page-entry equality required");
    BdaResidentCapture captured;
    captured.plan = latest;
    captured.owner_after = latest_owner;
    captured.dispatch_completed_tick = 12;
    captured.copy_wait_tick = 13;
    captured.dispatch_tick_confirmed = true;
    captured.copy_wait_completed = true;
    captured.requested_source_bytes = 136;
    captured.table_copy_attempted = captured.table_copied = true;
    captured.owner_copy_attempted = captured.owner_copied = true;
    captured.owner_stable_after_wait = true;
    captured.table_entry = latest.expected_page_bda;
    captured.page_mapping_matches = true;
    const auto report = Describe(captured);
    Check(report.find("assessment=matched\n") != std::string::npos &&
          report.find("guest_address=0x125cab4100\n") != std::string::npos &&
          report.find("owner_before_generation=9\n") != std::string::npos &&
          report.find("owner_after_generation=9\n") != std::string::npos &&
          report.find("requested_source_bytes=136\n") != std::string::npos &&
          report.find("event_time_mapping_proven=false\n") != std::string::npos,
          "complete metadata must preserve actual-address math and confidence boundary");
    captured.dispatch_tick_confirmed = false;
    Check(AssessBdaResidentCapture(captured) == BdaResidentAssessment::DispatchNotComplete,
          "matching bytes cannot prove a dispatch that was not confirmed complete");
    captured.dispatch_tick_confirmed = true;
    captured.copy_wait_completed = false;
    Check(AssessBdaResidentCapture(captured) == BdaResidentAssessment::CopyWaitNotComplete,
          "matching bytes cannot prove a copy wait that did not finish");
    captured.copy_wait_completed = true;
    captured.owner_stable_after_wait = false;
    Check(AssessBdaResidentCapture(captured) == BdaResidentAssessment::OwnerChanged,
          "matching metadata cannot override an explicitly false owner stability check");
    captured.owner_stable_after_wait = true;
    captured.owner_after = new_generation;
    Check(Describe(captured).find("assessment=owner_changed\n") != std::string::npos,
          "post-wait generation mismatch must retain a stable outcome name");
    captured.table_copied = false;
    Check(Describe(captured).find("assessment=table_read_failed\n") != std::string::npos,
          "failed copy cannot be promoted by stale matching table bytes");
    captured.plan = missing;
    captured.table_copied = true;
    captured.owner_copied = false;
    Check(Describe(captured).find("assessment=no_readable_owner\n") != std::string::npos,
          "missing owner must remain distinct from zero-entry proof");
    std::cout << "ProductionBdaResidentPlanTests24groups PASS\n";

}
