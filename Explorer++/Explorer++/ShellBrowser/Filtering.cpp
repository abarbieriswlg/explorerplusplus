// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "ShellBrowserImpl.h"
#include "AppServices.h"
#include "FilterDialog.h"
#include "MainResource.h"
#include "NavigationRequest.h"
#include "ShellBrowserEvents.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/ScopedRedrawDisabler.h"
#include "../Helper/StringHelper.h"
#include <wil/common.h>
#include <Shlwapi.h>
#include <vector>

std::wstring ShellBrowserImpl::GetFilterText() const
{
	return m_folderSettings.filter;
}

void ShellBrowserImpl::SetFilterText(const std::wstring &filter)
{
	if (filter == m_folderSettings.filter)
	{
		return;
	}

	m_folderSettings.filter = filter;

	if (m_folderSettings.filterEnabled)
	{
		ReapplyFilter();
	}
}

bool ShellBrowserImpl::IsFilterCaseSensitive() const
{
	return m_folderSettings.filterCaseSensitive;
}

void ShellBrowserImpl::SetFilterCaseSensitive(bool caseSensitive)
{
	if (caseSensitive == m_folderSettings.filterCaseSensitive)
	{
		return;
	}

	m_folderSettings.filterCaseSensitive = caseSensitive;

	if (m_folderSettings.filterEnabled)
	{
		ReapplyFilter();
	}
}

bool ShellBrowserImpl::IsFilterEnabled() const
{
	return m_folderSettings.filterEnabled;
}

void ShellBrowserImpl::SetFilterEnabled(bool enabled)
{
	if (enabled == m_folderSettings.filterEnabled)
	{
		return;
	}

	m_folderSettings.filterEnabled = enabled;

	ReapplyFilter();
}

void ShellBrowserImpl::SetQuickFilter(const std::wstring &filter)
{
	if (filter.empty())
	{
		// Clearing the text in the quick filter box removes the filter entirely.
		if (!m_folderSettings.filterEnabled && m_folderSettings.filter.empty())
		{
			return;
		}

		m_folderSettings.filter.clear();
		m_folderSettings.filterEnabled = false;
		ReapplyFilter();
		return;
	}

	if (filter == m_folderSettings.filter && m_folderSettings.filterEnabled)
	{
		return;
	}

	// The text and enabled state are updated together, so that the items are only re-filtered
	// once.
	m_folderSettings.filter = filter;
	m_folderSettings.filterEnabled = true;
	ReapplyFilter();
}

// Brings the set of items shown in the listview in line with the current filter settings.
//
// The previous implementation restored every filtered item one at a time (performing a linear
// search to determine each item's sorted position and inserting each item individually), then
// removed the items that didn't match one at a time, with the listview redrawing after each
// change. That was quadratic in the number of items and made filtering large folders extremely
// slow.
//
// Here, the work is done in two batched passes, with redrawing disabled:
//
// 1. Items that are visible, but should now be hidden, are removed (iterating from the end of
//    the list, so that indexes remain valid and the amount of data moved is minimized).
// 2. Items that are hidden, but should now be visible, are appended in a single insertion pass,
//    after which the folder is sorted once.
void ShellBrowserImpl::ReapplyFilter()
{
	ScopedRedrawDisabler redrawDisabler(m_listView);

	int numItems = ListView_GetItemCount(m_listView);

	for (int i = numItems - 1; i >= 0; i--)
	{
		int internalIndex = GetItemInternalIndex(i);

		if (IsFileFiltered(m_itemInfoMap.at(internalIndex)))
		{
			RemoveFilteredItem(i, internalIndex);
		}
	}

	std::vector<int> itemsToRestore;

	for (int internalIndex : m_directoryState.filteredItemsList)
	{
		if (!IsFileFiltered(m_itemInfoMap.at(internalIndex)))
		{
			itemsToRestore.push_back(internalIndex);
		}
	}

	if (!itemsToRestore.empty())
	{
		int insertIndex = ListView_GetItemCount(m_listView);

		for (int internalIndex : itemsToRestore)
		{
			m_directoryState.filteredItemsList.erase(internalIndex);

			// The items are simply appended here. Their final positions will be set by the call
			// to SortFolder() below.
			AwaitingAdd_t awaitingAdd;
			awaitingAdd.iItem = insertIndex++;
			awaitingAdd.bPosition = FALSE;
			awaitingAdd.iAfter = -1;
			awaitingAdd.iItemInternal = internalIndex;
			m_directoryState.awaitingAddList.push_back(awaitingAdd);
		}

		InsertAwaitingItems();
		SortFolder();
	}

	m_shellBrowserEvents->NotifyItemsChanged(this);
}

// A quick (plain text) filter is tied to the folder it was entered in, in the same way as a
// type-to-filter box in other file managers. Therefore, it's cleared when navigating to a
// different folder (but retained when the current folder is refreshed). Wildcard filters set via
// the filter dialog retain their original behavior and persist across navigations.
void ShellBrowserImpl::MaybeClearQuickFilterForNavigation(const NavigationRequest *request)
{
	if (!m_folderSettings.filterEnabled || m_folderSettings.filter.empty()
		|| GetParsedFilter().isWildcard)
	{
		return;
	}

	if (m_folderVisited && request->GetNavigateParams().pidl == m_directoryState.pidlDirectory)
	{
		return;
	}

	// No items need to be updated here, since the listview is about to be cleared and
	// repopulated for the new folder.
	m_folderSettings.filter.clear();
	m_folderSettings.filterEnabled = false;
}

void ShellBrowserImpl::RemoveFilteredItem(int iItem, int iItemInternal)
{
	const auto &item = m_itemInfoMap.at(iItemInternal);

	if (WI_IsFlagSet(ListView_GetItemState(m_listView, iItem, LVIS_SELECTED), LVIS_SELECTED))
	{
		// Deselecting the item will trigger a LVN_ITEMCHANGED notification, which will update the
		// selection counts and selection size. Previously, only the selection size was updated
		// here, which left the selected item counts out of sync.
		ListView_SetItemState(m_listView, iItem, 0, LVIS_SELECTED);
	}

	if (m_folderSettings.showInGroups)
	{
		auto groupId = GetItemGroupId(iItem);

		if (groupId)
		{
			OnItemRemovedFromGroup(*groupId);
		}
	}

	/* Take the file size of the removed file away from the total
	directory size. */
	ULARGE_INTEGER ulFileSize;
	ulFileSize.LowPart = item.wfd.nFileSizeLow;
	ulFileSize.HighPart = item.wfd.nFileSizeHigh;

	m_directoryState.totalDirSize -= ulFileSize.QuadPart;

	ListView_DeleteItem(m_listView, iItem);

	m_directoryState.numItems--;

	DCHECK_EQ(m_directoryState.filteredItemsList.count(iItemInternal), 0u);
	m_directoryState.filteredItemsList.insert(iItemInternal);
}

bool ShellBrowserImpl::IsItemFilteredByName(const ItemInfo_t &itemInfo) const
{
	if (!m_folderSettings.filterEnabled || m_folderSettings.filter.empty())
	{
		return false;
	}

	const auto &filter = GetParsedFilter();
	bool isFolder = WI_IsFlagSet(itemInfo.wfd.dwFileAttributes, FILE_ATTRIBUTE_DIRECTORY);

	if (filter.isWildcard)
	{
		// Wildcard filters (e.g. "*.txt" or "*.h: *.cpp") retain the original behavior and only
		// apply to files, so that it's still possible to navigate into subfolders.
		if (isFolder)
		{
			return false;
		}

		return !CheckWildcardMatch(m_folderSettings.filter.c_str(), itemInfo.displayName.c_str(),
			m_folderSettings.filterCaseSensitive);
	}

	// Plain text filters (as typed into the quick filter box) match against any part of the
	// name and apply to both files and folders. If multiple space-separated terms are entered,
	// each term must be present.
	for (const auto &term : filter.terms)
	{
		bool found;

		if (m_folderSettings.filterCaseSensitive)
		{
			found = itemInfo.displayName.find(term) != std::wstring::npos;
		}
		else
		{
			found = StrStrIW(itemInfo.displayName.c_str(), term.c_str()) != nullptr;
		}

		if (!found)
		{
			return true;
		}
	}

	return false;
}

const ShellBrowserImpl::ParsedFilter &ShellBrowserImpl::GetParsedFilter() const
{
	if (m_parsedFilter.source == m_folderSettings.filter)
	{
		return m_parsedFilter;
	}

	m_parsedFilter = {};
	m_parsedFilter.source = m_folderSettings.filter;
	m_parsedFilter.isWildcard = m_folderSettings.filter.find_first_of(L"*?:") != std::wstring::npos;

	if (!m_parsedFilter.isWildcard)
	{
		size_t pos = 0;
		const auto &text = m_folderSettings.filter;

		while (pos < text.size())
		{
			size_t start = text.find_first_not_of(L' ', pos);

			if (start == std::wstring::npos)
			{
				break;
			}

			size_t end = text.find(L' ', start);

			if (end == std::wstring::npos)
			{
				end = text.size();
			}

			m_parsedFilter.terms.push_back(text.substr(start, end - start));
			pos = end;
		}
	}

	return m_parsedFilter;
}

void ShellBrowserImpl::UnfilterItem(int internalIndex)
{
	assert(m_directoryState.filteredItemsList.count(internalIndex) == 1);

	RestoreFilteredItem(internalIndex);
	m_directoryState.filteredItemsList.erase(internalIndex);
	m_shellBrowserEvents->NotifyItemsChanged(this);
}

void ShellBrowserImpl::RestoreFilteredItem(int internalIndex)
{
	int sortedPosition = DetermineItemSortedPosition(internalIndex);

	AwaitingAdd_t awaitingAdd;
	awaitingAdd.iItem = sortedPosition;
	awaitingAdd.bPosition = TRUE;
	awaitingAdd.iAfter = sortedPosition - 1;
	awaitingAdd.iItemInternal = internalIndex;
	m_directoryState.awaitingAddList.push_back(awaitingAdd);

	InsertAwaitingItems();
}

void ShellBrowserImpl::EditFilterSettings()
{
	auto *filterDialog = FilterDialog::Create(m_resourceLoader, m_owner, this);
	filterDialog->ShowModalDialog();
}
