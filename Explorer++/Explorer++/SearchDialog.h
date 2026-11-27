// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#pragma once

#include "BaseDialog.h"
#include "../Helper/DialogSettings.h"
#include "../Helper/Pidl.h"
#include <boost/circular_buffer.hpp>
#include <MsXml2.h>
#include <objbase.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AcceleratorManager;
class BrowserList;
class ClipboardStore;
class ResourceLoader;
class SearchDialog;
class WindowSubclass;

/* Manages settings for the search dialog. */
class SearchDialogPersistentSettings : public DialogSettings
{
public:
	static SearchDialogPersistentSettings &GetInstance();

private:
	friend SearchDialog;

	static const TCHAR SETTINGS_KEY[];

	static constexpr int NUM_COLUMNS = 5;

	static const TCHAR SETTING_COLUMN_WIDTH_1[];
	static const TCHAR SETTING_COLUMN_WIDTH_2[];
	static const TCHAR SETTING_COLUMN_WIDTH_3[];
	static const TCHAR SETTING_COLUMN_WIDTH_4[];
	static const TCHAR SETTING_COLUMN_WIDTH_5[];
	static const TCHAR SETTING_ITEM_TYPE[];
	static const TCHAR SETTING_SEARCH_DIRECTORY_TEXT[];
	static const TCHAR SETTING_SEARCH_SUB_FOLDERS[];
	static const TCHAR SETTING_USE_REGULAR_EXPRESSIONS[];
	static const TCHAR SETTING_CASE_INSENSITIVE[];
	static const TCHAR SETTING_ARCHIVE[];
	static const TCHAR SETTING_HIDDEN[];
	static const TCHAR SETTING_READ_ONLY[];
	static const TCHAR SETTING_SYSTEM[];
	static const TCHAR SETTING_DIRECTORY_LIST[];
	static const TCHAR SETTING_PATTERN_LIST[];
	static const TCHAR SETTING_SORT_MODE[];
	static const TCHAR SETTING_SORT_ASCENDING[];

	enum class SortMode
	{
		Name = 1,
		Path = 2,
		Size = 3,
		DateModified = 4,
		Type = 5
	};

	struct ColumnInfo
	{
		SortMode sortMode;
		UINT uStringID;

		/* Indicates whether the sort direction
		for this column should be ascending by
		default. */
		bool bSortAscending;
	};

	SearchDialogPersistentSettings();

	SearchDialogPersistentSettings(const SearchDialogPersistentSettings &);
	SearchDialogPersistentSettings &operator=(const SearchDialogPersistentSettings &);

	void SaveExtraRegistrySettings(HKEY hKey) override;
	void LoadExtraRegistrySettings(HKEY hKey) override;

	void SaveExtraXMLSettings(IXMLDOMDocument *pXMLDom, IXMLDOMElement *pParentNode) override;
	void LoadExtraXMLSettings(BSTR bstrName, BSTR bstrValue) override;

	template <typename T>
	void CircularBufferToList(const boost::circular_buffer<T> &cb, std::list<T> &list);
	template <typename T>
	void ListToCircularBuffer(const std::list<T> &list, boost::circular_buffer<T> &cb);

	std::wstring m_searchPattern;
	boost::circular_buffer<std::wstring> m_searchPatterns;
	boost::circular_buffer<std::wstring> m_searchDirectories;
	BOOL m_bSearchSubFolders;
	BOOL m_bUseRegularExpressions;
	BOOL m_bCaseInsensitive;
	BOOL m_bArchive;
	BOOL m_bHidden;
	BOOL m_bReadOnly;
	BOOL m_bSystem;

	std::vector<ColumnInfo> m_Columns;
	SortMode m_SortMode;
	BOOL m_bSortAscending;

	// Which kinds of items are included in the results (see SearchItemType).
	int m_itemType;

	// A value of -1 indicates that no width has been saved for the column.
	std::array<int, NUM_COLUMNS> m_columnWidths;
};

// Which kinds of items should be included in the search results.
enum class SearchItemType
{
	FilesAndFolders = 0,
	FilesOnly = 1,
	FoldersOnly = 2
};

struct SearchParameters
{
	std::wstring baseDirectory;
	std::wstring pattern;
	DWORD attributes = 0;
	bool useRegularExpressions = false;
	bool caseInsensitive = false;
	bool searchSubFolders = true;
	SearchItemType itemType = SearchItemType::FilesAndFolders;
	std::optional<uint64_t> minSize;
	std::optional<uint64_t> maxSize;

	// UTC times. Items are matched if their last modified time is within [modifiedAfter,
	// modifiedBefore).
	std::optional<FILETIME> modifiedAfter;
	std::optional<FILETIME> modifiedBefore;
};

// A single item found during a search. The directory is stored separately (and shared between all
// the items in that directory), to reduce memory usage when there are a large number of results.
struct SearchResult
{
	std::wstring name;
	uint32_t directoryIndex;
	DWORD attributes;
	uint64_t size;
	FILETIME lastModified;

	// Lazily retrieved, the first time the item is shown.
	int iconIndex = -1;
};

// State shared between the dialog and the background search thread. The thread holds a reference
// to this object, so that it remains valid even if the dialog is closed while a search is in
// progress.
struct SearchSharedState
{
	std::mutex mutex;

	// The window that's notified when results are available or when the search finishes. Set to
	// nullptr when the dialog is destroyed.
	HWND notifyWindow = nullptr;

	std::atomic<bool> stopRequested = false;

	// Results that have been found, but not yet picked up by the dialog.
	std::vector<SearchResult> pendingResults;
	std::vector<std::wstring> pendingDirectories;
	bool resultsNotificationPending = false;

	std::wstring currentDirectory;
	int foldersFound = 0;
	int filesFound = 0;
	bool regularExpressionInvalid = false;
};

class SearchDialog : public BaseDialog
{
public:
	static SearchDialog *Create(const ResourceLoader *resourceLoader, HWND hParent,
		std::wstring_view searchDirectory, BrowserList *browserList,
		const AcceleratorManager *acceleratorManager, ClipboardStore *clipboardStore);

protected:
	INT_PTR OnInitDialog() override;
	INT_PTR OnTimer(int iTimerID) override;
	INT_PTR OnCommand(WPARAM wParam, LPARAM lParam) override;
	INT_PTR OnNotify(NMHDR *pnmhdr) override;
	INT_PTR OnClose() override;
	INT_PTR OnDestroy() override;

	INT_PTR OnPrivateMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

private:
	static const int STATUS_UPDATE_TIMER_ID = 1;
	static const int STATUS_UPDATE_TIMER_ELAPSED = 200;

	struct TypeInfo
	{
		std::wstring typeName;
		int iconIndex;
	};

	SearchDialog(const ResourceLoader *resourceLoader, HWND hParent,
		std::wstring_view searchDirectory, BrowserList *browserList,
		const AcceleratorManager *acceleratorManager, ClipboardStore *clipboardStore);
	~SearchDialog();

	void AddDynamicControls() override;
	std::vector<ResizableDialogControl> GetResizableControls() override;
	void SaveState() override;

	void SetUpFilterControls();
	std::optional<SearchParameters> BuildSearchParameters();
	std::optional<uint64_t> GetSizeFilterValue(int editId, int unitId);
	std::optional<FILETIME> GetDateFilterValue(int dateTimePickerId, bool endOfDay);

	void OnBrowserForFolder();
	void OnSearch();
	void StartSearching();
	void StopSearching();
	void OnResultsAvailable();
	void OnSearchFinished();
	void UpdateSearchingStatus();
	void SaveEntry(int comboBoxId, boost::circular_buffer<std::wstring> &buffer);

	// Results listview.
	HWND GetResultsListView() const;
	LRESULT ResultsListViewSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void OnGetDisplayInfo(NMLVDISPINFO *dispInfo);
	int OnFindItem(const NMLVFINDITEM *findItem);
	void OnColumnClicked(int column);
	void SortResults();
	int CompareResults(const SearchResult &result1, const SearchResult &result2) const;
	void UpdateListViewHeader();
	void ShowContextMenu(const POINT &ptScreen);
	void ShowMixedLocationsContextMenu(const POINT &ptScreen, const std::vector<size_t> &items);

	// Actions on the results.
	std::vector<size_t> GetSelectedResults() const;
	std::optional<size_t> GetResultAtListIndex(int index) const;
	std::wstring GetResultPath(size_t resultIndex) const;
	std::vector<PidlAbsolute> GetPidlsForResults(const std::vector<size_t> &items) const;
	void OpenResults(const std::vector<size_t> &items);
	void OpenResultLocation(size_t resultIndex);
	void CopyResultsToClipboard(const std::vector<size_t> &items, bool cut);
	void CopyResultPathsToClipboard(const std::vector<size_t> &items);
	void DeleteResults(const std::vector<size_t> &items, bool permanent);
	void StartDraggingResults(const std::vector<size_t> &items);
	void RemoveResultsThatNoLongerExist(const std::vector<size_t> &items);
	void SelectAllResults();

	const TypeInfo &GetTypeInfo(const SearchResult &result) const;
	int GetIconIndex(SearchResult &result);
	std::wstring GetColumnText(const SearchResult &result, int column) const;

	std::wstring m_searchDirectory;
	BrowserList *const m_browserList;
	const AcceleratorManager *const m_acceleratorManager;
	ClipboardStore *const m_clipboardStore;
	wil::unique_hicon m_directoryIcon;
	bool m_searching = false;
	TCHAR m_szSearchButton[32];

	std::shared_ptr<SearchSharedState> m_searchState;

	// All results found so far. Results are only ever appended to this vector (or removed, if the
	// underlying item is deleted). The display order is determined by m_displayOrder, which
	// contains indexes into m_results.
	std::vector<SearchResult> m_results;
	std::vector<std::wstring> m_directories;
	std::vector<size_t> m_displayOrder;

	// True if results have been appended (in an unsorted order) since the last sort.
	bool m_sortPending = false;

	mutable std::unordered_map<std::wstring, TypeInfo> m_typeInfoCache;

	std::unique_ptr<WindowSubclass> m_listViewSubclass;
	int m_iPreviousSelectedColumn = -1;

	SearchDialogPersistentSettings *m_persistentSettings = nullptr;
};
