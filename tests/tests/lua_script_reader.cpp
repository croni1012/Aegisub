#include <gtest/gtest.h>

#include <libaegisub/fs.h>
#include <libaegisub/lua/modules.h>
#include <libaegisub/lua/script_reader.h>
#include <libaegisub/lua/utils.h>

#include <chrono>
#include <fstream>
#include <memory>

namespace {
class LuaScriptReader : public testing::Test {
protected:
	std::unique_ptr<lua_State, decltype(&lua_close)> state{luaL_newstate(), lua_close};
	agi::fs::path root;
	agi::fs::path includes;

	void SetUp() override {
		ASSERT_NE(nullptr, state);
		root = agi::fs::path(std::filesystem::temp_directory_path()) /
			("aegisub-lua-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		includes = root / "Lófasz 日本語 😀/AppData/Roaming/Aegisub/automation/include";
		std::filesystem::create_directories(includes);
		std::filesystem::copy(agi::fs::path(AUTOMATION_TEST_INCLUDE_DIR), includes,
			std::filesystem::copy_options::recursive);
		agi::lua::preload_modules(state.get());
	}

	void TearDown() override {
		state.reset();
		if (!root.empty()) std::filesystem::remove_all(root);
	}

	void Write(agi::fs::path const& path, const char *contents) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary);
		file << contents;
		ASSERT_TRUE(file.good());
	}

	void Install() {
		ASSERT_TRUE(agi::lua::Install(state.get(), {includes}))
			<< agi::lua::get_string_or_default(state.get(), -1);
		ASSERT_EQ(0, lua_gettop(state.get()));
	}

	void Run(const char *script) {
		ASSERT_EQ(0, luaL_dostring(state.get(), script))
			<< agi::lua::get_string_or_default(state.get(), -1);
	}
};

TEST_F(LuaScriptReader, UnicodeScriptAndModules) {
	Write(includes / "modul_árvíz.lua", "return 42");
	Write(includes / "csomag_日本語/init.lua", "return 43");
	Write(includes / "hold_😀.moon", "return 44");
	const auto script = includes / "szkript_őű.lua";
	Write(script, "assert(require('modul_árvíz') == 42)\n"
		"assert(require('csomag_日本語') == 43)\n"
		"assert(require('hold_😀') == 44)\n");
	ASSERT_NO_FATAL_FAILURE(Install());
	ASSERT_TRUE(agi::lua::LoadFile(state.get(), script));
	ASSERT_EQ(0, lua_pcall(state.get(), 0, 0, 0))
		<< agi::lua::get_string_or_default(state.get(), -1);
}

TEST_F(LuaScriptReader, UnicodeFileAttributes) {
	const auto file = includes / "adat_őű_日本語.txt";
	Write(file, "contents");
	ASSERT_NO_FATAL_FAILURE(Install());
	agi::lua::push_value(state.get(), file);
	lua_setglobal(state.get(), "test_file");
	agi::lua::push_value(state.get(), includes);
	lua_setglobal(state.get(), "test_directory");
	Run("local lfs = require('lfs')\n"
		"assert(lfs.attributes(test_file, 'mode') == 'file')\n"
		"assert(lfs.attributes(test_directory, 'mode') == 'directory')\n"
		"assert(lfs.attributes(test_file).size == 8)\n"
		"assert(lfs.attributes(test_file .. '.missing', 'mode') == nil)\n");
}

TEST_F(LuaScriptReader, UnicodeFileIO) {
	ASSERT_NO_FATAL_FAILURE(Install());
	agi::lua::push_value(state.get(), includes / "adat_őű_日本語.txt");
	lua_setglobal(state.get(), "test_file");
	Run("local f = assert(io.open(test_file, 'wb'))\n"
		"assert(f:write('contents'))\n"
		"assert(f:close())\n"
		"f = assert(io.open(test_file, 'rb'))\n"
		"assert(f:read('*a') == 'contents')\n"
		"assert(f:close())\n"
		"assert(os.rename(test_file, test_file .. '.renamed'))\n"
		"assert(os.remove(test_file .. '.renamed'))\n");
}

TEST_F(LuaScriptReader, UnicodeModuleSyntaxError) {
	Write(includes / "hibás_őű.lua", "return )");
	ASSERT_NO_FATAL_FAILURE(Install());
	Run("local ok, err = pcall(require, 'hibás_őű')\n"
		"assert(not ok)\n"
		"assert(err:find('hibás_őű.lua', 1, true), err)\n"
		"assert(err:find('unexpected symbol', 1, true), err)\n");
}
}
