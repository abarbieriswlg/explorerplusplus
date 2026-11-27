// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#pragma once

#include <glog/logging.h>
#include <chrono>
#include <string>

// Used to measure how long the individual phases of startup take. The timings are written to the
// log (which is only enabled when the application is started with --enable-logging).
class ScopedStartupTimer
{
public:
	explicit ScopedStartupTimer(std::string name) :
		m_name(std::move(name)),
		m_start(std::chrono::steady_clock::now())
	{
	}

	~ScopedStartupTimer()
	{
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - m_start);
		LOG(INFO) << "Startup: " << m_name << " took " << elapsed.count() << " ms";
	}

	ScopedStartupTimer(const ScopedStartupTimer &) = delete;
	ScopedStartupTimer &operator=(const ScopedStartupTimer &) = delete;

private:
	const std::string m_name;
	const std::chrono::steady_clock::time_point m_start;
};

// Logs the amount of time that has elapsed since the process was created.
inline void LogTimeSinceProcessStart(const std::string &event)
{
	FILETIME creationTime;
	FILETIME exitTime;
	FILETIME kernelTime;
	FILETIME userTime;

	if (!GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime))
	{
		return;
	}

	FILETIME now;
	GetSystemTimeAsFileTime(&now);

	ULARGE_INTEGER start = { { creationTime.dwLowDateTime, creationTime.dwHighDateTime } };
	ULARGE_INTEGER current = { { now.dwLowDateTime, now.dwHighDateTime } };

	// FILETIME values are in 100 nanosecond intervals.
	auto elapsedMs = (current.QuadPart - start.QuadPart) / 10'000;
	LOG(INFO) << "Startup: " << event << " " << elapsedMs << " ms after the process started";
}
