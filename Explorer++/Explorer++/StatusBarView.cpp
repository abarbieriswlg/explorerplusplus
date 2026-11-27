// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "StatusBarView.h"
#include "../Helper/Controls.h"
#include "../Helper/WindowHelper.h"
#include "../Helper/WindowSubclass.h"
#include <wil/common.h>
#include <wil/resource.h>
#include <CommCtrl.h>
#include <windowsx.h>

StatusBarView::StatusBarView(ViewBase *parent, const Config *config) :
	ViewBase(CreateStatusBar(parent->GetHWND(),
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SBARS_SIZEGRIP | WS_CLIPCHILDREN)),
	m_fontSetter(GetHWND(), config),
	m_parts({ 100 })
{
	AddSubclass(std::make_unique<WindowSubclass>(GetHWND(),
		std::bind_front(&StatusBarView::StatusBarSubclass, this)));

	m_fontSetter.fontUpdatedSignal.AddObserver(
		std::bind_front(&StatusBarView::UpdateMinHeight, this));
	m_fontSetter.fontUpdatedSignal.AddObserver(
		std::bind_front(&StatusBarView::UpdateFilterBoxFont, this));

	UpdateControlParts();

	CreateFilterBox();

	// Even if the status bar uses the default font, the height won't necessarily be correct. As
	// detailed below, if the text size is increased in Windows, the height of the status bar won't
	// change at 96 DPI. Therefore, setting the minimum height here ensures that the status bar is
	// sized correctly in that situation.
	UpdateMinHeight();
}

StatusBarView::~StatusBarView() = default;

void StatusBarView::CreateFilterBox()
{
	// Note that the edit control is created hidden. It will be shown once a part has been
	// assigned to it.
	m_filterBox = CreateWindowEx(WS_EX_CLIENTEDGE, WC_EDIT, L"",
		WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, GetHWND(), nullptr,
		GetModuleHandle(nullptr), nullptr);
	CHECK(m_filterBox);

	m_filterBoxSubclass = std::make_unique<WindowSubclass>(m_filterBox,
		std::bind_front(&StatusBarView::FilterBoxSubclass, this));

	UpdateFilterBoxFont();
}

void StatusBarView::UpdateFilterBoxFont()
{
	if (!m_filterBox)
	{
		return;
	}

	auto font = reinterpret_cast<HFONT>(SendMessage(GetHWND(), WM_GETFONT, 0, 0));
	SendMessage(m_filterBox, WM_SETFONT, reinterpret_cast<WPARAM>(font), true);

	UpdateFilterBoxPosition();
}

void StatusBarView::SetFilterBoxPart(std::optional<int> partIndex)
{
	m_filterBoxPart = partIndex;

	if (!m_filterBoxPart)
	{
		// If the box is being hidden while it has focus, the focus would otherwise be lost.
		if (GetFocus() == m_filterBox)
		{
			filterBoxExitedSignal.m_signal();
		}

		ShowWindow(m_filterBox, SW_HIDE);
		return;
	}

	UpdateFilterBoxPosition();
	ShowWindow(m_filterBox, SW_SHOWNA);
}

void StatusBarView::SetFilterBoxCueBanner(const std::wstring &text)
{
	// The cue banner is also shown while the control has focus, so that it's clear what the box
	// is for, even after it's been focused via the keyboard shortcut.
	SendMessage(m_filterBox, EM_SETCUEBANNER, true, reinterpret_cast<LPARAM>(text.c_str()));
}

void StatusBarView::SetFilterText(const std::wstring &text)
{
	if (m_filterChangePending)
	{
		// The user is in the middle of typing. The text they've entered takes precedence and will
		// be applied once the debounce timer fires.
		return;
	}

	if (text == GetFilterText())
	{
		return;
	}

	m_settingFilterText = true;
	SetWindowText(m_filterBox, text.c_str());
	m_settingFilterText = false;

	// Any pending (debounced) change is now out of date.
	KillTimer(GetHWND(), FILTER_TIMER_ID);
	m_filterChangePending = false;

	// Place the caret at the end of the text.
	auto length = GetWindowTextLength(m_filterBox);
	SendMessage(m_filterBox, EM_SETSEL, length, length);
}

std::wstring StatusBarView::GetFilterText() const
{
	auto length = GetWindowTextLength(m_filterBox);
	std::wstring text(length + 1, '\0');
	GetWindowText(m_filterBox, text.data(), length + 1);
	text.resize(length);
	return text;
}

void StatusBarView::FocusFilterBox()
{
	if (!m_filterBoxPart)
	{
		return;
	}

	SetFocus(m_filterBox);
	SendMessage(m_filterBox, EM_SETSEL, 0, -1);
}

void StatusBarView::UpdateFilterBoxPosition()
{
	if (!m_filterBox || !m_filterBoxPart)
	{
		return;
	}

	RECT partRect;
	auto res =
		SendMessage(GetHWND(), SB_GETRECT, *m_filterBoxPart, reinterpret_cast<LPARAM>(&partRect));

	if (!res)
	{
		return;
	}

	// The last part extends underneath the size grip, so the box is shrunk to avoid overlapping
	// it.
	if (*m_filterBoxPart == static_cast<int>(m_parts.size()) - 1
		&& WI_IsFlagSet(GetWindowLongPtr(GetHWND(), GWL_STYLE), SBARS_SIZEGRIP))
	{
		partRect.right -= GetSystemMetrics(SM_CXVSCROLL);
	}

	const int padding = 1;
	InflateRect(&partRect, -padding, -padding);

	SetWindowPos(m_filterBox, nullptr, partRect.left, partRect.top, GetRectWidth(&partRect),
		GetRectHeight(&partRect), SWP_NOZORDER | SWP_NOACTIVATE);
}

void StatusBarView::OnFilterBoxTextChanged()
{
	if (m_settingFilterText)
	{
		return;
	}

	// Restarting the timer here means that the filter will only be applied once the user pauses
	// typing.
	m_filterChangePending = true;
	SetTimer(GetHWND(), FILTER_TIMER_ID, FILTER_DEBOUNCE_DELAY_MS, nullptr);
}

void StatusBarView::FlushPendingFilterChange()
{
	KillTimer(GetHWND(), FILTER_TIMER_ID);

	if (!m_filterChangePending)
	{
		return;
	}

	m_filterChangePending = false;
	filterTextChangedSignal.m_signal(GetFilterText());
}

LRESULT StatusBarView::FilterBoxSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_GETDLGCODE:
		switch (wParam)
		{
		case VK_ESCAPE:
		case VK_RETURN:
			return DLGC_WANTALLKEYS;
		}
		break;

	case WM_KEYDOWN:
		switch (wParam)
		{
		case VK_ESCAPE:
			if (GetWindowTextLength(m_filterBox) > 0)
			{
				// The first press of Escape clears the filter (and leaves the focus in the box).
				// A second press moves the focus back to the listview.
				SetWindowText(m_filterBox, L"");
				FlushPendingFilterChange();
			}
			else
			{
				filterBoxExitedSignal.m_signal();
			}
			return 0;

		case VK_RETURN:
		case VK_DOWN:
			// Apply the filter immediately and move to the (filtered) list of items.
			FlushPendingFilterChange();
			filterBoxExitedSignal.m_signal();
			return 0;
		}
		break;

	case WM_CHAR:
		// Prevents the default beep that's played when Enter or Escape are pressed in a
		// single-line edit control.
		if (wParam == VK_RETURN || wParam == VK_ESCAPE)
		{
			return 0;
		}
		break;
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void StatusBarView::UpdateMinHeight()
{
	auto hdc = wil::GetDC(GetHWND());

	auto font = reinterpret_cast<HFONT>(SendMessage(GetHWND(), WM_GETFONT, 0, 0));
	wil::unique_select_object selectFont;

	if (font)
	{
		selectFont = wil::SelectObject(hdc.get(), font);
	}

	wil::unique_htheme theme(OpenThemeData(GetHWND(), L"Status"));
	DCHECK(theme);

	TEXTMETRIC textMetrics;
	HRESULT hr = GetThemeTextMetrics(theme.get(), hdc.get(), 0, 0, &textMetrics);
	DCHECK(SUCCEEDED(hr));

	// From looking into precisely what the status bar control does when it receives a WM_SETFONT
	// message, it appears that it does recalculate its height. However, for whatever reason, the
	// font that's passed to WM_SETFONT will only be used as part of the calculation if the DPI is
	// greater than 96. Otherwise, the default DC font will be used for the calculation.
	//
	// That means that, confusingly, if the DPI is greater than 96, WM_SETFONT will work as expected
	// - the font will be set and the control size will be correctly updated. However, if the DPI is
	// 96, the font will be set, but the control size won't change.
	//
	// The same issue can also be seen when increasing the text size in Windows. Doing that will
	// cause the status bar to use the larger font, but the height of the status bar won't change,
	// leading to the text being potentially cut off.
	//
	// Therefore, to work around those sorts of issues, the minimum height is set here. Note that
	// the height set here will be ignored if it's smaller than the text height calculated by the
	// control. That means that, at 96 DPI, the height will be ignored if the font that's set is
	// smaller than the default font.
	//
	// That shouldn't really be a problem, though, since the control will simply be a bit larger
	// than necessary.
	SendMessage(GetHWND(), SB_SETMINHEIGHT, textMetrics.tmHeight, 0);

	// As per the documentation for SB_SETMINHEIGHT, this will redraw the window.
	SendMessage(GetHWND(), WM_SIZE, 0, 0);
}

void StatusBarView::SetParts(const std::vector<int> &parts)
{
	CHECK(VerifyParts(parts));

	m_parts = parts;
	UpdateControlParts();
}

void StatusBarView::SetPartText(int partIndex, const std::wstring &text)
{
	auto res =
		SendMessage(GetHWND(), SB_SETTEXT, partIndex, reinterpret_cast<LPARAM>(text.c_str()));
	DCHECK(res);
}

LRESULT StatusBarView::StatusBarSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_SIZE:
	{
		UpdateControlParts();

		// The default handling resizes the control, so the filter box can only be positioned
		// correctly once that's done.
		auto res = DefSubclassProc(hwnd, msg, wParam, lParam);
		UpdateFilterBoxPosition();
		return res;
	}

	case WM_COMMAND:
		if (reinterpret_cast<HWND>(lParam) == m_filterBox)
		{
			switch (HIWORD(wParam))
			{
			case EN_CHANGE:
				OnFilterBoxTextChanged();
				return 0;

			case EN_KILLFOCUS:
				// Ensures that a change that's still pending is applied to the tab it was
				// entered for, before any other tab can be selected.
				FlushPendingFilterChange();
				return 0;
			}
		}
		break;

	case WM_TIMER:
		if (wParam == FILTER_TIMER_ID)
		{
			FlushPendingFilterChange();
			return 0;
		}
		break;
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool StatusBarView::VerifyParts(const std::vector<int> &parts)
{
	if (parts.empty())
	{
		return false;
	}

	int previousPart = 0;

	for (int part : parts)
	{
		if (part < 1 || part > 100)
		{
			return false;
		}

		if (part <= previousPart)
		{
			return false;
		}

		previousPart = part;
	}

	return true;
}

void StatusBarView::UpdateControlParts()
{
	RECT clientRect;
	BOOL res = GetClientRect(GetHWND(), &clientRect);
	CHECK(res);

	int width = GetRectWidth(&clientRect);

	std::vector<int> partWidths;

	for (int part : m_parts)
	{
		if (part == 100)
		{
			partWidths.push_back(-1);
		}
		else
		{
			partWidths.push_back(static_cast<int>((part / 100.0) * width));
		}
	}

	auto setPartsRes = SendMessage(GetHWND(), SB_SETPARTS, m_parts.size(),
		reinterpret_cast<LPARAM>(partWidths.data()));
	DCHECK(setPartsRes);

	UpdateFilterBoxPosition();
}
