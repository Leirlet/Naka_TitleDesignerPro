#include "tdp_json.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace tdp::json {

namespace {

const Value kNull;

class Parser {
public:
	explicit Parser(const std::string& s) : s_(s) {}

	bool parse(Value& out, std::string* error)
	{
		skipWs();
		if (!parseValue(out, 0)) {
			if (error) *error = err_ + " at " + std::to_string(i_);
			return false;
		}
		skipWs();
		if (i_ != s_.size()) {
			if (error) *error = "trailing characters at " + std::to_string(i_);
			return false;
		}
		return true;
	}

private:
	void skipWs()
	{
		while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
	}

	bool fail(const char* msg) { err_ = msg; return false; }

	bool parseValue(Value& out, int depth)
	{
		if (depth > 128) return fail("nesting too deep");
		if (i_ >= s_.size()) return fail("unexpected end");
		const char c = s_[i_];
		if (c == '{') return parseObject(out, depth);
		if (c == '[') return parseArray(out, depth);
		if (c == '"') {
			std::string str;
			if (!parseString(str)) return false;
			out = Value(std::move(str));
			return true;
		}
		if (c == 't' && s_.compare(i_, 4, "true") == 0) { i_ += 4; out = Value(true); return true; }
		if (c == 'f' && s_.compare(i_, 5, "false") == 0) { i_ += 5; out = Value(false); return true; }
		if (c == 'n' && s_.compare(i_, 4, "null") == 0) { i_ += 4; out = Value(); return true; }
		if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
		return fail("unexpected character");
	}

	bool parseNumber(Value& out)
	{
		const char* begin = s_.c_str() + i_;
		char* end = nullptr;
		double d = strtod(begin, &end);
		if (end == begin) return fail("bad number");
		i_ += end - begin;
		out = Value(d);
		return true;
	}

	static void appendUtf8(std::string& s, uint32_t cp)
	{
		if (cp < 0x80) s += (char)cp;
		else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
		else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
		else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
	}

	bool parseHex4(uint32_t& v)
	{
		if (i_ + 4 > s_.size()) return fail("bad unicode escape");
		v = 0;
		for (int k = 0; k < 4; ++k) {
			char h = s_[i_++];
			v <<= 4;
			if (h >= '0' && h <= '9') v |= h - '0';
			else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
			else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
			else return fail("bad unicode escape");
		}
		return true;
	}

	bool parseString(std::string& out)
	{
		++i_; // opening quote
		while (i_ < s_.size()) {
			char c = s_[i_++];
			if (c == '"') return true;
			if (c != '\\') { out += c; continue; }
			if (i_ >= s_.size()) break;
			char e = s_[i_++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				uint32_t cp;
				if (!parseHex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
					i_ += 2;
					uint32_t lo;
					if (!parseHex4(lo)) return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				appendUtf8(out, cp);
				break;
			}
			default: return fail("bad escape");
			}
		}
		return fail("unterminated string");
	}

	bool parseArray(Value& out, int depth)
	{
		++i_;
		out = Value::Array();
		skipWs();
		if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
		for (;;) {
			skipWs();
			Value item;
			if (!parseValue(item, depth + 1)) return false;
			out.push(std::move(item));
			skipWs();
			if (i_ >= s_.size()) return fail("unterminated array");
			if (s_[i_] == ',') { ++i_; continue; }
			if (s_[i_] == ']') { ++i_; return true; }
			return fail("expected , or ]");
		}
	}

	bool parseObject(Value& out, int depth)
	{
		++i_;
		out = Value::Object();
		skipWs();
		if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
		for (;;) {
			skipWs();
			if (i_ >= s_.size() || s_[i_] != '"') return fail("expected key");
			std::string key;
			if (!parseString(key)) return false;
			skipWs();
			if (i_ >= s_.size() || s_[i_] != ':') return fail("expected :");
			++i_;
			skipWs();
			Value v;
			if (!parseValue(v, depth + 1)) return false;
			out.set(key.c_str(), std::move(v));
			skipWs();
			if (i_ >= s_.size()) return fail("unterminated object");
			if (s_[i_] == ',') { ++i_; continue; }
			if (s_[i_] == '}') { ++i_; return true; }
			return fail("expected , or }");
		}
	}

	const std::string& s_;
	size_t i_ = 0;
	std::string err_;
};

void DumpString(std::string& out, const std::string& s)
{
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += (char)c;
			}
		}
	}
	out += '"';
}

} // namespace

std::string ToUtf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring FromUtf8(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

Value::Value(const std::wstring& s) : type_(Type::String), str_(ToUtf8(s)) {}

std::wstring Value::asWString(const std::wstring& fallback) const
{
	return type_ == Type::String ? FromUtf8(str_) : fallback;
}

const Value& Value::operator[](size_t i) const
{
	return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : kNull;
}

const Value& Value::operator[](const char* key) const
{
	if (type_ == Type::Object)
		for (const auto& m : obj_)
			if (m.first == key) return m.second;
	return kNull;
}

bool Value::has(const char* key) const
{
	if (type_ == Type::Object)
		for (const auto& m : obj_)
			if (m.first == key) return true;
	return false;
}

Value& Value::set(const char* key, Value v)
{
	if (type_ != Type::Object) *this = Object();
	for (auto& m : obj_)
		if (m.first == key) { m.second = std::move(v); return m.second; }
	obj_.emplace_back(key, std::move(v));
	return obj_.back().second;
}

void Value::dumpTo(std::string& out, bool pretty, int indent) const
{
	auto newline = [&](int ind) {
		if (!pretty) return;
		out += '\n';
		out.append((size_t)ind * 2, ' ');
	};
	switch (type_) {
	case Type::Null: out += "null"; break;
	case Type::Bool: out += bool_ ? "true" : "false"; break;
	case Type::Number: {
		char buf[32];
		if (std::isfinite(num_) && num_ == std::floor(num_) && std::fabs(num_) < 1e15)
			snprintf(buf, sizeof(buf), "%.0f", num_);
		else
			snprintf(buf, sizeof(buf), "%.6g", std::isfinite(num_) ? num_ : 0.0);
		out += buf;
		break;
	}
	case Type::String: DumpString(out, str_); break;
	case Type::Array:
		if (arr_.empty()) { out += "[]"; break; }
		out += '[';
		for (size_t i = 0; i < arr_.size(); ++i) {
			if (i) out += ',';
			newline(indent + 1);
			arr_[i].dumpTo(out, pretty, indent + 1);
		}
		newline(indent);
		out += ']';
		break;
	case Type::Object:
		if (obj_.empty()) { out += "{}"; break; }
		out += '{';
		for (size_t i = 0; i < obj_.size(); ++i) {
			if (i) out += ',';
			newline(indent + 1);
			DumpString(out, obj_[i].first);
			out += pretty ? ": " : ":";
			obj_[i].second.dumpTo(out, pretty, indent + 1);
		}
		newline(indent);
		out += '}';
		break;
	}
}

std::string Value::dump(bool pretty) const
{
	std::string out;
	dumpTo(out, pretty, 0);
	return out;
}

bool Parse(const std::string& text, Value& out, std::string* error)
{
	Value v;
	Parser p(text);
	if (!p.parse(v, error)) return false;
	out = std::move(v);
	return true;
}

} // namespace tdp::json
