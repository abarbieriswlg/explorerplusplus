// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "Bookmarks/BookmarkHelper.h"
#include "AppInfo.h"
#include "Bookmarks/BookmarkClipboard.h"
#include "Bookmarks/BookmarkDataExchange.h"
#include "Bookmarks/BookmarkTree.h"
#include "Bookmarks/UI/AddBookmarkDialog.h"
#include "BrowserView.h"
#include "BrowserWindow.h"
#include "FmtWrapper.h"
#include "MainResource.h"
#include "ResourceLoader.h"
#include "ShellBrowser/ShellBrowser.h"
#include "TabBar.h"
#include "../Helper/ShellHelper.h"
#include <boost/range/adaptor/filtered.hpp>
#include <fmt/xchar.h>
#include <glog/logging.h>
#include <algorithm>
#include <unordered_map>

namespace
{

enum class LocationStatus
{
	Available,
	NotFound,
	Unavailable
};

// Removals are performed asynchronously (once the current message has been processed). That's
// because a bookmark is typically opened from a UI element that represents the bookmark (e.g. a
// toolbar button or menu item) and removing the bookmark would destroy that element while its
// click handler is still running.
struct PendingBookmarkRemoval
{
	BookmarkTree *bookmarkTree;
	WeakPtr<BookmarkItem> bookmarkItem;
};

std::unordered_map<UINT_PTR, PendingBookmarkRemoval> g_pendingBookmarkRemovals;

void CALLBACK RemoveBookmarkTimerProc(HWND hwnd, UINT msg, UINT_PTR timerId, DWORD time)
{
	UNREFERENCED_PARAMETER(hwnd);
	UNREFERENCED_PARAMETER(msg);
	UNREFERENCED_PARAMETER(time);

	KillTimer(nullptr, timerId);

	auto itr = g_pendingBookmarkRemovals.find(timerId);

	if (itr == g_pendingBookmarkRemovals.end())
	{
		return;
	}

	auto removal = itr->second;
	g_pendingBookmarkRemovals.erase(itr);

	// The bookmark may have been removed in the meantime.
	if (removal.bookmarkItem)
	{
		removal.bookmarkTree->RemoveBookmarkItem(removal.bookmarkItem.Get());
	}
}

void RemoveBookmarkAsync(BookmarkTree *bookmarkTree, BookmarkItem *bookmarkItem)
{
	UINT_PTR timerId = SetTimer(nullptr, 0, 0, RemoveBookmarkTimerProc);

	if (timerId == 0)
	{
		return;
	}

	g_pendingBookmarkRemovals.insert_or_assign(timerId,
		PendingBookmarkRemoval{ bookmarkTree, bookmarkItem->GetWeakPtr() });
}

// Determines whether a file system location exists. Only paths that refer to the file system
// (e.g. C:\Folder or \\server\share\folder) are checked. Other locations (e.g. virtual
// folders) are always treated as being available.
LocationStatus GetLocationStatus(const std::wstring &path)
{
	bool isFileSystemPath = PathIsUNC(path.c_str()) || PathGetDriveNumber(path.c_str()) != -1;

	if (!isFileSystemPath)
	{
		return LocationStatus::Available;
	}

	if (GetFileAttributes(path.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		return LocationStatus::Available;
	}

	switch (GetLastError())
	{
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
		return LocationStatus::NotFound;

	case ERROR_ACCESS_DENIED:
		// The item exists, but can't be accessed. Opening the location will show the appropriate
		// error.
		return LocationStatus::Available;

	default:
		// For example, ERROR_BAD_NETPATH or ERROR_NOT_READY (when a removable drive isn't
		// present). The bookmark may well be valid, so it shouldn't be suggested that it be
		// removed.
		return LocationStatus::Unavailable;
	}
}

// Returns true if the location exists (or isn't a file system location). Otherwise, an
// appropriate message will be shown (with the option to remove the bookmark, if the location
// doesn't exist).
bool CheckBookmarkLocation(const BookmarkItem *bookmarkItem, const std::wstring &path,
	BrowserWindow *browser, BookmarkTree *bookmarkTree, const ResourceLoader *resourceLoader)
{
	auto status = GetLocationStatus(path);

	if (status == LocationStatus::Available)
	{
		return true;
	}

	HWND parentWindow = browser->GetView()->GetHWND();

	if (status == LocationStatus::NotFound)
	{
		auto message =
			fmt::format(fmt::runtime(resourceLoader->LoadString(IDS_BOOKMARK_LOCATION_NOT_FOUND)),
				fmt::arg(L"name", bookmarkItem->GetName()), fmt::arg(L"location", path));
		int response = MessageBox(parentWindow, message.c_str(), AppInfo::NAME,
			MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);

		if (response == IDYES && !bookmarkTree->IsPermanentNode(bookmarkItem))
		{
			RemoveBookmarkAsync(bookmarkTree, const_cast<BookmarkItem *>(bookmarkItem));
		}
	}
	else
	{
		auto message =
			fmt::format(fmt::runtime(resourceLoader->LoadString(IDS_BOOKMARK_LOCATION_UNAVAILABLE)),
				fmt::arg(L"name", bookmarkItem->GetName()), fmt::arg(L"location", path));
		MessageBox(parentWindow, message.c_str(), AppInfo::NAME, MB_ICONWARNING | MB_OK);
	}

	return false;
}

void OpenBookmarkWithDisposition(const BookmarkItem *bookmarkItem,
	OpenFolderDisposition disposition, const std::wstring &currentDirectory, BrowserWindow *browser,
	BookmarkTree *bookmarkTree, const ResourceLoader *resourceLoader, bool checkLocation)
{
	DCHECK(bookmarkItem->IsBookmark());

	auto absolutePath = TransformUserEnteredPathToAbsolutePathAndNormalize(
		bookmarkItem->GetLocation(), currentDirectory, EnvVarsExpansion::Expand);

	if (!absolutePath)
	{
		return;
	}

	// Previously, opening a bookmark whose location no longer existed would silently do nothing.
	if (checkLocation
		&& !CheckBookmarkLocation(bookmarkItem, *absolutePath, browser, bookmarkTree,
			resourceLoader))
	{
		return;
	}

	browser->OpenItem(*absolutePath, disposition);
}

}

namespace BookmarkHelper
{

bool IsFolder(const std::unique_ptr<BookmarkItem> &bookmarkItem)
{
	return bookmarkItem->IsFolder();
}

bool IsBookmark(const std::unique_ptr<BookmarkItem> &bookmarkItem)
{
	return bookmarkItem->IsBookmark();
}

void BookmarkAllTabs(BookmarkTree *bookmarkTree, BrowserWindow *browser,
	PlatformContext *platformContext, const AcceleratorManager *acceleratorManager,
	const ResourceLoader *resourceLoader)
{
	std::wstring bookmarkAllTabsText =
		resourceLoader->LoadString(IDS_ADD_BOOKMARK_TITLE_BOOKMARK_ALL_TABS);
	auto bookmarkFolder = AddBookmarkItem(bookmarkTree, BookmarkItem::Type::Folder, nullptr,
		std::nullopt, browser->GetView()->GetHWND(), browser, acceleratorManager, resourceLoader,
		platformContext, bookmarkAllTabsText);

	if (!bookmarkFolder)
	{
		return;
	}

	AddBrowserTabsToBookmarkFolder(bookmarkTree, browser, bookmarkFolder);
}

void AddBrowserTabsToBookmarkFolder(BookmarkTree *bookmarkTree, BrowserWindow *browser,
	BookmarkItem *bookmarkFolder)
{
	DCHECK(bookmarkFolder->IsFolder());

	for (const auto *tab : browser->GetActiveTabBar()->GetAllTabsInOrder())
	{
		const auto &pidl = tab->GetShellBrowser()->GetDirectory();
		auto bookmark =
			std::make_unique<BookmarkItem>(GetDisplayNameWithFallback(pidl.Raw(), SHGDN_INFOLDER),
				GetDisplayNameWithFallback(pidl.Raw(), SHGDN_FORPARSING));

		bookmarkTree->AddBookmarkItem(bookmarkFolder, std::move(bookmark));
	}
}

BookmarkItem *AddBookmarkItem(BookmarkTree *bookmarkTree, BookmarkItem::Type type,
	BookmarkItem *defaultParentSelection, std::optional<size_t> suggestedIndex, HWND parentWindow,
	BrowserWindow *browser, const AcceleratorManager *acceleratorManager,
	const ResourceLoader *resourceLoader, PlatformContext *platformContext,
	std::optional<std::wstring> customDialogTitle)
{
	std::unique_ptr<BookmarkItem> bookmarkItem;

	if (type == BookmarkItem::Type::Bookmark)
	{
		std::wstring name;
		std::wstring location;

		if (browser)
		{
			const auto *shellBrowser = browser->GetActiveShellBrowser();
			const auto &directory = shellBrowser->GetDirectory();

			name = GetDisplayNameWithFallback(directory.Raw(), SHGDN_INFOLDER);
			location = GetDisplayNameWithFallback(directory.Raw(), SHGDN_FORPARSING);
		}

		bookmarkItem = std::make_unique<BookmarkItem>(name, location);
	}
	else
	{
		bookmarkItem = std::make_unique<BookmarkItem>(
			resourceLoader->LoadString(IDS_BOOKMARKS_NEWBOOKMARKFOLDER));
	}

	BookmarkItem *rawBookmarkItem = bookmarkItem.get();
	BookmarkItem *selectedParentFolder = nullptr;

	auto *addBookmarkDialog = AddBookmarkDialog::Create(resourceLoader, parentWindow, bookmarkTree,
		bookmarkItem.get(), defaultParentSelection, &selectedParentFolder, acceleratorManager,
		platformContext, customDialogTitle);
	auto res = addBookmarkDialog->ShowModalDialog();

	if (res == BaseDialog::RETURN_OK)
	{
		DCHECK(selectedParentFolder);

		size_t targetIndex;

		if (selectedParentFolder == defaultParentSelection && suggestedIndex)
		{
			targetIndex = *suggestedIndex;
		}
		else
		{
			targetIndex = selectedParentFolder->GetChildren().size();
		}

		bookmarkTree->AddBookmarkItem(selectedParentFolder, std::move(bookmarkItem), targetIndex);

		return rawBookmarkItem;
	}

	return nullptr;
}

void EditBookmarkItem(BookmarkItem *bookmarkItem, BookmarkTree *bookmarkTree, HWND parentWindow,
	const AcceleratorManager *acceleratorManager, const ResourceLoader *resourceLoader,
	PlatformContext *platformContext)
{
	if (bookmarkTree->IsPermanentNode(bookmarkItem))
	{
		DCHECK(false);
		return;
	}

	BookmarkItem *selectedParentFolder = nullptr;
	auto *addBookmarkDialog = AddBookmarkDialog::Create(resourceLoader, parentWindow, bookmarkTree,
		bookmarkItem, nullptr, &selectedParentFolder, acceleratorManager, platformContext);
	auto res = addBookmarkDialog->ShowModalDialog();

	if (res == BaseDialog::RETURN_OK)
	{
		DCHECK(selectedParentFolder);

		size_t newIndex;

		// The bookmark properties will have already been updated, so the only
		// thing that needs to be done is to move the bookmark to the correct
		// folder.
		if (selectedParentFolder == bookmarkItem->GetParent())
		{
			newIndex = bookmarkItem->GetParent()->GetChildIndex(bookmarkItem);
		}
		else
		{
			newIndex = selectedParentFolder->GetChildren().size();
		}

		bookmarkTree->MoveBookmarkItem(bookmarkItem, selectedParentFolder, newIndex);
	}
}

void RemoveBookmarks(BookmarkTree *bookmarkTree, const RawBookmarkItems &bookmarkItems)
{
	for (auto *bookmarkItem : bookmarkItems)
	{
		bookmarkTree->RemoveBookmarkItem(bookmarkItem);
	}
}

// If the specified item is a bookmark, it will be opened directly. If the item is a bookmark
// folder, each child bookmark will be opened.
void OpenBookmarkItemWithDisposition(const BookmarkItem *bookmarkItem,
	OpenFolderDisposition disposition, BrowserWindow *browser, BookmarkTree *bookmarkTree,
	const ResourceLoader *resourceLoader)
{
	// It doesn't make any sense to open a folder in the current tab.
	if (bookmarkItem->IsFolder() && disposition == OpenFolderDisposition::CurrentTab)
	{
		DCHECK(false);
		return;
	}

	// This isn't currently supported.
	if (bookmarkItem->IsFolder() && disposition == OpenFolderDisposition::NewWindow)
	{
		return;
	}

	const auto *shellBrowser = browser->GetActiveShellBrowser();
	std::wstring currentDirectory = shellBrowser->GetDirectoryPath();

	if (bookmarkItem->IsFolder())
	{
		for (auto &childItem : bookmarkItem->GetChildren() | boost::adaptors::filtered(IsBookmark))
		{
			// When opening all the bookmarks in a folder, any bookmarks whose location doesn't
			// exist are simply skipped, rather than showing a message for each one.
			OpenBookmarkWithDisposition(childItem.get(), disposition, currentDirectory, browser,
				bookmarkTree, resourceLoader, false);

			// When opening a set of bookmarks within a folder, only the first item should be
			// switched to.
			if (disposition == OpenFolderDisposition::ForegroundTab
				|| disposition == OpenFolderDisposition::NewTabDefault
				|| disposition == OpenFolderDisposition::NewTabAlternate)
			{
				disposition = OpenFolderDisposition::BackgroundTab;
			}
		}
	}
	else
	{
		OpenBookmarkWithDisposition(bookmarkItem, disposition, currentDirectory, browser,
			bookmarkTree, resourceLoader, true);
	}
}

// Cuts/copies the selected bookmark items. Each bookmark item needs to be part
// of the specified bookmark tree.
bool CopyBookmarkItems(ClipboardStore *clipboardStore, BookmarkTree *bookmarkTree,
	const RawBookmarkItems &bookmarkItems, ClipboardAction action)
{
	BookmarkClipboard bookmarkClipboard(clipboardStore);
	bool res = bookmarkClipboard.WriteBookmarks(bookmarkItems);

	if (action == ClipboardAction::Cut && res)
	{
		RemoveBookmarks(bookmarkTree, bookmarkItems);
	}

	return res;
}

// Note that the parent folder must be a part of the specified bookmark tree.
void PasteBookmarkItems(ClipboardStore *clipboardStore, BookmarkTree *bookmarkTree,
	BookmarkItem *parentFolder, size_t index)
{
	DCHECK(parentFolder->IsFolder());

	BookmarkClipboard bookmarkClipboard(clipboardStore);
	auto bookmarkItems = bookmarkClipboard.ReadBookmarks();
	int i = 0;

	for (auto &bookmarkItem : bookmarkItems)
	{
		bookmarkTree->AddBookmarkItem(parentFolder, std::move(bookmarkItem), index + i);

		i++;
	}
}

bool IsAncestor(const BookmarkItem *bookmarkItem, const BookmarkItem *possibleAncestor)
{
	if (bookmarkItem == possibleAncestor)
	{
		return true;
	}

	const BookmarkItem *parent = bookmarkItem->GetParent();

	if (!parent)
	{
		return false;
	}

	return IsAncestor(parent, possibleAncestor);
}

}
