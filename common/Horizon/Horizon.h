// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#define u128 nx_u128
#define s128 nx_s128
#include <switch.h>
#undef u128
#undef s128

namespace Horizon
{
	u64 GetProcessCoreMask();
	u32 GetProcessCoreCount();
	void ReserveCore3ForCallingThread();
	bool PinCallingThreadToCore3();

	// Whether the CPU clock is raised to its highest rate while the app runs. Stored as an int in
	// [Horizon] PerformanceMode: 0 off, anything else on.
	static constexpr bool DefaultCpuBoost = true;
	static constexpr const char* PerformanceModeSection = "Horizon";
	static constexpr const char* PerformanceModeKey = "PerformanceMode";

	void InitializeClocks();
	// Raises the CPU clock to its highest rate, or gives it back to the system. Safe from any thread.
	void SetCpuBoost(bool enabled);
	// Re-applies the boost, for after the system has changed the clocks (resume, dock/undock).
	void RefreshCpuBoost();
	void ShutdownClocks();

	// Lazily brings up the BSD socket
	bool EnsureNetworkInitialized();
	void ShutdownNetwork();

	// Closes any executable (JIT) mapping still open at exit; returns how many. 0 off Switch 2.
	size_t ReleaseLeftoverCodeMappings();

	// Init logging
	void BreadcrumbInit(const char* report_dir);
	void Breadcrumb(const char* message);
	void BreadcrumbShutdown();
	void ExitTrace(const char* message);

	void AuditHeapToExitTrace(const char* when);
} // namespace Horizon
