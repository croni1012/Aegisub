// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include <memory>

class wxFrame;
class wxWindow;
namespace agi { struct Context; }

enum class FloatingTagWindow {
	Basic,
	BorderShadow,
	Font,
	Alignment,
	Transform,
	DeleteTags
};

class FloatingTagWindowManager {
	class Impl;
	std::unique_ptr<Impl> impl;

public:
	FloatingTagWindowManager(wxFrame *frame, agi::Context *context);
	~FloatingTagWindowManager();

	void Toggle(FloatingTagWindow window);
	void ShowAll();
	bool IsShown(FloatingTagWindow window) const;
	void SaveLayout();
};
