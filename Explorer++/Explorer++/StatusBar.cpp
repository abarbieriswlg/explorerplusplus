// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "StatusBar.h"
#include "BrowserWindow.h"
#include "Config.h"
#include "FmtWrapper.h"
#include "MainResource.h"
#include "ResourceLoader.h"
#include "ShellBrowser/NavigationEvents.h"
#include "ShellBrowser/ShellBrowserEvents.h"
#include "ShellBrowser/ShellBrowserImpl.h"
#include "StatusBarView.h"
#include "TabEvents.h"
#include <fmt/xchar.h>
#include <wil/common.h>
#include <chrono>
#include <format>

StatusBar::StatusBar(StatusBarView *view, BrowserWindow *browser, const Config *config,
	TabEvents *tabEvents, ShellBrowserEvents *shellBrowserEvents,
	NavigationEvents *navigationEvents, const ResourceLoader *resourceLoader) :
	m_view(view),
	m_browser(browser),
	m_config(config),
	m_resourceLoader(resourceLoader)
{
	m_view->SetFilterBoxCueBanner(m_resourceLoader->LoadString(IDS_QUICK_FILTER_CUE_BANNER));
	SetStandardParts();

	m_connections.push_back(m_view->filterTextChangedSignal.AddObserver(
		std::bind_front(&StatusBar::OnFilterTextChanged, this)));
	m_connections.push_back(m_view->filterBoxExitedSignal.AddObserver(
		std::bind_front(&StatusBar::OnFilterBoxExited, this)));

	m_connections.push_back(tabEvents->AddSelectedObserver(
		std::bind_front(&StatusBar::OnTabSelected, this), TabEventScope::ForBrowser(*browser)));

	m_connections.push_back(shellBrowserEvents->AddItemsChangedObserver(
		std::bind_front(&StatusBar::OnDirectoryContentsChanged, this),
		ShellBrowserEventScope::ForActiveShellBrowser(*browser)));
	m_connections.push_back(shellBrowserEvents->AddSelectionChangedObserver(
		std::bind_front(&StatusBar::OnListViewSelectionChanged, this),
		ShellBrowserEventScope::ForActiveShellBrowser(*browser)));

	m_connections.push_back(navigationEvents->AddStartedObserver(
		std::bind_front(&StatusBar::UpdateTextForNavigation, this),
		NavigationEventScope::ForActiveShellBrowser(*browser)));
	m_connections.push_back(navigationEvents->AddCommittedObserver(
		std::bind_front(&StatusBar::UpdateTextForNavigation, this),
		NavigationEventScope::ForActiveShellBrowser(*browser)));
	m_connections.push_back(navigationEvents->AddFailedObserver(
		std::bind_front(&StatusBar::UpdateTextForNavigation, this),
		NavigationEventScope::ForActiveShellBrowser(*browser)));
	m_connections.push_back(navigationEvents->AddCancelledObserver(
		std::bind_front(&StatusBar::UpdateTextForNavigation, this),
		NavigationEventScope::ForActiveShellBrowser(*browser)));
	m_connections.push_back(navigationEvents->AddStoppedObserver(
		std::bind_front(&StatusBar::OnNavigationsStopped, this),
		NavigationEventScope::ForActiveShellBrowser(*browser)));
}

void StatusBar::SetStandardParts()
{
	// The last part contains the quick filter box.
	m_view->SetParts({ 40, 58, 74, 100 });
	m_view->SetFilterBoxPart(3);
}

void StatusBar::FocusFilterBox()
{
	m_view->FocusFilterBox();
}

void StatusBar::OnFilterTextChanged(const std::wstring &text)
{
	m_browser->GetActiveShellBrowser()->SetQuickFilter(text);
}

void StatusBar::OnFilterBoxExited()
{
	m_browser->FocusActiveTab();
}

void StatusBar::UpdateFilterBoxText(const Tab &tab)
{
	const auto *shellBrowser = tab.GetShellBrowser();
	m_view->SetFilterText(shellBrowser->IsFilterEnabled() ? shellBrowser->GetFilterText() : L"");
}

void StatusBar::OnTabSelected(const Tab &tab)
{
	UpdateText(tab);
}

void StatusBar::OnDirectoryContentsChanged(const ShellBrowser *shellBrowser)
{
	const auto *tab = shellBrowser->GetTab();
	UpdateText(*tab);
}

void StatusBar::OnListViewSelectionChanged(const ShellBrowser *shellBrowser)
{
	const auto *tab = shellBrowser->GetTab();
	UpdateText(*tab);
}

void StatusBar::UpdateTextForNavigation(const NavigationRequest *request)
{
	const auto *tab = request->GetShellBrowser()->GetTab();
	UpdateText(*tab);
}

void StatusBar::OnNavigationsStopped(const ShellBrowser *shellBrowser)
{
	const auto *tab = shellBrowser->GetTab();
	UpdateText(*tab);
}

StatusBarView *StatusBar::GetView()
{
	return m_view;
}

void StatusBar::NotifyMenuLoopStarted()
{
	m_showingMenuHelpText = true;

	m_view->SetFilterBoxPart(std::nullopt);
	m_view->SetParts({ 100 });
	m_view->SetPartText(0, L"");
}

void StatusBar::NotifyMenuLoopEnded()
{
	m_showingMenuHelpText = false;

	const auto *tab = m_browser->GetActiveShellBrowser()->GetTab();
	SetStandardParts();
	UpdateText(*tab);
}

void StatusBar::SetMenuHelpText(const std::wstring &text)
{
	DCHECK(m_showingMenuHelpText);

	m_view->SetPartText(0, text);
}

void StatusBar::UpdateText(const Tab &tab)
{
	if (m_showingMenuHelpText)
	{
		// If menu help text is being shown, there's no need to update the status bar text for the
		// current tab.
		return;
	}

	UpdateFilterBoxText(tab);

	if (auto *navigation = tab.GetShellBrowser()->MaybeGetLatestActiveNavigation())
	{
		// In this case, there is at least one active navigation in progress, so the status bar
		// should reflect that.
		SetLoadingText(navigation->GetNavigateParams().pidl.Raw());
		return;
	}

	int numItemsSelected = tab.GetShellBrowserImpl()->GetNumSelected();
	std::wstring numItemsText;

	// The item count that's shown will either be the number of items selected, or the total number
	// of items if there is no selection.
	if (numItemsSelected != 0)
	{
		if (numItemsSelected == 1)
		{
			numItemsText = m_resourceLoader->LoadString(IDS_GENERAL_SELECTED_ONE_ITEM);
		}
		else
		{
			auto multipleItemsText =
				m_resourceLoader->LoadString(IDS_GENERAL_SELECTED_MULTIPLE_ITEMS);
			numItemsText = std::format(L"{:L} {}", numItemsSelected, multipleItemsText);
		}
	}
	else
	{
		int numItems = tab.GetShellBrowserImpl()->GetNumItems();

		if (numItems == 1)
		{
			numItemsText = m_resourceLoader->LoadString(IDS_GENERAL_ONE_ITEM);
		}
		else
		{
			auto multipleItemsText = m_resourceLoader->LoadString(IDS_GENERAL_MULTIPLE_ITEMS);
			numItemsText = std::format(L"{:L} {}", numItems, multipleItemsText);
		}
	}

	if (tab.GetShellBrowser()->IsFilterEnabled())
	{
		auto filterAppliedText = m_resourceLoader->LoadString(IDS_FILTER_ENABLED);
		numItemsText += L" | " + filterAppliedText;
	}

	m_view->SetPartText(0, numItemsText);

	std::wstring sizeText;

	if (numItemsSelected == 0)
	{
		auto displayFormat = m_config->globalFolderSettings.forceSize
			? m_config->globalFolderSettings.sizeDisplayFormat
			: SizeDisplayFormat::None;
		sizeText =
			FormatSizeString(tab.GetShellBrowserImpl()->GetTotalDirectorySize(), displayFormat);
	}
	else
	{
		// Note that no size will be shown if only folders are selected.
		if (tab.GetShellBrowserImpl()->GetNumSelectedFiles() != 0)
		{
			auto displayFormat = m_config->globalFolderSettings.forceSize
				? m_config->globalFolderSettings.sizeDisplayFormat
				: SizeDisplayFormat::None;
			sizeText =
				FormatSizeString(tab.GetShellBrowserImpl()->GetSelectionSize(), displayFormat);
		}
	}

	m_view->SetPartText(1, sizeText);

	m_view->SetPartText(2,
		GetCachedDriveFreeSpaceString(tab.GetShellBrowser()->GetDirectoryPath()));
}

void StatusBar::SetLoadingText(PCIDLIST_ABSOLUTE pidl)
{
	std::wstring loadingTemplate = m_resourceLoader->LoadString(IDS_GENERAL_LOADING);
	std::wstring loadingText = fmt::format(fmt::runtime(loadingTemplate),
		fmt::arg(L"folder_name", GetDisplayNameWithFallback(pidl, SHGDN_INFOLDER)));
	m_view->SetPartText(0, loadingText);
	m_view->SetPartText(1, L"");
	m_view->SetPartText(2, L"");
}

// The status bar text is updated whenever the selection changes. Determining the free space
// requires a call to GetDiskFreeSpaceEx(), which can be slow for network drives and cloud folders.
// Therefore, the result is cached briefly for each directory.
std::wstring StatusBar::GetCachedDriveFreeSpaceString(const std::wstring &path)
{
	constexpr auto CACHE_DURATION = std::chrono::seconds(5);
	auto now = std::chrono::steady_clock::now();

	if (path != m_freeSpaceCache.path || (now - m_freeSpaceCache.timestamp) > CACHE_DURATION)
	{
		m_freeSpaceCache.path = path;
		m_freeSpaceCache.text = CreateDriveFreeSpaceString(path);
		m_freeSpaceCache.timestamp = now;
	}

	return m_freeSpaceCache.text;
}

std::wstring StatusBar::CreateDriveFreeSpaceString(const std::wstring &path)
{
	ULARGE_INTEGER totalNumberOfBytes;
	ULARGE_INTEGER totalNumberOfFreeBytes;
	ULARGE_INTEGER bytesAvailableToCaller;
	if (GetDiskFreeSpaceEx(path.c_str(), &bytesAvailableToCaller, &totalNumberOfBytes,
			&totalNumberOfFreeBytes)
		== 0)
	{
		return {};
	}

	return std::format(L"{} {} ({:.0Lf}%)", FormatSizeString(totalNumberOfFreeBytes.QuadPart),
		m_resourceLoader->LoadString(IDS_GENERAL_FREE),
		totalNumberOfFreeBytes.QuadPart * 100.0 / totalNumberOfBytes.QuadPart);
}
