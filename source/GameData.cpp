#include "Reforger.hpp"
#include <fstream>
#include <sstream>

using namespace Aurie;

std::vector<StatInfo> g_Stats;

using Row = std::map<std::string, std::string>;

static std::string Base64Decode(const std::string& Text)
{
	std::string out;
	unsigned buffer = 0;
	int bits = 0;
	for (unsigned char c : Text)
	{
		int value;
		if (c >= 'A' && c <= 'Z') value = c - 'A';
		else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
		else if (c >= '0' && c <= '9') value = c - '0' + 52;
		else if (c == '+') value = 62;
		else if (c == '/') value = 63;
		else continue;
		buffer = (buffer << 6) | value;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
		}
	}
	return out;
}

// Just enough JSON for the game's tables: an array of flat objects.
struct Parser
{
	const std::string& text;
	size_t at = 0;

	void Space() { while (at < text.size() && isspace(static_cast<unsigned char>(text[at]))) at++; }

	std::string String()
	{
		std::string out;
		at++;
		while (at < text.size() && text[at] != '"')
		{
			char c = text[at++];
			if (c != '\\' || at >= text.size()) { out.push_back(c); continue; }
			char escape = text[at++];
			if (escape == 'n') out.push_back('\n');
			else if (escape == 't') out.push_back('\t');
			else if (escape == 'u' && at + 4 <= text.size())
			{
				unsigned code = std::stoul(text.substr(at, 4), nullptr, 16);
				at += 4;
				if (code < 0x80) out.push_back(static_cast<char>(code));
				else if (code < 0x800) { out.push_back(static_cast<char>(0xC0 | (code >> 6))); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
				else { out.push_back(static_cast<char>(0xE0 | (code >> 12))); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
			}
			else out.push_back(escape);
		}
		at++;
		return out;
	}

	// Scalars come back as text; nested containers are skipped.
	std::string Value()
	{
		Space();
		if (at >= text.size())
			return "";
		if (text[at] == '"')
			return String();
		if (text[at] == '{' || text[at] == '[')
		{
			int depth = 0;
			do
			{
				if (text[at] == '"') { String(); continue; }
				if (text[at] == '{' || text[at] == '[') depth++;
				if (text[at] == '}' || text[at] == ']') depth--;
				at++;
			} while (depth > 0 && at < text.size());
			return "";
		}
		size_t start = at;
		while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ']' && !isspace(static_cast<unsigned char>(text[at]))) at++;
		std::string word = text.substr(start, at - start);
		return word == "null" ? "" : word;
	}

	std::vector<Row> Rows()
	{
		std::vector<Row> rows;
		Space();
		if (at >= text.size() || text[at] != '[')
			return rows;
		at++;
		while (true)
		{
			Space();
			if (at >= text.size() || text[at] != '{')
				break;
			at++;
			Row row;
			while (true)
			{
				Space();
				if (at >= text.size() || text[at] != '"')
					break;
				std::string key = String();
				Space();
				if (at < text.size() && text[at] == ':') at++;
				row[key] = Value();
				Space();
				if (at < text.size() && text[at] == ',') at++;
			}
			if (at < text.size() && text[at] == '}') at++;
			rows.push_back(std::move(row));
			Space();
			if (at < text.size() && text[at] == ',') at++;
		}
		return rows;
	}
};

static std::vector<Row> LoadTable(const fs::path& Path)
{
	std::ifstream file(Path, std::ios::binary);
	if (!file)
		return {};
	std::stringstream content;
	content << file.rdbuf();
	// Some tables are base64-wrapped, some are plain JSON.
	std::string text = content.str();
	if (text.rfind("\xEF\xBB\xBF", 0) == 0)
		text.erase(0, 3);
	if (text.find_first_of("[{") != text.find_first_not_of(" \t\r\n"))
		text = Base64Decode(text);
	try { return Parser{ text }.Rows(); }
	catch (...) { return {}; }
}

void LoadGameData()
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	fs::path game = fs::path(exe).parent_path();

	std::map<std::string, std::string> names;
	for (Row& row : LoadTable(game / "dat_str.json"))
		if (!row["EN"].empty()) names[row["REF"]] = row["EN"];

	std::lock_guard guard(g_Lock);
	g_Stats.clear();
	for (Row& row : LoadTable(game / "dat_sta.json"))
	{
		StatInfo stat;
		stat.ref = row["REF"];
		if (stat.ref.empty())
			continue;
		auto name = names.find(stat.ref);
		stat.name = name != names.end() ? name->second : stat.ref;
		stat.label = stat.name;
		if (row["PERCENT"] == "%")
			stat.name += " %";
		stat.columns = std::move(row);
		g_Stats.push_back(std::move(stat));
	}
}

const StatInfo* FindStat(const std::string& Ref)
{
	for (const StatInfo& stat : g_Stats)
		if (stat.ref == Ref) return &stat;
	return nullptr;
}

std::string StatName(const std::string& Ref)
{
	const StatInfo* stat = FindStat(Ref);
	return stat ? stat->name : Ref;
}
