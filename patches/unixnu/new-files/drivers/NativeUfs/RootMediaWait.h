// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Allow early APFS initialization; busy and read counters are diagnostic and never replace volume identity checks.
constexpr unsigned kUfsRootMediaWaitMs = 180000;
constexpr unsigned kUfsRootMediaPollMs = 100;
template <class Ops>
static bool ufsWaitForRootMedia (Ops &ops)
{
  for (unsigned elapsed = 0; ; elapsed += kUfsRootMediaPollMs) {
    if (ops.ready ()) return true;
    if (elapsed % 10000 == 0) ops.progress (elapsed);
    if (elapsed == kUfsRootMediaWaitMs) return false;
    ops.pause (kUfsRootMediaPollMs);
  }
}
