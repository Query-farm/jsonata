#define DUCKDB_EXTENSION_MAIN

#include "jsonata_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <jsonata/IRegex.h>
#include <jsonata/JException.h>
#include <jsonata/Jsonata.h>
#include <nlohmann/json.hpp>
#include "query_farm_telemetry.hpp"
#include "re2/re2.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <list>
#include <unordered_map>

#if defined(_WIN32)
#include "duckdb/common/windows.hpp"
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

//! The extension version, reported both by Version() and by telemetry
#define JSONATA_EXTENSION_VERSION "2026110701"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Settings
//===--------------------------------------------------------------------===//
static constexpr const char *TIMEOUT_SETTING = "jsonata_timeout_ms";
static constexpr const char *MAX_DEPTH_SETTING = "jsonata_max_depth";
static constexpr int64_t DEFAULT_TIMEOUT_MS = 10000;
static constexpr int64_t DEFAULT_MAX_DEPTH = 1000;

//! Maximum nesting of arrays/objects accepted in JSON data and bindings
static constexpr idx_t MAX_JSON_NESTING = 1000;
//! Native stack reserved per level of JSON nesting (conversion to and from the evaluator's representation and
//! serialization are all recursive). Measured at about 250 bytes per level on 64-bit release builds.
static constexpr idx_t JSON_STACK_PER_LEVEL = 1024;
//! Native stack reserved per level of estimated expression nesting (the parser and its AST post-processing are
//! recursive). Measured at about 850 bytes per level on 64-bit release builds.
static constexpr idx_t EXPRESSION_STACK_PER_LEVEL = 2048;
//! Number of parsed expressions each thread keeps for a non-constant expression column
static constexpr idx_t EXPRESSION_CACHE_CAPACITY = 32;

struct JsonataLimits {
	//! Wall-clock limit of a single evaluation in milliseconds, 0 = unlimited
	int64_t timeout_ms = DEFAULT_TIMEOUT_MS;
	//! Maximum depth of the evaluator's eval/apply cycle, 0 = unlimited (the native stack guard still applies)
	int64_t max_depth = DEFAULT_MAX_DEPTH;
};

static void ValidateNonNegative(const char *name, const Value &parameter) {
	if (!parameter.IsNull() && parameter.GetValue<int64_t>() < 0) {
		throw InvalidInputException("%s must be >= 0 (0 disables the limit)", name);
	}
}

static void ValidateTimeout(ClientContext &, SetScope, Value &parameter) {
	ValidateNonNegative(TIMEOUT_SETTING, parameter);
}

static void ValidateMaxDepth(ClientContext &, SetScope, Value &parameter) {
	ValidateNonNegative(MAX_DEPTH_SETTING, parameter);
}

static JsonataLimits ReadLimits(ClientContext &context) {
	JsonataLimits limits;
	Value value;
	if (context.TryGetCurrentSetting(TIMEOUT_SETTING, value) && !value.IsNull()) {
		limits.timeout_ms = value.GetValue<int64_t>();
	}
	if (context.TryGetCurrentSetting(MAX_DEPTH_SETTING, value) && !value.IsNull()) {
		limits.max_depth = value.GetValue<int64_t>();
	}
	return limits;
}

//===--------------------------------------------------------------------===//
// Native stack guard
//===--------------------------------------------------------------------===//
// The JSONata evaluator is recursive. Counting eval depth alone cannot protect the process, because the thread
// stacks DuckDB runs on differ widely (macOS worker threads: 512KB, Linux: 8MB, musl: 128KB), so every evaluation
// step also checks how much of the real stack is left.

//! Lowest usable address of the current thread's stack, or 0 if unknown
static uintptr_t CurrentThreadStackLow() {
#if defined(_WIN32)
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(&mbi, &mbi, sizeof(mbi)) == 0) {
		return 0;
	}
	// The allocation base is the bottom of the reserved stack region; leave room for the guard pages
	return reinterpret_cast<uintptr_t>(mbi.AllocationBase) + 64 * 1024;
#elif defined(__APPLE__)
	pthread_t self = pthread_self();
	auto top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
	auto size = static_cast<uintptr_t>(pthread_get_stacksize_np(self));
	return top > size ? top - size : 0;
#elif defined(__linux__)
	pthread_attr_t attr;
	if (pthread_getattr_np(pthread_self(), &attr) != 0) {
		return 0;
	}
	void *addr = nullptr;
	size_t size = 0;
	auto rc = pthread_attr_getstack(&attr, &addr, &size);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		return 0;
	}
	return reinterpret_cast<uintptr_t>(addr);
#else
	// Unknown platform (including wasm): fall back to a fixed budget below the current stack pointer
	return 0;
#endif
}

static uintptr_t CurrentStackPointer() {
	volatile char marker = 0;
	return reinterpret_cast<uintptr_t>(&marker);
}

//! The lowest stack address an evaluation started here may reach before it is aborted
static uintptr_t ComputeStackLimit() {
	static constexpr uintptr_t FALLBACK_BUDGET = 256 * 1024;
	static constexpr uintptr_t MIN_MARGIN = 64 * 1024;
	static constexpr uintptr_t MIN_BUDGET = 16 * 1024;

	// Querying the stack bounds can be expensive (glibc reads /proc/self/maps for the main thread), so cache it
	static thread_local uintptr_t stack_low = 0;
	static thread_local bool stack_low_known = false;
	if (!stack_low_known) {
		stack_low = CurrentThreadStackLow();
		stack_low_known = true;
	}
	auto sp = CurrentStackPointer();
	if (stack_low == 0 || sp <= stack_low) {
		return sp > FALLBACK_BUDGET ? sp - FALLBACK_BUDGET : 0;
	}
	uintptr_t available = sp - stack_low;
	// Keep a margin for the native frames pushed between two checks and for unwinding/error reporting
	uintptr_t margin = MaxValue<uintptr_t>(MIN_MARGIN, available / 4);
	if (available <= margin + MIN_BUDGET) {
		return sp - MinValue<uintptr_t>(available, MIN_BUDGET);
	}
	return stack_low + margin;
}

//! Bytes of native stack this thread may still use before an evaluation started here is aborted
static idx_t AvailableStack() {
	auto limit = ComputeStackLimit();
	auto sp = CurrentStackPointer();
	return sp > limit ? sp - limit : 0;
}

//===--------------------------------------------------------------------===//
// Evaluation guard: depth, native stack, timeout and query cancellation
//===--------------------------------------------------------------------===//
enum class GuardTrip { NONE, DEPTH, STACK, TIMEOUT, INTERRUPTED };

//! Per-evaluation guard, hooked into jsonata-cpp's evaluate entry/exit callbacks. When a limit is hit it throws a
//! JSONata error to unwind the evaluator and remembers why, so the caller can report a precise DuckDB error even
//! if the library rewrapped the exception on the way out.
class EvaluationGuard {
public:
	EvaluationGuard(ClientContext &context_p, const JsonataLimits &limits_p)
	    : context(context_p), limits(limits_p), stack_limit(ComputeStackLimit()) {
		if (limits.timeout_ms > 0) {
			deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeout_ms);
		}
	}

	void OnEntry(bool parallel_call) {
		if (!parallel_call) {
			depth++;
			if (limits.max_depth > 0 && depth > limits.max_depth) {
				Trip(GuardTrip::DEPTH, "D1011");
			}
		}
		if (CurrentStackPointer() < stack_limit) {
			Trip(GuardTrip::STACK, "D1011");
		}
		// Reading the clock and the interrupt flag on every step is measurable; every 64th step is plenty
		if ((++steps & 63) == 0) {
			if (context.IsInterrupted()) {
				Trip(GuardTrip::INTERRUPTED, "D1012");
			}
			if (limits.timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
				Trip(GuardTrip::TIMEOUT, "D1012");
			}
		}
	}

	void OnExit(bool parallel_call) {
		if (!parallel_call && depth > 0) {
			depth--;
		}
	}

	void Install(jsonata::Frame &frame) {
		jsonata::EntryCallback entry = [this](std::shared_ptr<jsonata::Parser::Symbol>, const std::any &,
		                                      std::shared_ptr<jsonata::Frame> env) {
			OnEntry(env && env->isParallelCall);
		};
		jsonata::ExitCallback exit = [this](std::shared_ptr<jsonata::Parser::Symbol>, const std::any &,
		                                    std::shared_ptr<jsonata::Frame> env, const std::any &) {
			OnExit(env && env->isParallelCall);
		};
		frame.bind("__evaluate_entry", entry);
		frame.bind("__evaluate_exit", exit);
	}

	//! If a limit stopped the evaluation, throw the corresponding DuckDB error
	void ThrowIfTripped() const {
		switch (tripped) {
		case GuardTrip::NONE:
			return;
		case GuardTrip::DEPTH:
			throw InvalidInputException("JSONata evaluation exceeded the maximum evaluation depth of %lld "
			                            "(check for non-terminating recursion, or raise %s)",
			                            static_cast<long long>(limits.max_depth), MAX_DEPTH_SETTING);
		case GuardTrip::STACK:
			throw InvalidInputException("JSONata evaluation is nested too deeply for the available stack "
			                            "(check for non-terminating recursion)");
		case GuardTrip::TIMEOUT:
			throw InvalidInputException("JSONata evaluation timed out after %lld ms (raise %s, or set it to 0 to "
			                            "disable the timeout)",
			                            static_cast<long long>(limits.timeout_ms), TIMEOUT_SETTING);
		case GuardTrip::INTERRUPTED:
			throw InterruptException();
		}
	}

private:
	[[noreturn]] void Trip(GuardTrip reason, const char *code) {
		tripped = reason;
		throw jsonata::JException(code, -1);
	}

	ClientContext &context;
	JsonataLimits limits;
	uintptr_t stack_limit;
	std::chrono::steady_clock::time_point deadline;
	int64_t depth = 0;
	uint64_t steps = 0;
	GuardTrip tripped = GuardTrip::NONE;
};

//===--------------------------------------------------------------------===//
// Parsing helpers
//===--------------------------------------------------------------------===//

//! A positional-only parameter with a real name (bare types would be reported as col0, col1, ...)
static FunctionParameter Positional(const char *name, LogicalType type) {
	return FunctionParameter(name, std::move(type), {}, FunctionParameterKind::POSITIONAL_ONLY);
}

//! Upper bound on how deeply the parser recurses for an expression: one level per open bracket plus one per
//! operator in the current comma/semicolon-separated segment of every open bracket (operator chains such as
//! a + b + c build trees as deep as they are long). String literals, quoted names and comments are skipped.
static idx_t EstimateExpressionNesting(const char *data, idx_t size) {
	vector<idx_t> segments {0};
	idx_t operators = 0; // sum of segments
	idx_t max_nesting = 0;
	for (idx_t i = 0; i < size; i++) {
		char c = data[i];
		if (c == '"' || c == '\'' || c == '`') {
			// String literal or quoted name: skip to the closing quote, honouring backslash escapes
			for (i++; i < size && data[i] != c; i++) {
				if (data[i] == '\\' && c != '`') {
					i++;
				}
			}
			continue;
		}
		if (c == '/' && i + 1 < size && data[i + 1] == '*') {
			for (i += 2; i + 1 < size && !(data[i] == '*' && data[i + 1] == '/'); i++) {
			}
			i++;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			segments.push_back(0);
		} else if (c == ')' || c == ']' || c == '}') {
			if (segments.size() > 1) {
				operators -= segments.back();
				segments.pop_back();
			}
		} else if (c == ',' || c == ';') {
			operators -= segments.back();
			segments.back() = 0;
		} else if (c != '\0' && strchr("+-*/%&=<>!~?:.@#^|", c) != nullptr) {
			segments.back()++;
			operators++;
		} else if (isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
			idx_t start = i;
			while (i + 1 < size && (isalnum(static_cast<unsigned char>(data[i + 1])) || data[i + 1] == '_' ||
			                        data[i + 1] == '$')) {
				i++;
			}
			idx_t len = i - start + 1;
			if ((len == 3 && (memcmp(data + start, "and", 3) == 0)) || (len == 2 && memcmp(data + start, "or", 2) == 0) ||
			    (len == 2 && memcmp(data + start, "in", 2) == 0)) {
				segments.back()++;
				operators++;
			}
		}
		max_nesting = MaxValue<idx_t>(max_nesting, segments.size() + operators);
	}
	return max_nesting;
}

//! Regular expressions use RE2 (the engine behind DuckDB's own regexp_* functions) instead of std::regex: matching
//! runs in linear time and never recurses, so a pattern cannot hang the query (catastrophic backtracking) or
//! overflow the stack on a long input string.
//! The last regex compile error on this thread: jsonata-cpp reports every failure to compile a regex literal as
//! "Empty regular expressions are not allowed", so the parser wrapper substitutes the real reason.
static thread_local string last_regex_error;

class RE2Regex : public jsonata::IRegex {
public:
	RE2Regex(const std::string &pattern, jsonata::RegexFlags flags)
	    : re(flags.multiline ? "(?m)" + pattern : pattern, Options(flags)) {
		if (!re.ok()) {
			last_regex_error = StringUtil::Format("invalid regular expression /%s/: %s (regular expressions use RE2 "
			                                      "syntax, which has no lookaround or backreferences)",
			                                      pattern, re.error());
			throw InvalidInputException(last_regex_error);
		}
	}

	bool test(const std::string &str) const override {
		return duckdb_re2::RE2::PartialMatch(str, re);
	}

	std::optional<jsonata::RegexMatch> findFirst(const std::string &str, size_t pos) const override {
		if (pos > str.size()) {
			return std::nullopt;
		}
		int groups = re.NumberOfCapturingGroups();
		std::vector<duckdb_re2::StringPiece> submatch(groups + 1);
		duckdb_re2::StringPiece input(str);
		if (!re.Match(input, pos, str.size(), duckdb_re2::RE2::UNANCHORED, submatch.data(), groups + 1)) {
			return std::nullopt;
		}
		jsonata::RegexMatch result;
		result.text.assign(submatch[0].data(), submatch[0].size());
		result.position = static_cast<size_t>(submatch[0].data() - input.data());
		result.length = submatch[0].size();
		for (int g = 1; g <= groups; g++) {
			if (submatch[g].data() == nullptr) {
				result.groups.push_back(std::nullopt);
			} else {
				result.groups.push_back(std::string(submatch[g].data(), submatch[g].size()));
			}
		}
		return result;
	}

	std::vector<jsonata::RegexMatch> findAll(const std::string &str) const override {
		std::vector<jsonata::RegexMatch> results;
		size_t pos = 0;
		while (auto match = findFirst(str, pos)) {
			// Step past zero-length matches (a whole UTF-8 character) so the loop always advances
			pos = match->position + match->length;
			if (match->length == 0) {
				pos++;
				while (pos < str.size() && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
					pos++;
				}
			}
			results.push_back(std::move(*match));
			if (pos > str.size()) {
				break;
			}
		}
		return results;
	}

	std::vector<std::string> split(const std::string &str) const override {
		std::vector<std::string> result;
		size_t last_end = 0;
		for (const auto &match : findAll(str)) {
			result.push_back(str.substr(last_end, match.position - last_end));
			last_end = match.position + match.length;
		}
		result.push_back(str.substr(last_end));
		return result;
	}

private:
	static duckdb_re2::RE2::Options Options(jsonata::RegexFlags flags) {
		duckdb_re2::RE2::Options options;
		options.set_case_sensitive(!flags.caseInsensitive);
		// ^ and $ match only at the start and end of the text unless the 'm' flag added (?m) to the pattern
		options.set_log_errors(false);
		return options;
	}

	duckdb_re2::RE2 re;
};

static jsonata::RegexEngine RE2RegexEngine() {
	return [](const std::string &pattern, jsonata::RegexFlags flags) -> std::shared_ptr<jsonata::IRegex> {
		return std::make_shared<RE2Regex>(pattern, flags);
	};
}

static unique_ptr<jsonata::Jsonata> ParseJsonataExpression(const string &expr_str) {
	auto nesting = EstimateExpressionNesting(expr_str.data(), expr_str.size());
	if (EXPRESSION_STACK_PER_LEVEL > 0 && nesting * EXPRESSION_STACK_PER_LEVEL > AvailableStack()) {
		throw InvalidInputException("Invalid JSONata expression: the expression is nested too deeply (about %llu "
		                            "levels of brackets and chained operators)",
		                            static_cast<unsigned long long>(nesting));
	}
	last_regex_error.clear();
	try {
		return make_uniq<jsonata::Jsonata>(expr_str, RE2RegexEngine());
	} catch (const Exception &) {
		throw;
	} catch (const std::exception &e) {
		if (!last_regex_error.empty()) {
			throw InvalidInputException("Invalid JSONata expression: %s", last_regex_error);
		}
		throw InvalidInputException("Invalid JSONata expression: %s", e.what());
	} catch (...) {
		throw InvalidInputException("Invalid JSONata expression: unknown error");
	}
}

//! Nesting depth of arrays/objects in a JSON text (strings are skipped). Malformed text is left to the parser.
static idx_t JsonNesting(const char *data, idx_t size) {
	idx_t depth = 0;
	idx_t max_depth = 0;
	for (idx_t i = 0; i < size; i++) {
		char c = data[i];
		if (c == '"') {
			for (i++; i < size && data[i] != '"'; i++) {
				if (data[i] == '\\') {
					i++;
				}
			}
		} else if (c == '[' || c == '{') {
			max_depth = MaxValue(max_depth, ++depth);
		} else if ((c == ']' || c == '}') && depth > 0) {
			depth--;
		}
	}
	return max_depth;
}

static nlohmann::json ParseJsonData(string_t data) {
	auto nesting = JsonNesting(data.GetData(), data.GetSize());
	if (nesting > MAX_JSON_NESTING) {
		throw InvalidInputException("Invalid JSON data: arrays/objects are nested more than %llu levels deep",
		                            static_cast<unsigned long long>(MAX_JSON_NESTING));
	}
	if (JSON_STACK_PER_LEVEL > 0 && nesting * JSON_STACK_PER_LEVEL > AvailableStack()) {
		throw InvalidInputException("Invalid JSON data: arrays/objects are nested too deeply (%llu levels) for the "
		                            "available stack",
		                            static_cast<unsigned long long>(nesting));
	}
	try {
		return nlohmann::json::parse(data.GetData(), data.GetData() + data.GetSize());
	} catch (const Exception &) {
		throw;
	} catch (const std::exception &e) {
		throw InvalidInputException("Invalid JSON data: %s", e.what());
	} catch (...) {
		throw InvalidInputException("Invalid JSON data: unknown error");
	}
}

//===--------------------------------------------------------------------===//
// Per-thread state: limits and a small LRU cache of parsed expressions
//===--------------------------------------------------------------------===//
// jsonata::Jsonata keeps thread_local evaluation state, so an instance must never be used by two threads at once.
// FunctionLocalState is owned by a single executing thread at a time, which makes it the right home for parsed
// expressions: they are parsed once per thread and query instead of once per row (or per chunk).
//! Bind data: whether the expression can return a different value on every call ($now(), $random(), ...)
struct JsonataBindData : public FunctionData {
	explicit JsonataBindData(bool is_volatile_p) : is_volatile(is_volatile_p) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JsonataBindData>(is_volatile);
	}
	bool Equals(const FunctionData &other) const override {
		return is_volatile == other.Cast<JsonataBindData>().is_volatile;
	}
	bool is_volatile;
};

struct JsonataLocalState : public FunctionLocalState {
	JsonataLocalState(ClientContext &context_p, bool is_volatile_p)
	    : context(context_p), limits(ReadLimits(context_p)), is_volatile(is_volatile_p) {
	}

	jsonata::Jsonata &GetExpression(string_t expression) {
		// Fast path: the most recently used expression (always the case for a constant expression)
		if (!entries.empty() && string_t(entries.front().first) == expression) {
			return *entries.front().second;
		}
		string key = expression.GetString();
		auto it = index.find(key);
		if (it != index.end()) {
			entries.splice(entries.begin(), entries, it->second);
			return *entries.front().second;
		}
		auto parsed = ParseJsonataExpression(key);
		if (entries.size() >= EXPRESSION_CACHE_CAPACITY) {
			index.erase(entries.back().first);
			entries.pop_back();
		}
		entries.emplace_front(key, std::move(parsed));
		index[entries.front().first] = entries.begin();
		return *entries.front().second;
	}

	ClientContext &context;
	JsonataLimits limits;
	//! A volatile expression is evaluated for every row even when all arguments are constant
	bool is_volatile;

private:
	using Entry = std::pair<string, unique_ptr<jsonata::Jsonata>>;
	std::list<Entry> entries;
	std::unordered_map<string, std::list<Entry>::iterator> index;
};

static unique_ptr<FunctionLocalState> JsonataInitLocalState(ExpressionState &state, const BoundFunctionExpression &,
                                                            FunctionData *bind_data) {
	bool is_volatile = bind_data && bind_data->Cast<JsonataBindData>().is_volatile;
	return make_uniq<JsonataLocalState>(state.GetContext(), is_volatile);
}

//===--------------------------------------------------------------------===//
// Evaluation
//===--------------------------------------------------------------------===//
static string_t Evaluate(JsonataLocalState &local, jsonata::Jsonata &expr, const nlohmann::json &data,
                         const nlohmann::json *bindings, Vector &result) {
	if (bindings && !bindings->is_object()) {
		throw InvalidInputException("Bindings must be a JSON object");
	}
	EvaluationGuard guard(local.context, local.limits);
	try {
		auto frame = expr.createFrame();
		if (bindings) {
			for (auto &[key, value] : bindings->items()) {
				frame->bind(key, jsonata::Jsonata::jsonToAny(value));
			}
		}
		// Installed last so a binding can never replace the guard
		guard.Install(*frame);
		auto output = expr.evaluate(data, frame);
		return StringVector::AddString(result, output.dump());
	} catch (const Exception &) {
		guard.ThrowIfTripped();
		throw;
	} catch (const std::exception &e) {
		guard.ThrowIfTripped();
		throw InvalidInputException("JSONata evaluation error: %s", e.what());
	} catch (...) {
		guard.ThrowIfTripped();
		throw InvalidInputException("JSONata evaluation error: unknown error");
	}
}

static JsonataLocalState &GetLocalState(ExpressionState &state, DataChunk &args) {
	auto &local = ExecuteFunctionState::GetFunctionState(state)->Cast<JsonataLocalState>();
	if (local.is_volatile && args.AllConstant()) {
		// Constant arguments would make the executors evaluate once per chunk and broadcast the result
		args.Flatten();
	}
	return local;
}

static void JsonataScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local = GetLocalState(state, args);
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t expression, string_t data) {
		    auto &expr = local.GetExpression(expression);
		    auto parsed = ParseJsonData(data);
		    return Evaluate(local, expr, parsed, nullptr, result);
	    });
}

static void JsonataScalarFunWithBindings(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local = GetLocalState(state, args);
	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t expression, string_t data, string_t bindings) {
		    auto &expr = local.GetExpression(expression);
		    auto parsed = ParseJsonData(data);
		    auto parsed_bindings = ParseJsonData(bindings);
		    return Evaluate(local, expr, parsed, &parsed_bindings, result);
	    });
}

static void JsonataValidateFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t expression) {
		try {
			ParseJsonataExpression(expression.GetString());
			return true;
		} catch (const InterruptException &) {
			throw;
		} catch (const std::exception &) {
			return false;
		}
	});
}

//! $now(), $millis() and $random() (and $eval, which can call them) return a different value on every call, so
//! an expression that uses them must not be constant-folded or deduplicated. Every other expression is a pure
//! function of its inputs and keeps the default (consistent) stability.
static unique_ptr<FunctionData> JsonataBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	bool is_volatile = true;
	if (arguments[0]->IsFoldable()) {
		auto expression = ExpressionExecutor::EvaluateScalar(input.GetClientContext(), *arguments[0]);
		if (expression.IsNull()) {
			is_volatile = false;
		} else {
			auto text = StringValue::Get(expression.DefaultCastAs(LogicalType::VARCHAR));
			is_volatile = false;
			for (auto name : {"$now", "$millis", "$random", "$eval"}) {
				if (text.find(name) != string::npos) {
					is_volatile = true;
				}
			}
		}
	}
	if (is_volatile) {
		input.GetBoundFunction().SetVolatile();
	}
	return make_uniq<JsonataBindData>(is_volatile);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void LoadInternal(ExtensionLoader &loader) {
	// Best-effort: prefer json's stricter VARCHAR->JSON cast for input validation.
	// Our function still works without it (LogicalType::JSON() is a VARCHAR alias
	// and we parse via nlohmann), so don't fail extension load if autoload is off.
	ExtensionHelper::TryAutoLoadExtension(loader.GetDatabaseInstance(), "json");

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(TIMEOUT_SETTING,
	                          "Maximum wall-clock time in milliseconds of a single jsonata() evaluation (one row). "
	                          "0 disables the timeout.",
	                          LogicalType::BIGINT, Value::BIGINT(DEFAULT_TIMEOUT_MS), ValidateTimeout);
	config.AddExtensionOption(MAX_DEPTH_SETTING,
	                          "Maximum evaluation depth of a jsonata() expression, which bounds recursion in "
	                          "user-defined functions. 0 removes the depth limit (a native stack guard still applies).",
	                          LogicalType::BIGINT, Value::BIGINT(DEFAULT_MAX_DEPTH), ValidateMaxDepth);

	ScalarFunctionSet jsonata_function_set("jsonata");

	// 2-argument version: jsonata(expression, json_data)
	auto jsonata_scalar_function =
	    ScalarFunction({Positional("expression", LogicalType::VARCHAR), Positional("json_data", LogicalType::JSON())},
	                   LogicalType::JSON(), JsonataScalarFun, JsonataBind, nullptr, JsonataInitLocalState);
	jsonata_function_set.AddFunction(jsonata_scalar_function);

	// 3-argument version: jsonata(expression, json_data, bindings)
	auto jsonata_with_bindings =
	    ScalarFunction({Positional("expression", LogicalType::VARCHAR), Positional("json_data", LogicalType::JSON()),
	                    Positional("bindings", LogicalType::JSON())},
	                   LogicalType::JSON(), JsonataScalarFunWithBindings, JsonataBind, nullptr, JsonataInitLocalState);
	jsonata_function_set.AddFunction(jsonata_with_bindings);
	// Both overloads raise errors for invalid expressions, JSON data or bindings, and for exceeded limits.
	jsonata_function_set.SetFallible();

	CreateScalarFunctionInfo info(jsonata_function_set);
	info.descriptions.push_back({
	    // parameter_types
	    {LogicalType::VARCHAR, LogicalType::JSON()},
	    // parameter_names
	    {"expression", "json_data"},
	    // description
	    "Evaluates a JSONata expression against JSON data. JSONata is a lightweight query and transformation "
	    "language for JSON data. See https://jsonata.org for the full language reference.",
	    // examples
	    {"jsonata('Account.Name', '{\"Account\": {\"Name\": \"Firefly\"}}')",
	     "jsonata('$.prices[price > 100]', my_json_column)", "jsonata('$sum(Order.Product.Price)', orders)"},
	    // categories
	    {"json"},
	});
	info.descriptions.push_back({
	    // parameter_types
	    {LogicalType::VARCHAR, LogicalType::JSON(), LogicalType::JSON()},
	    // parameter_names
	    {"expression", "json_data", "bindings"},
	    // description
	    "Evaluates a JSONata expression against JSON data with external variable bindings. "
	    "The bindings parameter is a JSON object where keys become variable names accessible in the expression "
	    "using $variable_name syntax.",
	    // examples
	    {"jsonata('$name', '{}', '{\"name\": \"Alice\"}')", "jsonata('$x + $y', '{}', '{\"x\": 10, \"y\": 20}')",
	     "jsonata('items[price > $threshold]', my_json, '{\"threshold\": 100}')"},
	    // categories
	    {"json"},
	});
	loader.RegisterFunction(info);

	// jsonata_validate(expression): does the expression parse?
	ScalarFunction validate_function("jsonata_validate", {Positional("expression", LogicalType::VARCHAR)},
	                                 LogicalType::BOOLEAN, JsonataValidateFun);
	CreateScalarFunctionInfo validate_info(validate_function);
	validate_info.descriptions.push_back({
	    // parameter_types
	    {LogicalType::VARCHAR},
	    // parameter_names
	    {"expression"},
	    // description
	    "Returns true if the string is a syntactically valid JSONata expression and false otherwise, without "
	    "evaluating it. Returns NULL for NULL input.",
	    // examples
	    {"jsonata_validate('Account.Order[0].Product')", "jsonata_validate('$sum(')"},
	    // categories
	    {"json"},
	});
	loader.RegisterFunction(validate_info);

	QueryFarmSendTelemetry(loader, "jsonata", JSONATA_EXTENSION_VERSION);
}

void JsonataExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string JsonataExtension::Name() {
	return "jsonata";
}

std::string JsonataExtension::Version() const {
	return JSONATA_EXTENSION_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(jsonata, loader) {
	duckdb::LoadInternal(loader);
}
}
