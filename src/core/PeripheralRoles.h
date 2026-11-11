#pragma once

#include "core/SvdTypes.h"

namespace mrd {

/// Small "what kind of peripheral is this" helpers.
///
/// They live in core (not in the UI) so the self-test can assert the naming
/// rules against the real SVD, and so the tree's live column stays a thin view.
namespace PeripheralRoles {

/// True for ATIM / BTIM / GTIM / TIM / LPTIM / SHRTIM style timers.
bool isTimer(const SvdPeripheral &peripheral);

/// The live counter register ("TIMx_CNT", "LPTIM_CNT"), or nullptr.
///
/// "TIMx_REPCNT" is a repetition counter, not the running count, so it is
/// explicitly skipped. Timers without a readable counter (watchdogs) yield
/// nullptr and simply get no live value.
const SvdRegister *counterRegister(const SvdPeripheral &peripheral);

} // namespace PeripheralRoles

} // namespace mrd
