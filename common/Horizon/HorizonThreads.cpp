// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Threading.h"

#include <algorithm>
#include <bit>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "common/Horizon/Horizon.h"
#include "common/Horizon/HorizonProfiling.h"

namespace
{
	std::mutex s_thread_handle_map_mutex;
	std::unordered_map<void*, Handle> s_thread_handle_map;

	u64 GetThreadCPUTimeUS(void* native_handle)
	{
		if (!native_handle)
			return 0;

		Handle handle;
		if (static_cast<void*>(pthread_self()) == native_handle)
		{
			handle = CUR_THREAD_HANDLE;
		}
		else
		{
			std::lock_guard lock(s_thread_handle_map_mutex);
			const auto it = s_thread_handle_map.find(native_handle);
			if (it == s_thread_handle_map.end())
				return 0;
			handle = it->second;
		}

		u64 ticks = 0;
		if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)) &&
			R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, handle, TickCountInfo_Total)))
		{
			return 0;
		}
		return armTicksToNs(ticks) / 1000;
	}
} // namespace

u64 Horizon::GetProcessCoreMask()
{
	u64 mask = 0;
	return R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ? mask : 0;
}

u32 Horizon::GetProcessCoreCount()
{
	const u64 mask = GetProcessCoreMask();
	const u32 count = static_cast<u32>(std::popcount(mask));
	return count ? count : 4u;
}

void Horizon::ReserveCore3ForCallingThread()
{
#ifdef __SWITCH2__
	const u64 allowed = GetProcessCoreMask();
	// Keep general worker threads off the primary EE/VU1/GS cores (0, 1, 2) when upper cores exist,
	// preferring the highest core (Core 5 on Switch 2). Emulation threads override this via SetAffinity.
	const u64 worker_mask = (allowed & ~0x7ULL) ? (allowed & ~0x7ULL) : allowed;
	const s32 ideal_core = worker_mask ? (63 - __builtin_clzll(worker_mask)) : -1;
	if (worker_mask && R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal_core, static_cast<u32>(worker_mask))))
#else
	const u64 mask = GetProcessCoreMask() & 0x7;
	if (mask && R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, __builtin_ctzll(mask), mask)))
#endif
		Console.Warning("Switch: failed to restrict worker to application cores");
}

bool Horizon::PinCallingThreadToCore3()
{
#ifdef __SWITCH2__
	const u64 allowed = GetProcessCoreMask();
	if (allowed == 0)
		return false;
	const s32 highest_core = 63 - __builtin_clzll(allowed);
	return R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, highest_core, static_cast<u32>(1ULL << highest_core)));
#else
	return (GetProcessCoreMask() & 8) && R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, 8));
#endif
}

namespace
{
	std::mutex s_clk_mutex;
	ClkrstSession s_clk_cpu{};
	bool s_clk_ready = false;      // clkrst session open and rates known
	bool s_applet_boost = false;   // clkrst unavailable: boost requested through the applet instead
	bool s_boost_wanted = Horizon::DefaultCpuBoost;
	bool s_boosted = false;
	u32 s_clk_cpu_max = 0;         // highest CPU rate offered
	u32 s_clk_cpu_baseline = 0;    // the system's own rate, restored when the boost is turned off

	constexpr u32 MHz(u32 hz) { return hz / 1000000; }
	constexpr u32 MHzTenth(u32 hz) { return (hz / 100000) % 10; }

	bool OpenClockSessionLocked()
	{
		Result rc = clkrstInitialize();
		if (R_FAILED(rc))
		{
			Console.Warning("Switch: clkrstInitialize failed (0x%08x)", rc);
			return false;
		}

		rc = clkrstOpenSession(&s_clk_cpu, PcvModuleId_CpuBus, 3);
		if (R_FAILED(rc))
		{
			Console.Warning("Switch: clkrstOpenSession(CpuBus) failed (0x%08x)", rc);
			clkrstExit();
			return false;
		}

		u32 rates[32] = {};
		PcvClockRatesListType type = PcvClockRatesListType_Invalid;
		s32 count = 0;
		u32 now = 0;
		const Result rc_now = clkrstGetClockRate(&s_clk_cpu, &now);
		const Result rc_rates = clkrstGetPossibleClockRates(&s_clk_cpu, rates, 32, &type, &count);
		if (R_FAILED(rc_now) || R_FAILED(rc_rates) || count <= 0)
		{
			Console.Warning("Switch: failed to query CPU clock rates (now=0x%08x, rates=0x%08x, count=%d)",
				rc_now, rc_rates, count);
			clkrstCloseSession(&s_clk_cpu);
			clkrstExit();
			return false;
		}

		s_clk_cpu_max = (type == PcvClockRatesListType_Range && count >= 2) ?
		                    rates[1] :
		                    *std::max_element(rates, rates + std::min<s32>(count, 32));
		s_clk_cpu_baseline = now;
		Console.WriteLn("Switch: CPU clock at %u.%01u MHz (max %u.%01u MHz)",
			MHz(now), MHzTenth(now), MHz(s_clk_cpu_max), MHzTenth(s_clk_cpu_max));
		return true;
	}

	void ApplyBoostLocked(bool boost)
	{
		if (!s_clk_ready)
		{
			// No clkrst: the applet's CPU boost is the only lever.
			if (boost && !s_applet_boost)
			{
				const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
				Console.WriteLn("Switch: clkrst unavailable, requested appletSetCpuBoostMode(FastLoad) (rc=0x%08x)", rc);
				s_applet_boost = R_SUCCEEDED(rc);
			}
			else if (!boost && s_applet_boost)
			{
				appletCancelCpuBoostMode();
				s_applet_boost = false;
			}
			return;
		}

		u32 now = 0;
		if (R_FAILED(clkrstGetClockRate(&s_clk_cpu, &now)))
			return;

		if (boost)
		{
			// Not boosted yet: what the system runs at now is what to go back to.
			if (!s_boosted)
				s_clk_cpu_baseline = now;
			if (now < s_clk_cpu_max)
			{
				const Result rc = clkrstSetClockRate(&s_clk_cpu, s_clk_cpu_max);
				u32 after = 0;
				clkrstGetClockRate(&s_clk_cpu, &after);
				Console.WriteLn(Color_StrongGreen, "Switch: CPU clock boosted %u.%01u MHz -> %u.%01u MHz (rc=0x%08x)",
					MHz(now), MHzTenth(now), MHz(after), MHzTenth(after), rc);
			}
			s_boosted = true;
		}
		else
		{
			if (s_boosted && s_clk_cpu_baseline != 0 && now != s_clk_cpu_baseline)
			{
				clkrstSetClockRate(&s_clk_cpu, s_clk_cpu_baseline);
				Console.WriteLn("Switch: CPU clock restored to %u.%01u MHz", MHz(s_clk_cpu_baseline), MHzTenth(s_clk_cpu_baseline));
			}
			s_boosted = false;
		}
	}
} // namespace

void Horizon::InitializeClocks()
{
	std::lock_guard lock(s_clk_mutex);
	if (!s_clk_ready)
		s_clk_ready = OpenClockSessionLocked();
	ApplyBoostLocked(s_boost_wanted);
}

void Horizon::SetCpuBoost(bool enabled)
{
	std::lock_guard lock(s_clk_mutex);
	if (enabled != s_boost_wanted)
		Console.WriteLn("Switch: CPU boost %s", enabled ? "on" : "off");
	s_boost_wanted = enabled;
	ApplyBoostLocked(enabled);
}

void Horizon::RefreshCpuBoost()
{
	std::lock_guard lock(s_clk_mutex);
	// A dock change or resume can move the clock: when not boosted, take the system's new rate as the baseline.
	if (s_clk_ready && !s_boosted)
		clkrstGetClockRate(&s_clk_cpu, &s_clk_cpu_baseline);
	ApplyBoostLocked(s_boost_wanted);
}

void Horizon::ShutdownClocks()
{
	std::lock_guard lock(s_clk_mutex);
	if (!s_clk_ready)
	{
		appletCancelCpuBoostMode();
		s_applet_boost = false;
		return;
	}

	ApplyBoostLocked(false);
	clkrstCloseSession(&s_clk_cpu);
	clkrstExit();
	s_clk_ready = false;
}

__forceinline void Threading::Timeslice()
{
	sched_yield();
}

__forceinline void Threading::SpinWait()
{
	__asm__ __volatile__("isb");
}

__forceinline void Threading::EnableHiresScheduler()
{
}

__forceinline void Threading::DisableHiresScheduler()
{
}

u64 Threading::GetThreadTicksPerSecond()
{
	return 1000000;
}

u64 Threading::GetThreadCpuTime()
{
	return GetThreadCPUTimeUS(static_cast<void*>(pthread_self()));
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
{
	handle.m_native_handle = nullptr;
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle result;
	result.m_native_handle = static_cast<void*>(pthread_self());
	std::lock_guard lock(s_thread_handle_map_mutex);
	s_thread_handle_map[result.m_native_handle] = threadGetCurHandle();
	return result;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
	return *this;
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
	return GetThreadCPUTimeUS(m_native_handle);
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	if (!m_native_handle)
		return false;

	const u64 allowed_cores = Horizon::GetProcessCoreMask();
	if (allowed_cores == 0)
		return false;
#ifdef __SWITCH2__
	const u64 mask = processor_mask ? (processor_mask & allowed_cores) : allowed_cores;
	const s32 ideal_core = processor_mask ? __builtin_ctzll(mask) : -1;
#else
	const u64 mask = processor_mask ? (processor_mask & allowed_cores) : (allowed_cores & 0x7);
	const s32 ideal_core = __builtin_ctzll(mask);
#endif
	if (!mask)
		return false;

	Handle handle;
	if (static_cast<void*>(pthread_self()) == m_native_handle)
	{
		handle = CUR_THREAD_HANDLE;
	}
	else
	{
		std::lock_guard lock(s_thread_handle_map_mutex);
		const auto it = s_thread_handle_map.find(m_native_handle);
		if (it == s_thread_handle_map.end())
			return false;
		handle = it->second;
	}
	const Result result = svcSetThreadCoreMask(handle, ideal_core, mask);
	if (R_FAILED(result))
		Console.Warning("Switch: setting thread affinity 0x%llx failed: 0x%08x",
			static_cast<unsigned long long>(mask), result);
	return R_SUCCEEDED(result);
}

bool Threading::ThreadHandle::SetNicePriority(int nice) const
{
	(void)nice;
	return false;
}

u64 Threading::ThreadHandle::GetAffinity() const
{
	if (!m_native_handle)
		return 0;
	Handle handle = CUR_THREAD_HANDLE;
	if (static_cast<void*>(pthread_self()) != m_native_handle)
	{
		std::lock_guard lock(s_thread_handle_map_mutex);
		const auto it = s_thread_handle_map.find(m_native_handle);
		if (it == s_thread_handle_map.end())
			return 0;
		handle = it->second;
	}
	s32 preferred_core = 0;
	u64 mask = 0;
	return R_SUCCEEDED(svcGetThreadCoreMask(&preferred_core, &mask, handle)) ? mask : 0;
}

int Threading::ThreadHandle::GetCurrentCpu() const
{
	return m_native_handle == static_cast<void*>(pthread_self()) ? svcGetCurrentProcessorNumber() : -1;
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
{
	if (!Start(std::move(func)))
		pxFailRel("Failed to start implicitly started thread.");
}

Threading::Thread::~Thread()
{
	pxAssertRel(!m_native_handle, "Thread should be detached or joined at destruction");
}

void Threading::Thread::SetStackSize(u32 size)
{
	pxAssertRel(!m_native_handle, "Can't change the stack size on a started thread");
	m_stack_size = size;
}

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
	Horizon::ReserveCore3ForCallingThread();
	void* const key = static_cast<void*>(pthread_self());
	{
		std::lock_guard lock(s_thread_handle_map_mutex);
		s_thread_handle_map[key] = threadGetCurHandle();
	}
	Horizon::Profiling::RegisterCurrentThread("Worker");
	(*entry)();
	{
		std::lock_guard lock(s_thread_handle_map_mutex);
		s_thread_handle_map.erase(key);
	}
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already started thread");
	auto started = std::make_shared<std::promise<void>>();
	auto ready = started->get_future();
	auto entry = std::make_unique<EntryPoint>([func = std::move(func), started = std::move(started)]() mutable {
		started->set_value();
		func();
	});
	pthread_attr_t attrs;
	pthread_attr_t* attr = nullptr;
	if (m_stack_size != 0)
	{
		pthread_attr_init(&attrs);
		pthread_attr_setstacksize(&attrs, m_stack_size);
		attr = &attrs;
	}

	pthread_t handle;
	const int result = pthread_create(&handle, attr, ThreadProc, entry.get());
	if (attr)
		pthread_attr_destroy(&attrs);
	if (result != 0)
		return false;
	m_native_handle = static_cast<void*>(handle);
	entry.release();
	ready.wait();
	return true;
}

namespace
{
	struct DetachedThread
	{
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> finished;
	};

	std::mutex s_detached_mutex;
	std::vector<DetachedThread> s_detached_threads;
	std::vector<pthread_t> s_detached_handles;
} // namespace

void Threading::Thread::Detach()
{
	pxAssertRel(m_native_handle, "Can't detach without a thread");
	{
		std::lock_guard lock(s_detached_mutex);
		s_detached_handles.push_back(static_cast<pthread_t>(m_native_handle));
	}
	m_native_handle = nullptr;
}

void Threading::StartDetachedThread(std::function<void()> func)
{
	auto finished = std::make_shared<std::atomic<bool>>(false);
	std::thread thread([func = std::move(func), finished]() {
		func();
		finished->store(true, std::memory_order_release);
	});

	std::lock_guard lock(s_detached_mutex);
	for (auto it = s_detached_threads.begin(); it != s_detached_threads.end();)
	{
		if (it->finished->load(std::memory_order_acquire))
		{
			it->thread.join();
			it = s_detached_threads.erase(it);
		}
		else
		{
			++it;
		}
	}
	s_detached_threads.push_back({std::move(thread), std::move(finished)});
}

void Threading::JoinDetachedThreads()
{
	for (;;)
	{
		std::vector<DetachedThread> threads;
		std::vector<pthread_t> handles;
		{
			std::lock_guard lock(s_detached_mutex);
			threads.swap(s_detached_threads);
			handles.swap(s_detached_handles);
		}
		if (threads.empty() && handles.empty())
			break;

		for (DetachedThread& thread : threads)
			thread.thread.join();
		for (pthread_t handle : handles)
			pthread_join(handle, nullptr);
	}
}

void Threading::Thread::Join()
{
	pxAssertRel(m_native_handle, "Can't join without a thread");
	if (pthread_join(static_cast<pthread_t>(m_native_handle), nullptr) != 0)
		pxFailRel("pthread_join() for thread join failed");
	m_native_handle = nullptr;
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(thread);
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}

void Threading::SetNameOfCurrentThread(const char* name)
{
	Horizon::Profiling::RegisterCurrentThread(name);
}
