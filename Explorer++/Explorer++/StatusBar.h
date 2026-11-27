// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#pragma once

#include <boost/signals2.hpp>
#include <chrono>
#include <string>
#include <vector>

class BrowserWindow;
struct Config;
class NavigationEvents;
class NavigationRequest;
class ResourceLoader;
class ShellBrowser;
class ShellBrowserEvents;
class StatusBarView;
class Tab;
class TabEvents;

class StatusBar
{
public:
	StatusBar(StatusBarView *view, BrowserWindow *browser, const Config *config,
		TabEvents *tabEvents, ShellBrowserEvents *shellBrowserEvents,
		NavigationEvents *navigationEvents, const ResourceLoader *resourceLoader);

	StatusBarView *GetView();

	void NotifyMenuLoopStarted();
	void NotifyMenuLoopEnded();
	void SetMenuHelpText(const std::wstring &text);

	void FocusFilterBox();

private:
	void SetStandardParts();

	void OnTabSelected(const Tab &tab);
	void OnDirectoryContentsChanged(const ShellBrowser *shellBrowser);
	void OnListViewSelectionChanged(const ShellBrowser *shellBrowser);
	void UpdateTextForNavigation(const NavigationRequest *request);
	void OnNavigationsStopped(const ShellBrowser *shellBrowser);

	void UpdateText(const Tab &tab);
	void UpdateFilterBoxText(const Tab &tab);
	void OnFilterTextChanged(const std::wstring &text);
	void OnFilterBoxExited();
	void SetLoadingText(PCIDLIST_ABSOLUTE pidl);
	std::wstring GetCachedDriveFreeSpaceString(const std::wstring &path);
	std::wstring CreateDriveFreeSpaceString(const std::wstring &path);

	StatusBarView *const m_view;
	BrowserWindow *const m_browser;
	const Config *const m_config;
	const ResourceLoader *const m_resourceLoader;
	bool m_showingMenuHelpText = false;

	struct FreeSpaceCache
	{
		std::wstring path;
		std::wstring text;
		std::chrono::steady_clock::time_point timestamp;
	};

	FreeSpaceCache m_freeSpaceCache;
	std::vector<boost::signals2::scoped_connection> m_connections;
};
