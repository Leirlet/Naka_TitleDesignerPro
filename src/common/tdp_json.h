// NAKA Title Designer Pro - minimal JSON value / parser / writer (UTF-8)
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tdp::json {

class Value {
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	Value() = default;
	Value(std::nullptr_t) {}
	Value(bool b) : type_(Type::Bool), bool_(b) {}
	Value(int n) : type_(Type::Number), num_(n) {}
	Value(double n) : type_(Type::Number), num_(n) {}
	Value(float n) : type_(Type::Number), num_(n) {}
	Value(const char* s) : type_(Type::String), str_(s) {}
	Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
	Value(const std::wstring& s);

	static Value Array() { Value v; v.type_ = Type::Array; return v; }
	static Value Object() { Value v; v.type_ = Type::Object; return v; }

	Type type() const { return type_; }
	bool isNull() const { return type_ == Type::Null; }
	bool isArray() const { return type_ == Type::Array; }
	bool isObject() const { return type_ == Type::Object; }
	bool isString() const { return type_ == Type::String; }
	bool isNumber() const { return type_ == Type::Number; }

	// Lenient getters: return the fallback when the type does not match.
	bool asBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
	double asNumber(double fallback = 0) const { return type_ == Type::Number ? num_ : fallback; }
	float asFloat(float fallback = 0) const { return type_ == Type::Number ? (float)num_ : fallback; }
	int asInt(int fallback = 0) const { return type_ == Type::Number ? (int)num_ : fallback; }
	const std::string& asString() const { return str_; }
	std::wstring asWString(const std::wstring& fallback = L"") const;

	// Array
	size_t size() const { return type_ == Type::Array ? arr_.size() : (type_ == Type::Object ? obj_.size() : 0); }
	const Value& operator[](size_t i) const;
	void push(Value v) { if (type_ != Type::Array) *this = Array(); arr_.push_back(std::move(v)); }
	const std::vector<Value>& items() const { return arr_; }

	// Object (keeps insertion order)
	const Value& operator[](const char* key) const;
	bool has(const char* key) const;
	Value& set(const char* key, Value v);
	const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }

	std::string dump(bool pretty = true) const;

private:
	void dumpTo(std::string& out, bool pretty, int indent) const;

	Type type_ = Type::Null;
	bool bool_ = false;
	double num_ = 0;
	std::string str_;
	std::vector<Value> arr_;
	std::vector<std::pair<std::string, Value>> obj_;
};

// Returns false (and leaves out untouched) on syntax errors.
bool Parse(const std::string& text, Value& out, std::string* error = nullptr);

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

} // namespace tdp::json
