// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#pragma once

#include "MainFontSetter.h"
#include "ViewBase.h"
#include "../Helper/SignalWrapper.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct Config;
class WindowSubclass;

class StatusBarView : public ViewBase
{
public:
	StatusBarView(ViewBase *parent, const Config *config);
	~StatusBarView();

	// The parts are specified here as a list of integer values between 1 and 100, with each value
	// indicating how far along the status bar the associated part ends. For example, if the values
	// specified were:
	//
	// 25, 40, 100
	//
	// The parts would span:
	//
	// 0% - 25%
	// 25% - 40%
	// 40% - 100%
	void SetParts(const std::vector<int> &parts);

	void SetPartText(int partIndex, const std::wstring &text);

	// The quick filter box is an edit control that's shown within one of the status bar parts.
	// Passing std::nullopt hides the box.
	void SetFilterBoxPart(std::optional<int> partIndex);
	void SetFilterBoxCueBanner(const std::wstring &text);

	// Updates the text in the filter box, without triggering filterTextChangedSignal. Ignored if
	// there's a pending (not yet applied) change entered by the user.
	void SetFilterText(const std::wstring &text);
	std::wstring GetFilterText() const;

	void FocusFilterBox();

	// Triggered when the text in the filter box changes. Changes made while typing are debounced,
	// so that the filter isn't re-applied after every single key press.
	SignalWrapper<StatusBarView, void(const std::wstring &text)> filterTextChangedSignal;

	// Triggered when the user leaves the filter box via the keyboard (Enter, Escape or the down
	// arrow key). Focus should be moved back to the active listview at that point.
	SignalWrapper<StatusBarView, void()> filterBoxExitedSignal;

private:
	static constexpr UINT_PTR FILTER_TIMER_ID = 0x5146;
	static constexpr UINT FILTER_DEBOUNCE_DELAY_MS = 120;

	LRESULT StatusBarSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT FilterBoxSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void CreateFilterBox();
	void UpdateFilterBoxFont();
	void UpdateFilterBoxPosition();
	void OnFilterBoxTextChanged();
	void FlushPendingFilterChange();

	// This function should be called whenever the font is updated for the status bar control.
	void UpdateMinHeight();

	static bool VerifyParts(const std::vector<int> &parts);
	void UpdateControlParts();

	MainFontSetter m_fontSetter;
	std::vector<int> m_parts;

	HWND m_filterBox = nullptr;
	std::unique_ptr<WindowSubclass> m_filterBoxSubclass;
	std::optional<int> m_filterBoxPart;
	bool m_settingFilterText = false;
	bool m_filterChangePending = false;
};
