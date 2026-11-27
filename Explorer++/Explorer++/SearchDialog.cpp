// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "SearchDialog.h"
#include "BrowserList.h"
#include "BrowserView.h"
#include "BrowserWindow.h"
#include "DialogConstants.h"
#include "MainResource.h"
#include "NoOpMenuHelpTextHost.h"
#include "PopupMenuView.h"
#include "ResourceLoader.h"
#include "SearchResultsItemContextMenu.h"
#include "ShellBrowser/NavigateParams.h"
#include "ShellBrowser/ShellBrowser.h"
#include "ShellBrowser/ShellNavigationController.h"
#include "../Helper/BulkClipboardWriter.h"
#include "../Helper/ComboBox.h"
#include "../Helper/Controls.h"
#include "../Helper/DpiCompatibility.h"
#include "../Helper/DragDropHelper.h"
#include "../Helper/FileDialogs.h"
#include "../Helper/FileOperations.h"
#include "../Helper/Helper.h"
#include "../Helper/ImageInterop.h"
#include "../Helper/RegistrySettings.h"
#include "../Helper/ShellHelper.h"
#include "../Helper/StringHelper.h"
#include "../Helper/WindowHelper.h"
#include "../Helper/WindowSubclass.h"
#include "../Helper/XMLSettings.h"
#include <wil/common.h>
#include <Shlwapi.h>
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <format>
#include <regex>
#include <thread>

namespace
{

const UINT WM_APP_SEARCH_RESULTS_AVAILABLE = WM_APP + 1;
const UINT WM_APP_SEARCH_FINISHED = WM_APP + 2;

// The number of results the search thread accumulates before handing them over to the dialog.
// Results are also handed over after a short period of time, so that they appear promptly even
// when matches are infrequent.
constexpr size_t RESULTS_BATCH_SIZE = 512;
constexpr auto RESULTS_BATCH_INTERVAL = std::chrono::milliseconds(100);

// The maximum number of items that will be opened at once when activating a multi-item selection.
constexpr size_t MAX_ITEMS_TO_OPEN = 20;

enum class ResultsColumn
{
	Name = 0,
	Path = 1,
	Size = 2,
	DateModified = 3,
	Type = 4
};

class SearchWorker
{
public:
	SearchWorker(SearchParameters parameters, std::shared_ptr<SearchSharedState> state) :
		m_parameters(std::move(parameters)),
		m_state(std::move(state))
	{
	}

	void Run()
	{
		if (!InitializePattern())
		{
			std::scoped_lock lock(m_state->mutex);
			m_state->regularExpressionInvalid = true;
			NotifyFinishedLocked();
			return;
		}

		m_lastFlush = std::chrono::steady_clock::now();

		std::vector<std::wstring> directoriesToSearch;
		directoriesToSearch.push_back(m_parameters.baseDirectory);

		while (!directoriesToSearch.empty() && !m_state->stopRequested)
		{
			std::wstring directory = std::move(directoriesToSearch.back());
			directoriesToSearch.pop_back();

			{
				std::scoped_lock lock(m_state->mutex);
				m_state->currentDirectory = directory;
			}

			std::vector<std::wstring> subfolders;
			SearchDirectory(directory, subfolders);

			// The subfolders are added in reverse order, so that they're searched in the order
			// in which they were enumerated.
			directoriesToSearch.insert(directoriesToSearch.end(),
				std::make_move_iterator(subfolders.rbegin()),
				std::make_move_iterator(subfolders.rend()));

			if (m_batch.size() >= RESULTS_BATCH_SIZE
				|| (std::chrono::steady_clock::now() - m_lastFlush) >= RESULTS_BATCH_INTERVAL)
			{
				FlushResults();
			}
		}

		FlushResults();

		std::scoped_lock lock(m_state->mutex);
		m_state->foldersFound = m_foldersFound;
		m_state->filesFound = m_filesFound;
		NotifyFinishedLocked();
	}

private:
	bool InitializePattern()
	{
		if (m_parameters.pattern.empty())
		{
			return true;
		}

		if (m_parameters.useRegularExpressions)
		{
			try
			{
				auto flags = std::regex_constants::ECMAScript;

				if (m_parameters.caseInsensitive)
				{
					flags |= std::regex_constants::icase;
				}

				m_regex.assign(m_parameters.pattern, flags);
			}
			catch (const std::regex_error &)
			{
				return false;
			}

			return true;
		}

		// Patterns of the form "abc" are turned into "*abc*", so that they match any name that
		// contains the text.
		const auto &pattern = m_parameters.pattern;

		if (pattern.front() != '*' && pattern.back() != '*')
		{
			m_wildcardPattern = L"*" + pattern + L"*";
		}
		else
		{
			m_wildcardPattern = pattern;
		}

		return true;
	}

	static std::wstring CombinePath(const std::wstring &directory, const wchar_t *name)
	{
		std::wstring path = directory;

		if (!path.empty() && path.back() != '\\')
		{
			path += '\\';
		}

		path += name;
		return path;
	}

	// Paths longer than MAX_PATH need to be prefixed, so that the Unicode versions of the file
	// APIs will accept them.
	static std::wstring GetEnumerationPattern(const std::wstring &directory)
	{
		std::wstring pattern = CombinePath(directory, L"*");

		if (pattern.size() >= MAX_PATH && pattern.compare(0, 4, L"\\\\?\\") != 0)
		{
			if (pattern.compare(0, 2, L"\\\\") == 0)
			{
				pattern = L"\\\\?\\UNC\\" + pattern.substr(2);
			}
			else
			{
				pattern = L"\\\\?\\" + pattern;
			}
		}

		return pattern;
	}

	// Junctions and symbolic links to directories aren't followed, since they can create cycles
	// (and often point to locations that are searched anyway). Other reparse points (e.g. the
	// placeholders used by OneDrive and other cloud storage providers) are treated as normal
	// directories.
	static bool IsDirectoryLink(const WIN32_FIND_DATA &findData)
	{
		if (WI_IsFlagClear(findData.dwFileAttributes, FILE_ATTRIBUTE_REPARSE_POINT))
		{
			return false;
		}

		return findData.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT
			|| findData.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
	}

	void SearchDirectory(const std::wstring &directory, std::vector<std::wstring> &subfolders)
	{
		WIN32_FIND_DATA findData;
		HANDLE findHandle = FindFirstFileEx(GetEnumerationPattern(directory).c_str(),
			FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

		if (findHandle == INVALID_HANDLE_VALUE)
		{
			return;
		}

		std::optional<uint32_t> directoryIndex;

		// Note that the first item returned by FindFirstFileEx() is processed here as well.
		// Previously, the first item was skipped (on the assumption that it was "."), which meant
		// that one item was missed when searching from the root of a drive.
		do
		{
			if (lstrcmp(findData.cFileName, L".") == 0 || lstrcmp(findData.cFileName, L"..") == 0)
			{
				continue;
			}

			bool isFolder = WI_IsFlagSet(findData.dwFileAttributes, FILE_ATTRIBUTE_DIRECTORY);

			if (Matches(findData, isFolder))
			{
				if (!directoryIndex)
				{
					directoryIndex = m_nextDirectoryIndex++;
					m_batchDirectories.push_back(directory);
				}

				ULARGE_INTEGER size = { { findData.nFileSizeLow, findData.nFileSizeHigh } };

				SearchResult result;
				result.name = findData.cFileName;
				result.directoryIndex = *directoryIndex;
				result.attributes = findData.dwFileAttributes;
				result.size = isFolder ? 0 : size.QuadPart;
				result.lastModified = findData.ftLastWriteTime;
				m_batch.push_back(std::move(result));

				if (isFolder)
				{
					m_foldersFound++;
				}
				else
				{
					m_filesFound++;
				}
			}

			if (isFolder && m_parameters.searchSubFolders && !IsDirectoryLink(findData))
			{
				subfolders.push_back(CombinePath(directory, findData.cFileName));
			}
		} while (!m_state->stopRequested && FindNextFile(findHandle, &findData));

		FindClose(findHandle);
	}

	bool Matches(const WIN32_FIND_DATA &findData, bool isFolder) const
	{
		if (m_parameters.itemType == SearchItemType::FilesOnly && isFolder)
		{
			return false;
		}

		if (m_parameters.itemType == SearchItemType::FoldersOnly && !isFolder)
		{
			return false;
		}

		if (m_parameters.attributes != 0
			&& (findData.dwFileAttributes & m_parameters.attributes) != m_parameters.attributes)
		{
			return false;
		}

		if (m_parameters.minSize || m_parameters.maxSize)
		{
			// Folders don't have a size, so they never match a size filter.
			if (isFolder)
			{
				return false;
			}

			ULARGE_INTEGER size = { { findData.nFileSizeLow, findData.nFileSizeHigh } };

			if (m_parameters.minSize && size.QuadPart < *m_parameters.minSize)
			{
				return false;
			}

			if (m_parameters.maxSize && size.QuadPart > *m_parameters.maxSize)
			{
				return false;
			}
		}

		if (m_parameters.modifiedAfter
			&& CompareFileTime(&findData.ftLastWriteTime, &*m_parameters.modifiedAfter) < 0)
		{
			return false;
		}

		if (m_parameters.modifiedBefore
			&& CompareFileTime(&findData.ftLastWriteTime, &*m_parameters.modifiedBefore) >= 0)
		{
			return false;
		}

		if (m_parameters.pattern.empty())
		{
			return true;
		}

		if (m_parameters.useRegularExpressions)
		{
			return std::regex_match(findData.cFileName, m_regex);
		}

		return CheckWildcardMatch(m_wildcardPattern.c_str(), findData.cFileName,
			!m_parameters.caseInsensitive);
	}

	void FlushResults()
	{
		m_lastFlush = std::chrono::steady_clock::now();

		if (m_batch.empty())
		{
			return;
		}

		std::scoped_lock lock(m_state->mutex);

		m_state->pendingDirectories.insert(m_state->pendingDirectories.end(),
			std::make_move_iterator(m_batchDirectories.begin()),
			std::make_move_iterator(m_batchDirectories.end()));
		m_state->pendingResults.insert(m_state->pendingResults.end(),
			std::make_move_iterator(m_batch.begin()), std::make_move_iterator(m_batch.end()));
		m_state->foldersFound = m_foldersFound;
		m_state->filesFound = m_filesFound;

		m_batch.clear();
		m_batchDirectories.clear();

		// Only a single notification is posted until the dialog picks up the results. That
		// avoids flooding the dialog's message queue.
		if (!m_state->resultsNotificationPending && m_state->notifyWindow)
		{
			m_state->resultsNotificationPending = true;
			PostMessage(m_state->notifyWindow, WM_APP_SEARCH_RESULTS_AVAILABLE, 0, 0);
		}
	}

	void NotifyFinishedLocked()
	{
		if (m_state->notifyWindow)
		{
			PostMessage(m_state->notifyWindow, WM_APP_SEARCH_FINISHED, 0, 0);
		}
	}

	const SearchParameters m_parameters;
	const std::shared_ptr<SearchSharedState> m_state;

	std::wregex m_regex;
	std::wstring m_wildcardPattern;

	std::vector<SearchResult> m_batch;
	std::vector<std::wstring> m_batchDirectories;
	uint32_t m_nextDirectoryIndex = 0;
	std::chrono::steady_clock::time_point m_lastFlush;

	int m_foldersFound = 0;
	int m_filesFound = 0;
};

bool ResultHasPerFileIcon(const std::wstring &name)
{
	const wchar_t *extension = PathFindExtension(name.c_str());

	// These file types typically have icons that are specific to each file.
	static const wchar_t *const perFileIconExtensions[] = { L".exe", L".lnk", L".ico", L".url",
		L".cpl", L".msc", L".scr", L".appref-ms" };

	for (const auto *perFileIconExtension : perFileIconExtensions)
	{
		if (lstrcmpi(extension, perFileIconExtension) == 0)
		{
			return true;
		}
	}

	return false;
}

}

SearchDialog *SearchDialog::Create(const ResourceLoader *resourceLoader, HWND hParent,
	std::wstring_view searchDirectory, BrowserList *browserList,
	const AcceleratorManager *acceleratorManager, ClipboardStore *clipboardStore)
{
	return new SearchDialog(resourceLoader, hParent, searchDirectory, browserList,
		acceleratorManager, clipboardStore);
}

SearchDialog::SearchDialog(const ResourceLoader *resourceLoader, HWND hParent,
	std::wstring_view searchDirectory, BrowserList *browserList,
	const AcceleratorManager *acceleratorManager, ClipboardStore *clipboardStore) :
	BaseDialog(resourceLoader, IDD_SEARCH, hParent, DialogSizingType::Both, Icon::Search),
	m_searchDirectory(searchDirectory),
	m_browserList(browserList),
	m_acceleratorManager(acceleratorManager),
	m_clipboardStore(clipboardStore)
{
	m_persistentSettings = &SearchDialogPersistentSettings::GetInstance();
}

SearchDialog::~SearchDialog()
{
	StopSearching();
}

void SearchDialog::AddDynamicControls()
{
	// The results listview is replaced with a virtual listview. With a large number of results,
	// that's significantly faster than inserting each result individually. Replacing the control
	// here (rather than changing the dialog template) means that this also works when the dialog
	// is loaded from a translation DLL.
	HWND originalListView = GetDlgItem(m_hDlg, IDC_LISTVIEW_SEARCHRESULTS);
	CHECK(originalListView);

	RECT rect;
	GetWindowRect(originalListView, &rect);
	MapWindowPoints(nullptr, m_hDlg, reinterpret_cast<POINT *>(&rect), 2);

	DWORD style = static_cast<DWORD>(GetWindowLongPtr(originalListView, GWL_STYLE));
	DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(originalListView, GWL_EXSTYLE));
	HWND previousControl = GetWindow(originalListView, GW_HWNDPREV);

	DestroyWindow(originalListView);

	WI_SetFlag(style, LVS_OWNERDATA);
	WI_ClearFlag(style, LVS_SINGLESEL);

	HWND listView = CreateWindowEx(exStyle, WC_LISTVIEW, L"", style, rect.left, rect.top,
		GetRectWidth(&rect), GetRectHeight(&rect), m_hDlg,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LISTVIEW_SEARCHRESULTS)),
		GetModuleHandle(nullptr), nullptr);
	CHECK(listView);

	// Keep the tab order the same as it was for the original control.
	SetWindowPos(listView, previousControl ? previousControl : HWND_TOP, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	SendMessage(listView, WM_SETFONT, SendMessage(m_hDlg, WM_GETFONT, 0, 0), false);

	m_listViewSubclass = std::make_unique<WindowSubclass>(listView,
		std::bind_front(&SearchDialog::ResultsListViewSubclass, this));
}

INT_PTR SearchDialog::OnInitDialog()
{
	UINT dpi = DpiCompatibility::GetInstance().GetDpiForWindow(m_hDlg);
	auto directoryBitmap = m_resourceLoader->LoadBitmapForDpi(Icon::Folder, 16, 16, dpi);
	HRESULT hr = ImageInterop::CreateHICONFromWICBitmap(directoryBitmap.get(), m_directoryIcon);
	CHECK(SUCCEEDED(hr));
	SendMessage(GetDlgItem(m_hDlg, IDC_BUTTON_DIRECTORY), BM_SETIMAGE, IMAGE_ICON,
		reinterpret_cast<LPARAM>(m_directoryIcon.get()));

	HWND hListView = GetResultsListView();

	ListView_SetExtendedListViewStyleEx(hListView,
		LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP,
		LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
	SetWindowTheme(hListView, L"Explorer", nullptr);

	// Hidden items are shown ghosted. The cut state is provided via LVN_GETDISPINFO.
	ListView_SetCallbackMask(hListView, LVIS_CUT);

	HIMAGELIST himlSmall;
	Shell_GetImageLists(nullptr, &himlSmall);
	ListView_SetImageList(hListView, himlSmall, LVSIL_SMALL);

	int i = 0;

	for (const auto &ci : m_persistentSettings->m_Columns)
	{
		auto columnName = m_resourceLoader->LoadString(ci.uStringID);

		LVCOLUMN lvColumn;
		lvColumn.mask = LVCF_TEXT | LVCF_FMT;
		lvColumn.fmt = (ci.sortMode == SearchDialogPersistentSettings::SortMode::Size)
			? LVCFMT_RIGHT
			: LVCFMT_LEFT;
		lvColumn.pszText = columnName.data();
		ListView_InsertColumn(hListView, i, &lvColumn);

		i++;
	}

	RECT rc;
	GetClientRect(hListView, &rc);
	int width = GetRectWidth(&rc);

	const double defaultColumnWidths[] = { 0.30, 0.38, 0.10, 0.14, 0.08 };
	static_assert(std::size(defaultColumnWidths) == SearchDialogPersistentSettings::NUM_COLUMNS);

	for (int column = 0; column < SearchDialogPersistentSettings::NUM_COLUMNS; column++)
	{
		int columnWidth = static_cast<int>(defaultColumnWidths[column] * width);

		if (m_persistentSettings->m_bStateSaved && m_persistentSettings->m_columnWidths[column] > 0)
		{
			columnWidth = m_persistentSettings->m_columnWidths[column];
		}

		ListView_SetColumnWidth(hListView, column, columnWidth);
	}

	auto sortMode = m_persistentSettings->m_SortMode;

	if (std::none_of(m_persistentSettings->m_Columns.begin(), m_persistentSettings->m_Columns.end(),
			[sortMode](const auto &column) { return column.sortMode == sortMode; }))
	{
		m_persistentSettings->m_SortMode = SearchDialogPersistentSettings::SortMode::Name;
	}

	UpdateListViewHeader();

	lCheckDlgButton(m_hDlg, IDC_CHECK_ARCHIVE, m_persistentSettings->m_bArchive);
	lCheckDlgButton(m_hDlg, IDC_CHECK_HIDDEN, m_persistentSettings->m_bHidden);
	lCheckDlgButton(m_hDlg, IDC_CHECK_READONLY, m_persistentSettings->m_bReadOnly);
	lCheckDlgButton(m_hDlg, IDC_CHECK_SYSTEM, m_persistentSettings->m_bSystem);
	lCheckDlgButton(m_hDlg, IDC_CHECK_SEARCHSUBFOLDERS, m_persistentSettings->m_bSearchSubFolders);
	lCheckDlgButton(m_hDlg, IDC_CHECK_CASEINSENSITIVE, m_persistentSettings->m_bCaseInsensitive);
	lCheckDlgButton(m_hDlg, IDC_CHECK_USEREGULAREXPRESSIONS,
		m_persistentSettings->m_bUseRegularExpressions);

	for (const auto &strDirectory : m_persistentSettings->m_searchDirectories)
	{
		SendDlgItemMessage(m_hDlg, IDC_COMBO_DIRECTORY, CB_INSERTSTRING, static_cast<WPARAM>(-1),
			reinterpret_cast<LPARAM>(strDirectory.c_str()));
	}

	for (const auto &strPattern : m_persistentSettings->m_searchPatterns)
	{
		SendDlgItemMessage(m_hDlg, IDC_COMBO_NAME, CB_INSERTSTRING, static_cast<WPARAM>(-1),
			reinterpret_cast<LPARAM>(strPattern.c_str()));
	}

	SetDlgItemText(m_hDlg, IDC_COMBO_NAME, m_persistentSettings->m_searchPattern.c_str());
	SetDlgItemText(m_hDlg, IDC_COMBO_DIRECTORY, m_searchDirectory.c_str());

	ComboBox::CreateNew(GetDlgItem(m_hDlg, IDC_COMBO_NAME));
	ComboBox::CreateNew(GetDlgItem(m_hDlg, IDC_COMBO_DIRECTORY));

	SetUpFilterControls();

	SetFocus(GetDlgItem(m_hDlg, IDC_COMBO_NAME));

	m_persistentSettings->RestoreDialogPosition(m_hDlg, true);

	return FALSE;
}

void SearchDialog::SetUpFilterControls()
{
	// The filter controls may not be present (e.g. if the dialog was loaded from a translation DLL
	// that hasn't been updated yet). In that case, the filters simply aren't available.
	if (HWND itemTypeCombo = GetDlgItem(m_hDlg, IDC_SEARCH_ITEM_TYPE))
	{
		ComboBox_AddString(itemTypeCombo,
			m_resourceLoader->LoadString(IDS_SEARCH_ITEM_TYPE_FILES_AND_FOLDERS).c_str());
		ComboBox_AddString(itemTypeCombo,
			m_resourceLoader->LoadString(IDS_SEARCH_ITEM_TYPE_FILES_ONLY).c_str());
		ComboBox_AddString(itemTypeCombo,
			m_resourceLoader->LoadString(IDS_SEARCH_ITEM_TYPE_FOLDERS_ONLY).c_str());

		int itemType = std::clamp(m_persistentSettings->m_itemType, 0, 2);
		ComboBox_SetCurSel(itemTypeCombo, itemType);
	}

	for (int unitId : { IDC_SEARCH_MIN_SIZE_UNIT, IDC_SEARCH_MAX_SIZE_UNIT })
	{
		HWND unitCombo = GetDlgItem(m_hDlg, unitId);

		if (!unitCombo)
		{
			continue;
		}

		for (const auto *unit : { L"KB", L"MB", L"GB" })
		{
			ComboBox_AddString(unitCombo, unit);
		}

		// Default to MB, which is likely to be the most commonly used unit.
		ComboBox_SetCurSel(unitCombo, 1);
	}

	for (int dateTimePickerId : { IDC_SEARCH_MODIFIED_AFTER, IDC_SEARCH_MODIFIED_BEFORE })
	{
		HWND dateTimePicker = GetDlgItem(m_hDlg, dateTimePickerId);

		if (!dateTimePicker)
		{
			continue;
		}

		// The date filters are disabled by default (the checkbox within the control is
		// unchecked).
		DateTime_SetSystemtime(dateTimePicker, GDT_NONE, nullptr);
	}
}

std::vector<ResizableDialogControl> SearchDialog::GetResizableControls()
{
	std::vector<ResizableDialogControl> controls;
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_COMBO_NAME), MovingType::None,
		SizingType::Horizontal);
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_COMBO_DIRECTORY), MovingType::None,
		SizingType::Horizontal);
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_BUTTON_DIRECTORY), MovingType::Horizontal,
		SizingType::None);
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_LISTVIEW_SEARCHRESULTS), MovingType::None,
		SizingType::Both);
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_STATIC_STATUSLABEL), MovingType::Vertical,
		SizingType::None);
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_STATIC_STATUS), MovingType::Vertical,
		SizingType::Horizontal);
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_LINK_STATUS), MovingType::Vertical,
		SizingType::Horizontal);
	controls.emplace_back(GetDlgItem(m_hDlg, IDC_STATIC_ETCHEDHORZ), MovingType::Vertical,
		SizingType::Horizontal);
	controls.emplace_back(GetDlgItem(m_hDlg, IDSEARCH), MovingType::Both, SizingType::None);
	controls.emplace_back(GetDlgItem(m_hDlg, IDEXIT), MovingType::Both, SizingType::None);

	if (HWND filtersGroup = GetDlgItem(m_hDlg, IDC_GROUP_SEARCH_FILTERS))
	{
		controls.emplace_back(filtersGroup, MovingType::None, SizingType::Horizontal);
	}

	return controls;
}

INT_PTR SearchDialog::OnCommand(WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);

	switch (LOWORD(wParam))
	{
	case IDC_BUTTON_DIRECTORY:
		OnBrowserForFolder();
		break;

	case IDSEARCH:
		OnSearch();
		break;

	case IDEXIT:
		DestroyWindow(m_hDlg);
		break;

	case IDCANCEL:
		DestroyWindow(m_hDlg);
		break;
	}

	return 0;
}

void SearchDialog::OnBrowserForFolder()
{
	wchar_t defaultDirectory[MAX_PATH];
	GetDlgItemText(m_hDlg, IDC_COMBO_DIRECTORY, defaultDirectory, std::size(defaultDirectory));

	std::wstring folderPath;
	HRESULT hr = FileDialogs::ShowSelectFolderDialog(m_hDlg, defaultDirectory, folderPath);

	if (FAILED(hr))
	{
		return;
	}

	SetDlgItemText(m_hDlg, IDC_COMBO_DIRECTORY, folderPath.c_str());
}

void SearchDialog::OnSearch()
{
	if (!m_searching)
	{
		StartSearching();
	}
	else
	{
		StopSearching();
	}
}

std::optional<uint64_t> SearchDialog::GetSizeFilterValue(int editId, int unitId)
{
	HWND edit = GetDlgItem(m_hDlg, editId);

	if (!edit)
	{
		return std::nullopt;
	}

	auto text = GetDlgItemString(m_hDlg, editId);
	TrimString(text, L" ");

	if (text.empty())
	{
		return std::nullopt;
	}

	wchar_t *end = nullptr;
	double value = wcstod(text.c_str(), &end);

	if (end == text.c_str() || value < 0)
	{
		return std::nullopt;
	}

	uint64_t multiplier = 1024 * 1024;

	if (HWND unitCombo = GetDlgItem(m_hDlg, unitId))
	{
		switch (ComboBox_GetCurSel(unitCombo))
		{
		case 0:
			multiplier = 1024;
			break;

		case 2:
			multiplier = 1024ull * 1024 * 1024;
			break;
		}
	}

	return static_cast<uint64_t>(value * static_cast<double>(multiplier));
}

std::optional<FILETIME> SearchDialog::GetDateFilterValue(int dateTimePickerId, bool endOfDay)
{
	HWND dateTimePicker = GetDlgItem(m_hDlg, dateTimePickerId);

	if (!dateTimePicker)
	{
		return std::nullopt;
	}

	SYSTEMTIME localTime;

	if (DateTime_GetSystemtime(dateTimePicker, &localTime) != GDT_VALID)
	{
		return std::nullopt;
	}

	// Only the date part is relevant.
	localTime.wHour = 0;
	localTime.wMinute = 0;
	localTime.wSecond = 0;
	localTime.wMilliseconds = 0;

	FILETIME localFileTime;

	if (!SystemTimeToFileTime(&localTime, &localFileTime))
	{
		return std::nullopt;
	}

	if (endOfDay)
	{
		// The "before" date is inclusive, so the limit is the start of the following day.
		ULARGE_INTEGER time = { { localFileTime.dwLowDateTime, localFileTime.dwHighDateTime } };
		time.QuadPart += 24ull * 60 * 60 * 10'000'000;
		localFileTime.dwLowDateTime = time.LowPart;
		localFileTime.dwHighDateTime = time.HighPart;
	}

	FILETIME utcFileTime;

	if (!LocalFileTimeToFileTime(&localFileTime, &utcFileTime))
	{
		return std::nullopt;
	}

	return utcFileTime;
}

std::optional<SearchParameters> SearchDialog::BuildSearchParameters()
{
	SearchParameters parameters;

	parameters.baseDirectory = GetDlgItemString(m_hDlg, IDC_COMBO_DIRECTORY);
	TrimString(parameters.baseDirectory, L" ");

	parameters.pattern = GetDlgItemString(m_hDlg, IDC_COMBO_NAME);
	TrimString(parameters.pattern, L" ");

	if (parameters.baseDirectory.empty())
	{
		return std::nullopt;
	}

	parameters.searchSubFolders =
		IsDlgButtonChecked(m_hDlg, IDC_CHECK_SEARCHSUBFOLDERS) == BST_CHECKED;
	parameters.useRegularExpressions =
		IsDlgButtonChecked(m_hDlg, IDC_CHECK_USEREGULAREXPRESSIONS) == BST_CHECKED;
	parameters.caseInsensitive =
		IsDlgButtonChecked(m_hDlg, IDC_CHECK_CASEINSENSITIVE) == BST_CHECKED;

	if (IsDlgButtonChecked(m_hDlg, IDC_CHECK_ARCHIVE) == BST_CHECKED)
	{
		parameters.attributes |= FILE_ATTRIBUTE_ARCHIVE;
	}

	if (IsDlgButtonChecked(m_hDlg, IDC_CHECK_HIDDEN) == BST_CHECKED)
	{
		parameters.attributes |= FILE_ATTRIBUTE_HIDDEN;
	}

	if (IsDlgButtonChecked(m_hDlg, IDC_CHECK_READONLY) == BST_CHECKED)
	{
		parameters.attributes |= FILE_ATTRIBUTE_READONLY;
	}

	if (IsDlgButtonChecked(m_hDlg, IDC_CHECK_SYSTEM) == BST_CHECKED)
	{
		parameters.attributes |= FILE_ATTRIBUTE_SYSTEM;
	}

	if (HWND itemTypeCombo = GetDlgItem(m_hDlg, IDC_SEARCH_ITEM_TYPE))
	{
		switch (ComboBox_GetCurSel(itemTypeCombo))
		{
		case 1:
			parameters.itemType = SearchItemType::FilesOnly;
			break;

		case 2:
			parameters.itemType = SearchItemType::FoldersOnly;
			break;
		}
	}

	parameters.minSize = GetSizeFilterValue(IDC_SEARCH_MIN_SIZE, IDC_SEARCH_MIN_SIZE_UNIT);
	parameters.maxSize = GetSizeFilterValue(IDC_SEARCH_MAX_SIZE, IDC_SEARCH_MAX_SIZE_UNIT);
	parameters.modifiedAfter = GetDateFilterValue(IDC_SEARCH_MODIFIED_AFTER, false);
	parameters.modifiedBefore = GetDateFilterValue(IDC_SEARCH_MODIFIED_BEFORE, true);

	return parameters;
}

void SearchDialog::StartSearching()
{
	auto parameters = BuildSearchParameters();

	if (!parameters)
	{
		return;
	}

	ShowWindow(GetDlgItem(m_hDlg, IDC_LINK_STATUS), SW_HIDE);
	ShowWindow(GetDlgItem(m_hDlg, IDC_STATIC_STATUS), SW_SHOW);

	m_results.clear();
	m_directories.clear();
	m_displayOrder.clear();
	m_sortPending = false;

	HWND listView = GetResultsListView();
	ListView_SetItemState(listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	ListView_SetItemCountEx(listView, 0, 0);

	/* Save the search directory and search pattern (only if they are not
	the same as the most recent entry). */
	if (m_persistentSettings->m_searchDirectories.empty()
		|| parameters->baseDirectory != *m_persistentSettings->m_searchDirectories.begin())
	{
		SaveEntry(IDC_COMBO_DIRECTORY, m_persistentSettings->m_searchDirectories);
	}

	auto patternText = GetDlgItemString(m_hDlg, IDC_COMBO_NAME);

	if (m_persistentSettings->m_searchPatterns.empty()
		|| patternText != *m_persistentSettings->m_searchPatterns.begin())
	{
		SaveEntry(IDC_COMBO_NAME, m_persistentSettings->m_searchPatterns);
	}

	GetDlgItemText(m_hDlg, IDSEARCH, m_szSearchButton,
		static_cast<int>(std::size(m_szSearchButton)));

	auto stopText = m_resourceLoader->LoadString(IDS_STOP);
	SetDlgItemText(m_hDlg, IDSEARCH, stopText.c_str());

	m_searching = true;

	m_searchState = std::make_shared<SearchSharedState>();
	m_searchState->notifyWindow = m_hDlg;

	// The thread is detached. It holds its own reference to the shared state, so it's safe for the
	// dialog to be destroyed while the thread is still running. In that case, the thread will
	// stop at the next opportunity and won't post any further messages.
	std::thread([worker = std::make_shared<SearchWorker>(std::move(*parameters), m_searchState)]()
		{ worker->Run(); })
		.detach();

	SetTimer(m_hDlg, STATUS_UPDATE_TIMER_ID, STATUS_UPDATE_TIMER_ELAPSED, nullptr);
	UpdateSearchingStatus();
}

void SearchDialog::SaveEntry(int comboBoxId, boost::circular_buffer<std::wstring> &buffer)
{
	TCHAR entry[MAX_PATH];
	GetDlgItemText(m_hDlg, comboBoxId, entry, std::size(entry));

	std::wstring strEntry(entry);
	auto itr = std::find_if(buffer.begin(), buffer.end(),
		[strEntry](const std::wstring &Pattern) { return Pattern == strEntry; });

	HWND hComboBox = GetDlgItem(m_hDlg, comboBoxId);
	ComboBox_SetCurSel(hComboBox, -1);

	if (itr != buffer.end())
	{
		/* Remove the current element from both the list and the
		combo box. It will be reinserted at the front of both below. */
		auto index = std::distance(buffer.begin(), itr);
		SendMessage(hComboBox, CB_DELETESTRING, index, 0);

		buffer.erase(itr);
	}

	buffer.push_front(entry);

	SendMessage(hComboBox, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(entry));
	ComboBox_SetCurSel(hComboBox, 0);
	ComboBox_SetEditSel(hComboBox, -1, -1);

	if (static_cast<size_t>(ComboBox_GetCount(hComboBox)) > buffer.capacity())
	{
		SendMessage(hComboBox, CB_DELETESTRING, ComboBox_GetCount(hComboBox) - 1, 0);
	}
}

void SearchDialog::StopSearching()
{
	if (!m_searchState)
	{
		return;
	}

	// The finished notification will still be posted by the search thread (unless the dialog is
	// being destroyed), at which point the UI will be updated.
	m_searchState->stopRequested = true;
}

INT_PTR SearchDialog::OnDestroy()
{
	KillTimer(m_hDlg, STATUS_UPDATE_TIMER_ID);

	if (m_searchState)
	{
		std::scoped_lock lock(m_searchState->mutex);
		m_searchState->notifyWindow = nullptr;
		m_searchState->stopRequested = true;
	}

	return 0;
}

INT_PTR SearchDialog::OnPrivateMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(wParam);
	UNREFERENCED_PARAMETER(lParam);

	switch (uMsg)
	{
	case WM_APP_SEARCH_RESULTS_AVAILABLE:
		OnResultsAvailable();
		break;

	case WM_APP_SEARCH_FINISHED:
		OnSearchFinished();
		break;
	}

	return 0;
}

void SearchDialog::OnResultsAvailable()
{
	if (!m_searchState)
	{
		return;
	}

	std::vector<SearchResult> newResults;
	std::vector<std::wstring> newDirectories;

	{
		std::scoped_lock lock(m_searchState->mutex);
		newResults.swap(m_searchState->pendingResults);
		newDirectories.swap(m_searchState->pendingDirectories);
		m_searchState->resultsNotificationPending = false;
	}

	if (newResults.empty())
	{
		return;
	}

	m_directories.insert(m_directories.end(), std::make_move_iterator(newDirectories.begin()),
		std::make_move_iterator(newDirectories.end()));

	size_t firstNewIndex = m_results.size();
	m_results.insert(m_results.end(), std::make_move_iterator(newResults.begin()),
		std::make_move_iterator(newResults.end()));

	for (size_t i = firstNewIndex; i < m_results.size(); i++)
	{
		m_displayOrder.push_back(i);
	}

	// Results are appended while the search is in progress (so that the items the user is looking
	// at don't move around). They're sorted once the search has finished.
	m_sortPending = true;

	ListView_SetItemCountEx(GetResultsListView(), static_cast<int>(m_displayOrder.size()),
		LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
}

void SearchDialog::OnSearchFinished()
{
	if (!m_searchState)
	{
		return;
	}

	// Pick up any results that haven't been processed yet.
	OnResultsAvailable();

	KillTimer(m_hDlg, STATUS_UPDATE_TIMER_ID);

	bool stopped;
	bool regularExpressionInvalid;
	int foldersFound;
	int filesFound;

	{
		std::scoped_lock lock(m_searchState->mutex);
		stopped = m_searchState->stopRequested;
		regularExpressionInvalid = m_searchState->regularExpressionInvalid;
		foldersFound = m_searchState->foldersFound;
		filesFound = m_searchState->filesFound;
	}

	m_searchState.reset();

	if (regularExpressionInvalid)
	{
		/* The link/status controls are in the same position, and
		have the same size. If one of the controls is showing text,
		the other should not be visible. */
		ShowWindow(GetDlgItem(m_hDlg, IDC_LINK_STATUS), SW_SHOW);
		ShowWindow(GetDlgItem(m_hDlg, IDC_STATIC_STATUS), SW_HIDE);

		auto errorMessage = m_resourceLoader->LoadString(IDS_SEARCH_REGULAR_EXPRESSION_INVALID);
		SetDlgItemText(m_hDlg, IDC_LINK_STATUS, errorMessage.c_str());
	}
	else if (stopped)
	{
		auto cancelledMessage = m_resourceLoader->LoadString(IDS_SEARCH_CANCELLED_MESSAGE);
		SetDlgItemText(m_hDlg, IDC_STATIC_STATUS, cancelledMessage.c_str());
	}
	else
	{
		TCHAR szStatus[512];
		auto messageTemplate = m_resourceLoader->LoadString(IDS_SEARCH_FINISHED_MESSAGE);
		StringCchPrintf(szStatus, std::size(szStatus), messageTemplate.c_str(), foldersFound,
			filesFound);
		SetDlgItemText(m_hDlg, IDC_STATIC_STATUS, szStatus);
	}

	if (m_sortPending)
	{
		SortResults();
	}

	m_searching = false;
	SetDlgItemText(m_hDlg, IDSEARCH, m_szSearchButton);
}

void SearchDialog::UpdateSearchingStatus()
{
	if (!m_searchState)
	{
		return;
	}

	std::wstring currentDirectory;

	{
		std::scoped_lock lock(m_searchState->mutex);
		currentDirectory = m_searchState->currentDirectory;
	}

	TCHAR szStatus[512];
	auto messageTemplate = m_resourceLoader->LoadString(IDS_SEARCHING);
	StringCchPrintf(szStatus, std::size(szStatus), messageTemplate.c_str(),
		currentDirectory.c_str());

	std::wstring status = std::format(L"{} ({:L})", szStatus, m_results.size());
	SetDlgItemText(m_hDlg, IDC_STATIC_STATUS, status.c_str());
}

INT_PTR SearchDialog::OnTimer(int iTimerID)
{
	if (iTimerID != STATUS_UPDATE_TIMER_ID)
	{
		return 1;
	}

	UpdateSearchingStatus();

	return 0;
}

INT_PTR SearchDialog::OnClose()
{
	DestroyWindow(m_hDlg);

	return 0;
}

HWND SearchDialog::GetResultsListView() const
{
	return GetDlgItem(m_hDlg, IDC_LISTVIEW_SEARCHRESULTS);
}

std::optional<size_t> SearchDialog::GetResultAtListIndex(int index) const
{
	if (index < 0 || static_cast<size_t>(index) >= m_displayOrder.size())
	{
		return std::nullopt;
	}

	return m_displayOrder[index];
}

std::vector<size_t> SearchDialog::GetSelectedResults() const
{
	std::vector<size_t> selectedResults;
	HWND listView = GetResultsListView();
	int index = -1;

	while ((index = ListView_GetNextItem(listView, index, LVNI_SELECTED)) != -1)
	{
		if (auto result = GetResultAtListIndex(index))
		{
			selectedResults.push_back(*result);
		}
	}

	return selectedResults;
}

std::wstring SearchDialog::GetResultPath(size_t resultIndex) const
{
	const auto &result = m_results[resultIndex];
	std::wstring path = m_directories[result.directoryIndex];

	if (!path.empty() && path.back() != '\\')
	{
		path += '\\';
	}

	path += result.name;
	return path;
}

std::vector<PidlAbsolute> SearchDialog::GetPidlsForResults(const std::vector<size_t> &items) const
{
	std::vector<PidlAbsolute> pidls;

	for (size_t item : items)
	{
		PidlAbsolute pidl;
		HRESULT hr = SHParseDisplayName(GetResultPath(item).c_str(), nullptr, PidlOutParam(pidl), 0,
			nullptr);

		if (SUCCEEDED(hr))
		{
			pidls.push_back(std::move(pidl));
		}
	}

	return pidls;
}

const SearchDialog::TypeInfo &SearchDialog::GetTypeInfo(const SearchResult &result) const
{
	bool isFolder = WI_IsFlagSet(result.attributes, FILE_ATTRIBUTE_DIRECTORY);

	// The type name and generic icon are determined by the extension, so they're cached per
	// extension. That avoids touching each individual file.
	std::wstring key;

	if (isFolder)
	{
		key = L"\\folder";
	}
	else
	{
		key = PathFindExtension(result.name.c_str());
		std::transform(key.begin(), key.end(), key.begin(), towlower);
	}

	auto itr = m_typeInfoCache.find(key);

	if (itr != m_typeInfoCache.end())
	{
		return itr->second;
	}

	SHFILEINFO shfi = {};
	std::wstring fileName = isFolder ? L"folder" : L"file" + key;
	SHGetFileInfo(fileName.c_str(), isFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
		&shfi, sizeof(shfi), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX);

	auto [insertedItr, inserted] =
		m_typeInfoCache.emplace(key, TypeInfo{ shfi.szTypeName, shfi.iIcon });
	return insertedItr->second;
}

int SearchDialog::GetIconIndex(SearchResult &result)
{
	if (result.iconIndex != -1)
	{
		return result.iconIndex;
	}

	bool isFolder = WI_IsFlagSet(result.attributes, FILE_ATTRIBUTE_DIRECTORY);

	if (!isFolder && ResultHasPerFileIcon(result.name))
	{
		SHFILEINFO shfi = {};
		auto path = GetResultPath(&result - m_results.data());

		if (SHGetFileInfo(path.c_str(), 0, &shfi, sizeof(shfi), SHGFI_SYSICONINDEX))
		{
			result.iconIndex = shfi.iIcon;
			return result.iconIndex;
		}
	}

	result.iconIndex = GetTypeInfo(result).iconIndex;
	return result.iconIndex;
}

std::wstring SearchDialog::GetColumnText(const SearchResult &result, int column) const
{
	switch (static_cast<ResultsColumn>(column))
	{
	case ResultsColumn::Name:
		return result.name;

	case ResultsColumn::Path:
		return m_directories[result.directoryIndex];

	case ResultsColumn::Size:
		if (WI_IsFlagSet(result.attributes, FILE_ATTRIBUTE_DIRECTORY))
		{
			return {};
		}

		return FormatSizeString(result.size);

	case ResultsColumn::DateModified:
	{
		TCHAR dateText[128];

		if (!CreateFileTimeString(&result.lastModified, dateText, std::size(dateText), FALSE))
		{
			return {};
		}

		return dateText;
	}

	case ResultsColumn::Type:
		return GetTypeInfo(result).typeName;
	}

	return {};
}

void SearchDialog::OnGetDisplayInfo(NMLVDISPINFO *dispInfo)
{
	auto resultIndex = GetResultAtListIndex(dispInfo->item.iItem);

	if (!resultIndex)
	{
		return;
	}

	auto &result = m_results[*resultIndex];

	if (WI_IsFlagSet(dispInfo->item.mask, LVIF_TEXT))
	{
		auto text = GetColumnText(result, dispInfo->item.iSubItem);
		StringCchCopy(dispInfo->item.pszText, dispInfo->item.cchTextMax, text.c_str());
	}

	if (WI_IsFlagSet(dispInfo->item.mask, LVIF_IMAGE))
	{
		dispInfo->item.iImage = GetIconIndex(result);
	}

	if (WI_IsFlagSet(dispInfo->item.mask, LVIF_STATE))
	{
		// Hidden items are shown ghosted, as they are in the main listview.
		if (WI_IsFlagSet(result.attributes, FILE_ATTRIBUTE_HIDDEN))
		{
			dispInfo->item.state |= LVIS_CUT;
			dispInfo->item.stateMask |= LVIS_CUT;
		}
	}
}

// Supports type-ahead navigation within the results (typing the start of a name selects the
// first matching item).
int SearchDialog::OnFindItem(const NMLVFINDITEM *findItem)
{
	if (WI_IsFlagClear(findItem->lvfi.flags, LVFI_STRING)
		&& WI_IsFlagClear(findItem->lvfi.flags, LVFI_PARTIAL))
	{
		return -1;
	}

	if (!findItem->lvfi.psz)
	{
		return -1;
	}

	size_t searchLength = lstrlen(findItem->lvfi.psz);
	int numItems = static_cast<int>(m_displayOrder.size());

	if (numItems == 0)
	{
		return -1;
	}

	int startIndex = std::clamp(findItem->iStart, 0, numItems - 1);

	for (int offset = 0; offset < numItems; offset++)
	{
		int index = (startIndex + offset) % numItems;

		if (offset > 0 && index == 0 && WI_IsFlagClear(findItem->lvfi.flags, LVFI_WRAP))
		{
			break;
		}

		const auto &name = m_results[m_displayOrder[index]].name;

		if (CompareString(LOCALE_USER_DEFAULT, NORM_IGNORECASE, name.c_str(),
				static_cast<int>(std::min(name.size(), searchLength)), findItem->lvfi.psz,
				static_cast<int>(searchLength))
			== CSTR_EQUAL)
		{
			return index;
		}
	}

	return -1;
}

int SearchDialog::CompareResults(const SearchResult &result1, const SearchResult &result2) const
{
	int comparison = 0;

	switch (m_persistentSettings->m_SortMode)
	{
	case SearchDialogPersistentSettings::SortMode::Name:
		comparison = StrCmpLogicalW(result1.name.c_str(), result2.name.c_str());
		break;

	case SearchDialogPersistentSettings::SortMode::Path:
		comparison = StrCmpLogicalW(m_directories[result1.directoryIndex].c_str(),
			m_directories[result2.directoryIndex].c_str());
		break;

	case SearchDialogPersistentSettings::SortMode::Size:
	{
		// Folders are always placed together, before files.
		bool isFolder1 = WI_IsFlagSet(result1.attributes, FILE_ATTRIBUTE_DIRECTORY);
		bool isFolder2 = WI_IsFlagSet(result2.attributes, FILE_ATTRIBUTE_DIRECTORY);

		if (isFolder1 != isFolder2)
		{
			comparison = isFolder1 ? -1 : 1;
		}
		else if (result1.size != result2.size)
		{
			comparison = result1.size < result2.size ? -1 : 1;
		}
	}
	break;

	case SearchDialogPersistentSettings::SortMode::DateModified:
		comparison = CompareFileTime(&result1.lastModified, &result2.lastModified);
		break;

	case SearchDialogPersistentSettings::SortMode::Type:
		comparison = StrCmpLogicalW(GetTypeInfo(result1).typeName.c_str(),
			GetTypeInfo(result2).typeName.c_str());
		break;
	}

	// Items that are otherwise equal are ordered by name, then by path, so that the order is
	// always well-defined.
	if (comparison == 0)
	{
		comparison = StrCmpLogicalW(result1.name.c_str(), result2.name.c_str());
	}

	if (comparison == 0)
	{
		comparison = StrCmpLogicalW(m_directories[result1.directoryIndex].c_str(),
			m_directories[result2.directoryIndex].c_str());
	}

	if (!m_persistentSettings->m_bSortAscending)
	{
		comparison = -comparison;
	}

	return comparison;
}

void SearchDialog::SortResults()
{
	HWND listView = GetResultsListView();

	// The selection (and focused item) is tracked by index in a virtual listview, so it needs to
	// be remapped after sorting.
	auto selectedResults = GetSelectedResults();
	auto focusedResult = GetResultAtListIndex(ListView_GetNextItem(listView, -1, LVNI_FOCUSED));

	std::stable_sort(m_displayOrder.begin(), m_displayOrder.end(),
		[this](size_t index1, size_t index2)
		{ return CompareResults(m_results[index1], m_results[index2]) < 0; });

	m_sortPending = false;

	ListView_SetItemState(listView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);

	if (!selectedResults.empty() || focusedResult)
	{
		std::unordered_set<size_t> selectedSet(selectedResults.begin(), selectedResults.end());

		for (size_t i = 0; i < m_displayOrder.size(); i++)
		{
			UINT state = 0;

			if (selectedSet.contains(m_displayOrder[i]))
			{
				state |= LVIS_SELECTED;
			}

			if (focusedResult && m_displayOrder[i] == *focusedResult)
			{
				state |= LVIS_FOCUSED;
			}

			if (state != 0)
			{
				ListView_SetItemState(listView, static_cast<int>(i), state, state);
			}
		}
	}

	InvalidateRect(listView, nullptr, false);
	UpdateListViewHeader();
}

void SearchDialog::OnColumnClicked(int column)
{
	if (column < 0 || static_cast<size_t>(column) >= m_persistentSettings->m_Columns.size())
	{
		return;
	}

	const auto &columnInfo = m_persistentSettings->m_Columns[column];

	/* If the column clicked matches the current sort mode,
	flip the sort direction, else switch to that sort mode. */
	if (columnInfo.sortMode == m_persistentSettings->m_SortMode)
	{
		m_persistentSettings->m_bSortAscending = !m_persistentSettings->m_bSortAscending;
	}
	else
	{
		m_persistentSettings->m_SortMode = columnInfo.sortMode;
		m_persistentSettings->m_bSortAscending = columnInfo.bSortAscending;
	}

	SortResults();
}

void SearchDialog::UpdateListViewHeader()
{
	HWND hHeader = ListView_GetHeader(GetResultsListView());

	HDITEM hdItem;

	/* Remove the sort arrow from the column that was
	previously selected. */
	if (m_iPreviousSelectedColumn != -1)
	{
		hdItem.mask = HDI_FORMAT;
		Header_GetItem(hHeader, m_iPreviousSelectedColumn, &hdItem);
		hdItem.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
		Header_SetItem(hHeader, m_iPreviousSelectedColumn, &hdItem);
	}

	int iColumn = 0;

	for (const auto &ci : m_persistentSettings->m_Columns)
	{
		if (ci.sortMode == m_persistentSettings->m_SortMode)
		{
			break;
		}

		iColumn++;
	}

	hdItem.mask = HDI_FORMAT;
	Header_GetItem(hHeader, iColumn, &hdItem);

	if (m_persistentSettings->m_bSortAscending)
	{
		hdItem.fmt |= HDF_SORTUP;
	}
	else
	{
		hdItem.fmt |= HDF_SORTDOWN;
	}

	Header_SetItem(hHeader, iColumn, &hdItem);

	m_iPreviousSelectedColumn = iColumn;
}

void SearchDialog::OpenResults(const std::vector<size_t> &items)
{
	auto *browser = m_browserList->GetLastActive();

	if (!browser)
	{
		return;
	}

	size_t numOpened = 0;

	for (size_t item : items)
	{
		if (numOpened == MAX_ITEMS_TO_OPEN)
		{
			break;
		}

		browser->OpenItem(GetResultPath(item).c_str());
		numOpened++;
	}
}

void SearchDialog::OpenResultLocation(size_t resultIndex)
{
	auto *browser = m_browserList->GetLastActive();

	if (!browser)
	{
		return;
	}

	const auto &directory = m_directories[m_results[resultIndex].directoryIndex];

	PidlAbsolute directoryPidl;
	HRESULT hr =
		SHParseDisplayName(directory.c_str(), nullptr, PidlOutParam(directoryPidl), 0, nullptr);

	if (FAILED(hr))
	{
		return;
	}

	auto navigateParams = NavigateParams::Normal(directoryPidl.Raw());

	// The item itself is selected once the folder has been opened.
	auto itemPidls = GetPidlsForResults({ resultIndex });
	navigateParams.itemsToSelect = itemPidls;

	browser->GetActiveShellBrowser()->GetNavigationController()->Navigate(navigateParams);

	SetForegroundWindow(browser->GetView()->GetHWND());
}

void SearchDialog::CopyResultsToClipboard(const std::vector<size_t> &items, bool cut)
{
	auto pidls = GetPidlsForResults(items);

	if (pidls.empty())
	{
		return;
	}

	wil::com_ptr_nothrow<IDataObject> dataObject;
	CopyFilesToClipboard(m_clipboardStore, pidls,
		cut ? ClipboardAction::Cut : ClipboardAction::Copy, &dataObject);
}

void SearchDialog::CopyResultPathsToClipboard(const std::vector<size_t> &items)
{
	std::wstring paths;

	for (size_t item : items)
	{
		if (!paths.empty())
		{
			paths += L"\r\n";
		}

		paths += GetResultPath(item);
	}

	if (paths.empty())
	{
		return;
	}

	BulkClipboardWriter clipboardWriter(m_clipboardStore);
	clipboardWriter.WriteText(paths);
}

void SearchDialog::DeleteResults(const std::vector<size_t> &items, bool permanent)
{
	auto pidls = GetPidlsForResults(items);

	if (pidls.empty())
	{
		return;
	}

	std::vector<PCIDLIST_ABSOLUTE> rawPidls;

	for (const auto &pidl : pidls)
	{
		rawPidls.push_back(pidl.Raw());
	}

	FileOperations::DeleteFiles(m_hDlg, rawPidls, permanent, false);

	RemoveResultsThatNoLongerExist(items);
}

// Removes any of the specified results whose underlying item no longer exists (e.g. because it
// was deleted from the results list).
void SearchDialog::RemoveResultsThatNoLongerExist(const std::vector<size_t> &items)
{
	std::unordered_set<size_t> removedResults;

	for (size_t item : items)
	{
		if (GetFileAttributes(GetResultPath(item).c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			removedResults.insert(item);
		}
	}

	if (removedResults.empty())
	{
		return;
	}

	// Only the display order is updated. The result data itself remains in m_results, which
	// means that existing indexes into that vector remain valid.
	std::erase_if(m_displayOrder,
		[&removedResults](size_t index) { return removedResults.contains(index); });

	HWND listView = GetResultsListView();
	ListView_SetItemState(listView, -1, 0, LVIS_SELECTED);
	ListView_SetItemCountEx(listView, static_cast<int>(m_displayOrder.size()), 0);
	InvalidateRect(listView, nullptr, false);
}

void SearchDialog::StartDraggingResults(const std::vector<size_t> &items)
{
	auto pidls = GetPidlsForResults(items);

	if (pidls.empty())
	{
		return;
	}

	std::vector<PCIDLIST_ABSOLUTE> rawPidls;

	for (const auto &pidl : pidls)
	{
		rawPidls.push_back(pidl.Raw());
	}

	StartDragForShellItems(rawPidls);

	// The items may have been moved as part of the drag.
	RemoveResultsThatNoLongerExist(items);
}

void SearchDialog::SelectAllResults()
{
	ListView_SetItemState(GetResultsListView(), -1, LVIS_SELECTED, LVIS_SELECTED);
}

void SearchDialog::ShowContextMenu(const POINT &ptScreen)
{
	auto selectedResults = GetSelectedResults();

	if (selectedResults.empty())
	{
		return;
	}

	auto pidls = GetPidlsForResults(selectedResults);

	if (pidls.size() != selectedResults.size())
	{
		return;
	}

	// The full shell context menu can only be shown for items that are in the same folder.
	auto parent = pidls[0];
	bool parentRemoved = parent.RemoveLastItem();
	DCHECK(parentRemoved);

	std::vector<PidlChild> children;
	bool sameParent = true;

	for (const auto &pidl : pidls)
	{
		auto currentParent = pidl;
		currentParent.RemoveLastItem();

		if (!(currentParent == parent))
		{
			sameParent = false;
			break;
		}

		children.push_back(pidl.GetLastItem());
	}

	if (!sameParent)
	{
		ShowMixedLocationsContextMenu(ptScreen, selectedResults);
		return;
	}

	ShellItemContextMenu::Flags flags = ShellItemContextMenu::Flags::None;

	if (IsKeyDown(VK_SHIFT))
	{
		WI_SetFlag(flags, ShellItemContextMenu::Flags::ExtendedVerbs);
	}

	PopupMenuView popupMenu(m_hDlg, NoOpMenuHelpTextHost::GetInstance());
	SearchResultsItemContextMenu menu(&popupMenu, m_acceleratorManager, parent, children, nullptr,
		flags, m_browserList, m_resourceLoader);
	popupMenu.Show(ptScreen);

	// Items may have been deleted or moved via the context menu.
	RemoveResultsThatNoLongerExist(selectedResults);
}

// When the selected items are spread across multiple folders, the standard shell context menu
// can't be used. In that case, a menu containing the most common operations is shown instead.
// These operations all work with items from different folders.
void SearchDialog::ShowMixedLocationsContextMenu(const POINT &ptScreen,
	const std::vector<size_t> &items)
{
	PopupMenuView popupMenu(m_hDlg, NoOpMenuHelpTextHost::GetInstance());

	popupMenu.AppendItem(
		MenuItemParams(m_resourceLoader->LoadString(IDS_APPLICATION_CONTEXT_MENU_OPEN))
			.SetSelectedCallback([this, items] { OpenResults(items); }));

	auto focusedResult = GetResultAtListIndex(
		ListView_GetNextItem(GetResultsListView(), -1, LVNI_FOCUSED | LVNI_SELECTED));
	size_t locationResult = focusedResult ? *focusedResult : items[0];

	popupMenu.AppendItem(
		MenuItemParams(m_resourceLoader->LoadString(IDS_SEARCH_OPEN_FILE_LOCATION))
			.SetSelectedCallback([this, locationResult] { OpenResultLocation(locationResult); }));

	popupMenu.AppendSeparator();

	popupMenu.AppendItem(
		MenuItemParams(m_resourceLoader->LoadString(IDS_EDIT_CUT))
			.SetSelectedCallback([this, items] { CopyResultsToClipboard(items, true); }));
	popupMenu.AppendItem(
		MenuItemParams(m_resourceLoader->LoadString(IDS_EDIT_COPY))
			.SetSelectedCallback([this, items] { CopyResultsToClipboard(items, false); }));
	popupMenu.AppendItem(
		MenuItemParams(m_resourceLoader->LoadString(IDS_FILE_COPYITEMPATH))
			.SetSelectedCallback([this, items] { CopyResultPathsToClipboard(items); }));

	popupMenu.AppendSeparator();

	popupMenu.AppendItem(
		MenuItemParams(m_resourceLoader->LoadString(IDS_FILE_DELETE))
			.SetSelectedCallback([this, items] { DeleteResults(items, IsKeyDown(VK_SHIFT)); }));

	popupMenu.Show(ptScreen);
}

LRESULT SearchDialog::ResultsListViewSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_GETDLGCODE:
		// Enter should open the selected items, rather than activating the default button in the
		// dialog.
		if (lParam && reinterpret_cast<MSG *>(lParam)->message == WM_KEYDOWN && wParam == VK_RETURN)
		{
			return DLGC_WANTALLKEYS;
		}
		break;

	case WM_KEYDOWN:
	{
		bool ctrlDown = IsKeyDown(VK_CONTROL);
		bool shiftDown = IsKeyDown(VK_SHIFT);

		switch (wParam)
		{
		case VK_RETURN:
			OpenResults(GetSelectedResults());
			return 0;

		case VK_DELETE:
			DeleteResults(GetSelectedResults(), shiftDown);
			return 0;

		case 'A':
			if (ctrlDown)
			{
				SelectAllResults();
				return 0;
			}
			break;

		case 'C':
			if (ctrlDown && shiftDown)
			{
				CopyResultPathsToClipboard(GetSelectedResults());
				return 0;
			}
			else if (ctrlDown)
			{
				CopyResultsToClipboard(GetSelectedResults(), false);
				return 0;
			}
			break;

		case 'X':
			if (ctrlDown)
			{
				CopyResultsToClipboard(GetSelectedResults(), true);
				return 0;
			}
			break;
		}
	}
	break;

	case WM_CHAR:
		// Prevents the beep that would otherwise be played when Enter is pressed.
		if (wParam == VK_RETURN)
		{
			return 0;
		}
		break;

	case WM_CONTEXTMENU:
	{
		POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

		// The menu was invoked via the keyboard, so it's shown next to the focused item.
		if (ptScreen.x == -1 && ptScreen.y == -1)
		{
			int focusedIndex = ListView_GetNextItem(hwnd, -1, LVNI_FOCUSED | LVNI_SELECTED);
			RECT itemRect = {};

			if (focusedIndex != -1
				&& ListView_GetItemRect(hwnd, focusedIndex, &itemRect, LVIR_LABEL))
			{
				ptScreen = { itemRect.left, itemRect.bottom };
			}

			ClientToScreen(hwnd, &ptScreen);
		}

		ShowContextMenu(ptScreen);
		return 0;
	}
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

INT_PTR SearchDialog::OnNotify(NMHDR *pnmhdr)
{
	if (pnmhdr->hwndFrom == GetResultsListView())
	{
		switch (pnmhdr->code)
		{
		case LVN_GETDISPINFO:
			OnGetDisplayInfo(reinterpret_cast<NMLVDISPINFO *>(pnmhdr));
			break;

		case LVN_ODFINDITEM:
			SetWindowLongPtr(m_hDlg, DWLP_MSGRESULT,
				OnFindItem(reinterpret_cast<NMLVFINDITEM *>(pnmhdr)));
			return TRUE;

		case NM_DBLCLK:
		{
			auto *itemActivate = reinterpret_cast<NMITEMACTIVATE *>(pnmhdr);

			if (auto result = GetResultAtListIndex(itemActivate->iItem))
			{
				OpenResults({ *result });
			}
		}
		break;

		case LVN_COLUMNCLICK:
			OnColumnClicked(reinterpret_cast<NMLISTVIEW *>(pnmhdr)->iSubItem);
			break;

		case LVN_BEGINDRAG:
		case LVN_BEGINRDRAG:
			StartDraggingResults(GetSelectedResults());
			break;
		}

		return 0;
	}

	switch (pnmhdr->code)
	{
	case NM_CLICK:
	case NM_RETURN:
		if (pnmhdr->hwndFrom == GetDlgItem(m_hDlg, IDC_LINK_STATUS))
		{
			auto pnmlink = reinterpret_cast<PNMLINK>(pnmhdr);

			ShellExecute(nullptr, L"open", pnmlink->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
		}
		break;
	}

	return 0;
}

const TCHAR SearchDialogPersistentSettings::SETTINGS_KEY[] = _T("Search");

const TCHAR SearchDialogPersistentSettings::SETTING_COLUMN_WIDTH_1[] = _T("ColumnWidth1");
const TCHAR SearchDialogPersistentSettings::SETTING_COLUMN_WIDTH_2[] = _T("ColumnWidth2");
const TCHAR SearchDialogPersistentSettings::SETTING_COLUMN_WIDTH_3[] = _T("ColumnWidth3");
const TCHAR SearchDialogPersistentSettings::SETTING_COLUMN_WIDTH_4[] = _T("ColumnWidth4");
const TCHAR SearchDialogPersistentSettings::SETTING_COLUMN_WIDTH_5[] = _T("ColumnWidth5");
const TCHAR SearchDialogPersistentSettings::SETTING_ITEM_TYPE[] = _T("ItemType");
const TCHAR SearchDialogPersistentSettings::SETTING_SEARCH_DIRECTORY_TEXT[] =
	_T("SearchDirectoryText");
const TCHAR SearchDialogPersistentSettings::SETTING_SEARCH_SUB_FOLDERS[] = _T("SearchSubFolders");
const TCHAR SearchDialogPersistentSettings::SETTING_USE_REGULAR_EXPRESSIONS[] =
	_T("UseRegularExpressions");
const TCHAR SearchDialogPersistentSettings::SETTING_CASE_INSENSITIVE[] = _T("CaseInsensitive");
const TCHAR SearchDialogPersistentSettings::SETTING_ARCHIVE[] = _T("Archive");
const TCHAR SearchDialogPersistentSettings::SETTING_HIDDEN[] = _T("Hidden");
const TCHAR SearchDialogPersistentSettings::SETTING_READ_ONLY[] = _T("ReadOnly");
const TCHAR SearchDialogPersistentSettings::SETTING_SYSTEM[] = _T("System");
const TCHAR SearchDialogPersistentSettings::SETTING_SORT_MODE[] = _T("SortMode");
const TCHAR SearchDialogPersistentSettings::SETTING_SORT_ASCENDING[] = _T("SortAscending");
const TCHAR SearchDialogPersistentSettings::SETTING_DIRECTORY_LIST[] = _T("Directory");
const TCHAR SearchDialogPersistentSettings::SETTING_PATTERN_LIST[] = _T("Pattern");

void SearchDialog::SaveState()
{
	m_persistentSettings->SaveDialogPosition(m_hDlg);

	m_persistentSettings->m_bCaseInsensitive =
		IsDlgButtonChecked(m_hDlg, IDC_CHECK_CASEINSENSITIVE) == BST_CHECKED;

	m_persistentSettings->m_bUseRegularExpressions =
		IsDlgButtonChecked(m_hDlg, IDC_CHECK_USEREGULAREXPRESSIONS) == BST_CHECKED;

	m_persistentSettings->m_bSearchSubFolders =
		IsDlgButtonChecked(m_hDlg, IDC_CHECK_SEARCHSUBFOLDERS) == BST_CHECKED;

	m_persistentSettings->m_bArchive = IsDlgButtonChecked(m_hDlg, IDC_CHECK_ARCHIVE) == BST_CHECKED;

	m_persistentSettings->m_bHidden = IsDlgButtonChecked(m_hDlg, IDC_CHECK_HIDDEN) == BST_CHECKED;

	m_persistentSettings->m_bReadOnly =
		IsDlgButtonChecked(m_hDlg, IDC_CHECK_READONLY) == BST_CHECKED;

	m_persistentSettings->m_bSystem = IsDlgButtonChecked(m_hDlg, IDC_CHECK_SYSTEM) == BST_CHECKED;

	if (HWND itemTypeCombo = GetDlgItem(m_hDlg, IDC_SEARCH_ITEM_TYPE))
	{
		auto selection = ComboBox_GetCurSel(itemTypeCombo);

		if (selection != CB_ERR)
		{
			m_persistentSettings->m_itemType = selection;
		}
	}

	HWND listView = GetResultsListView();

	for (int i = 0; i < SearchDialogPersistentSettings::NUM_COLUMNS; i++)
	{
		m_persistentSettings->m_columnWidths[i] = ListView_GetColumnWidth(listView, i);
	}

	m_persistentSettings->m_searchPattern = GetDlgItemString(m_hDlg, IDC_COMBO_NAME);

	m_persistentSettings->m_bStateSaved = TRUE;
}

SearchDialogPersistentSettings::SearchDialogPersistentSettings() :
	DialogSettings(SETTINGS_KEY),
	m_searchPatterns(DialogConstants::DEFAULT_HISTORY_SIZE),
	m_searchDirectories(DialogConstants::DEFAULT_HISTORY_SIZE)
{
	m_bSearchSubFolders = TRUE;
	m_bUseRegularExpressions = FALSE;
	m_bCaseInsensitive = FALSE;
	m_bArchive = FALSE;
	m_bHidden = FALSE;
	m_bReadOnly = FALSE;
	m_bSystem = FALSE;
	m_itemType = static_cast<int>(SearchItemType::FilesAndFolders);
	m_columnWidths.fill(-1);

	ColumnInfo ci;
	ci.sortMode = SortMode::Name;
	ci.uStringID = IDS_SEARCH_COLUMN_NAME;
	ci.bSortAscending = true;
	m_Columns.push_back(ci);

	ci.sortMode = SortMode::Path;
	ci.uStringID = IDS_SEARCH_COLUMN_PATH;
	ci.bSortAscending = true;
	m_Columns.push_back(ci);

	ci.sortMode = SortMode::Size;
	ci.uStringID = IDS_COLUMN_NAME_SIZE;
	ci.bSortAscending = false;
	m_Columns.push_back(ci);

	ci.sortMode = SortMode::DateModified;
	ci.uStringID = IDS_COLUMN_NAME_DATEMODIFIED;
	ci.bSortAscending = false;
	m_Columns.push_back(ci);

	ci.sortMode = SortMode::Type;
	ci.uStringID = IDS_COLUMN_NAME_TYPE;
	ci.bSortAscending = true;
	m_Columns.push_back(ci);

	m_SortMode = m_Columns.front().sortMode;
	m_bSortAscending = m_Columns.front().bSortAscending;
}

SearchDialogPersistentSettings &SearchDialogPersistentSettings::GetInstance()
{
	static SearchDialogPersistentSettings sdps;
	return sdps;
}

void SearchDialogPersistentSettings::SaveExtraRegistrySettings(HKEY hKey)
{
	RegistrySettings::SaveDword(hKey, SETTING_COLUMN_WIDTH_1, m_columnWidths[0]);
	RegistrySettings::SaveDword(hKey, SETTING_COLUMN_WIDTH_2, m_columnWidths[1]);
	RegistrySettings::SaveDword(hKey, SETTING_COLUMN_WIDTH_3, m_columnWidths[2]);
	RegistrySettings::SaveDword(hKey, SETTING_COLUMN_WIDTH_4, m_columnWidths[3]);
	RegistrySettings::SaveDword(hKey, SETTING_COLUMN_WIDTH_5, m_columnWidths[4]);
	RegistrySettings::SaveDword(hKey, SETTING_ITEM_TYPE, m_itemType);
	RegistrySettings::SaveString(hKey, SETTING_SEARCH_DIRECTORY_TEXT, m_searchPattern);
	RegistrySettings::SaveDword(hKey, SETTING_SEARCH_SUB_FOLDERS, m_bSearchSubFolders);
	RegistrySettings::SaveDword(hKey, SETTING_USE_REGULAR_EXPRESSIONS, m_bUseRegularExpressions);
	RegistrySettings::SaveDword(hKey, SETTING_CASE_INSENSITIVE, m_bCaseInsensitive);
	RegistrySettings::SaveDword(hKey, SETTING_ARCHIVE, m_bArchive);
	RegistrySettings::SaveDword(hKey, SETTING_HIDDEN, m_bHidden);
	RegistrySettings::SaveDword(hKey, SETTING_READ_ONLY, m_bReadOnly);
	RegistrySettings::SaveDword(hKey, SETTING_SYSTEM, m_bSystem);
	RegistrySettings::SaveDword(hKey, SETTING_SORT_MODE, static_cast<DWORD>(m_SortMode));
	RegistrySettings::SaveDword(hKey, SETTING_SORT_ASCENDING, m_bSortAscending);

	std::list<std::wstring> searchDirectoriesList;
	CircularBufferToList(m_searchDirectories, searchDirectoriesList);
	RegistrySettings::SaveStringList(hKey, SETTING_DIRECTORY_LIST, searchDirectoriesList);

	std::list<std::wstring> searchPatternList;
	CircularBufferToList(m_searchPatterns, searchPatternList);
	RegistrySettings::SaveStringList(hKey, SETTING_PATTERN_LIST, searchPatternList);
}

void SearchDialogPersistentSettings::LoadExtraRegistrySettings(HKEY hKey)
{
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_COLUMN_WIDTH_1, m_columnWidths[0]);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_COLUMN_WIDTH_2, m_columnWidths[1]);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_COLUMN_WIDTH_3, m_columnWidths[2]);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_COLUMN_WIDTH_4, m_columnWidths[3]);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_COLUMN_WIDTH_5, m_columnWidths[4]);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_ITEM_TYPE, m_itemType);
	RegistrySettings::ReadString(hKey, SETTING_SEARCH_DIRECTORY_TEXT, m_searchPattern);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_SEARCH_SUB_FOLDERS,
		m_bSearchSubFolders);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_USE_REGULAR_EXPRESSIONS,
		m_bUseRegularExpressions);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_CASE_INSENSITIVE,
		m_bCaseInsensitive);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_ARCHIVE, m_bArchive);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_HIDDEN, m_bHidden);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_READ_ONLY, m_bReadOnly);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_SYSTEM, m_bSystem);
	RegistrySettings::Read32BitValueFromRegistry(hKey, SETTING_SORT_ASCENDING, m_bSortAscending);

	DWORD value;
	RegistrySettings::ReadDword(hKey, SETTING_SORT_MODE, value);
	m_SortMode = static_cast<SortMode>(value);

	std::list<std::wstring> searchDirectoriesList;
	RegistrySettings::ReadStringList(hKey, SETTING_DIRECTORY_LIST, searchDirectoriesList);
	ListToCircularBuffer(searchDirectoriesList, m_searchDirectories);

	std::list<std::wstring> searchPatternList;
	RegistrySettings::ReadStringList(hKey, SETTING_PATTERN_LIST, searchPatternList);
	ListToCircularBuffer(searchPatternList, m_searchPatterns);
}

void SearchDialogPersistentSettings::SaveExtraXMLSettings(IXMLDOMDocument *pXMLDom,
	IXMLDOMElement *pParentNode)
{
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_COLUMN_WIDTH_1,
		XMLSettings::EncodeIntValue(m_columnWidths[0]));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_COLUMN_WIDTH_2,
		XMLSettings::EncodeIntValue(m_columnWidths[1]));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_COLUMN_WIDTH_3,
		XMLSettings::EncodeIntValue(m_columnWidths[2]));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_COLUMN_WIDTH_4,
		XMLSettings::EncodeIntValue(m_columnWidths[3]));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_COLUMN_WIDTH_5,
		XMLSettings::EncodeIntValue(m_columnWidths[4]));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_ITEM_TYPE,
		XMLSettings::EncodeIntValue(m_itemType));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_SEARCH_DIRECTORY_TEXT,
		m_searchPattern.c_str());
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_SEARCH_SUB_FOLDERS,
		XMLSettings::EncodeBoolValue(m_bSearchSubFolders));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_USE_REGULAR_EXPRESSIONS,
		XMLSettings::EncodeBoolValue(m_bUseRegularExpressions));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_CASE_INSENSITIVE,
		XMLSettings::EncodeBoolValue(m_bCaseInsensitive));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_ARCHIVE,
		XMLSettings::EncodeBoolValue(m_bArchive));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_HIDDEN,
		XMLSettings::EncodeBoolValue(m_bHidden));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_READ_ONLY,
		XMLSettings::EncodeBoolValue(m_bReadOnly));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_SYSTEM,
		XMLSettings::EncodeBoolValue(m_bSystem));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_SORT_MODE,
		XMLSettings::EncodeIntValue(static_cast<int>(m_SortMode)));
	XMLSettings::AddAttributeToNode(pXMLDom, pParentNode, SETTING_SORT_ASCENDING,
		XMLSettings::EncodeBoolValue(m_bSortAscending));

	std::list<std::wstring> searchDirectoriesList;
	CircularBufferToList(m_searchDirectories, searchDirectoriesList);
	XMLSettings::AddStringListToNode(pXMLDom, pParentNode, SETTING_DIRECTORY_LIST,
		searchDirectoriesList);

	std::list<std::wstring> searchPatternList;
	CircularBufferToList(m_searchPatterns, searchPatternList);
	XMLSettings::AddStringListToNode(pXMLDom, pParentNode, SETTING_PATTERN_LIST, searchPatternList);
}

void SearchDialogPersistentSettings::LoadExtraXMLSettings(BSTR bstrName, BSTR bstrValue)
{
	if (lstrcmpi(bstrName, SETTING_COLUMN_WIDTH_1) == 0)
	{
		m_columnWidths[0] = XMLSettings::DecodeIntValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_COLUMN_WIDTH_2) == 0)
	{
		m_columnWidths[1] = XMLSettings::DecodeIntValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_COLUMN_WIDTH_3) == 0)
	{
		m_columnWidths[2] = XMLSettings::DecodeIntValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_COLUMN_WIDTH_4) == 0)
	{
		m_columnWidths[3] = XMLSettings::DecodeIntValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_COLUMN_WIDTH_5) == 0)
	{
		m_columnWidths[4] = XMLSettings::DecodeIntValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_ITEM_TYPE) == 0)
	{
		m_itemType = XMLSettings::DecodeIntValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_SEARCH_DIRECTORY_TEXT) == 0)
	{
		m_searchPattern = bstrValue;
	}
	else if (lstrcmpi(bstrName, SETTING_SEARCH_SUB_FOLDERS) == 0)
	{
		m_bSearchSubFolders = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_USE_REGULAR_EXPRESSIONS) == 0)
	{
		m_bUseRegularExpressions = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_CASE_INSENSITIVE) == 0)
	{
		m_bCaseInsensitive = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_ARCHIVE) == 0)
	{
		m_bArchive = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_HIDDEN) == 0)
	{
		m_bHidden = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_READ_ONLY) == 0)
	{
		m_bReadOnly = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_SYSTEM) == 0)
	{
		m_bSystem = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (lstrcmpi(bstrName, SETTING_SORT_MODE) == 0)
	{
		m_SortMode = static_cast<SortMode>(XMLSettings::DecodeIntValue(bstrValue));
	}
	else if (lstrcmpi(bstrName, SETTING_SORT_ASCENDING) == 0)
	{
		m_bSortAscending = XMLSettings::DecodeBoolValue(bstrValue);
	}
	else if (CompareString(LOCALE_INVARIANT, NORM_IGNORECASE, bstrName,
				 lstrlen(SETTING_DIRECTORY_LIST), SETTING_DIRECTORY_LIST,
				 lstrlen(SETTING_DIRECTORY_LIST))
		== CSTR_EQUAL)
	{
		m_searchDirectories.push_back(bstrValue);
	}
	else if (CompareString(LOCALE_INVARIANT, NORM_IGNORECASE, bstrName,
				 lstrlen(SETTING_PATTERN_LIST), SETTING_PATTERN_LIST, lstrlen(SETTING_PATTERN_LIST))
		== CSTR_EQUAL)
	{
		m_searchPatterns.push_back(bstrValue);
	}
}

template <typename T>
void SearchDialogPersistentSettings::CircularBufferToList(const boost::circular_buffer<T> &cb,
	std::list<T> &list)
{
	for (const auto &item : cb)
	{
		list.push_back(item);
	}
}

template <typename T>
void SearchDialogPersistentSettings::ListToCircularBuffer(const std::list<T> &list,
	boost::circular_buffer<T> &cb)
{
	for (const auto &item : list)
	{
		cb.push_back(item);
	}
}
