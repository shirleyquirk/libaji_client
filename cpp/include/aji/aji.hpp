// C++23 RAII wrapper for libaji_client (aji.h). Everything in one include.
//
//   management.hpp   aji::scan, aji::find*, server and device-database functions
//   chain.hpp        aji::Chain            a cable/chain; lock() it
//   chain_lock.hpp   aji::ChainLock        the chain's lock (move-only); hand_to() an open
//   open.hpp         aji::Open<K>          an opened TAP, whole chain, SLD node or hub
//   session.hpp      aji::Session<K>       an open's lock (move-only): the scans; hand_back()
//   pending.hpp      aji::Pending<T>       a scan result that may not have arrived yet
//   types.hpp        value types (HardwareInfo, TapInfo, SldNodeInfo, ClaimSet, Bits, ...)
//   error.hpp        aji::Error, aji::Errc, aji::expected<T>
//
// Design and the library behaviour behind it: docs/cpp-wrapper/DESIGN.md and
// docs/cpp-wrapper/FINDINGS.md.
#pragma once

#include "aji/chain.hpp"
#include "aji/chain_lock.hpp"
#include "aji/error.hpp"
#include "aji/management.hpp"
#include "aji/open.hpp"
#include "aji/pending.hpp"
#include "aji/session.hpp"
#include "aji/types.hpp"
