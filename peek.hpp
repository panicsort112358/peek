// peek.hpp - compact, bounded C++20 developer diagnostics
// Copyright (c) 2026 njlane314
// SPDX-License-Identifier: MIT
// https://github.com/njlane314/peek

#ifndef PEEK_PEEK_HPP
#define PEEK_PEEK_HPP

// Configuration ----------------------------------------------------------------

// Diagnostics are opt-in. The disabled branch includes no standard-library
// machinery, so leaving the default unchanged has no standard-library cost.
#ifndef PEEK_COMPILED
#define PEEK_COMPILED 0
#endif

#if PEEK_COMPILED

// Standard-library dependencies ------------------------------------------------

#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <ostream>
#include <ranges>
#include <source_location>
#include <streambuf>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace peek {

// Public types -----------------------------------------------------------------

// These bounds keep diagnostics finite even when values are deeply nested or
// unexpectedly large. A reached bound truncates the diagnostic rather than
// rejecting the value.
struct format_options {
    // Maximum recursive expansion of optionals, variants, tuples, and ranges.
    std::size_t max_depth = 5;
    // Maximum number of elements inspected from any one forward range.
    std::size_t max_items = 32;
    // Maximum input bytes rendered from a string; the default bounds scope labels too.
    std::size_t max_string = 256;
    // Maximum bytes retained by a writer; the default also caps output records.
    std::size_t max_record = 4096;
};

// This bounded writer is also the target exposed to ADL diagnostic_format
// overloads, letting custom formatters use ordinary stream insertion. Its
// noexcept surface turns allocation and stream failures into bounded text.
class writer final : public std::streambuf {
public:
    explicit writer(format_options options = {}) noexcept : writer(options.max_record) {}
    explicit writer(std::size_t limit) noexcept : limit_(limit) {}

    void append(std::string_view text) noexcept
    {
        if (text.empty()) return;
        if (text_.size() >= limit_) {
            truncated_ = true;
            return;
        }
        try {
            const auto room = limit_ - text_.size();
            const auto count = text.size() < room ? text.size() : room;
            text_.append(text.data(), count);
            truncated_ = truncated_ || count != text.size();
        } catch (...) { truncated_ = true; }
    }

    void push(char value) noexcept { append({&value, 1}); }

    // The view is non-owning and remains valid only until mutation or destruction.
    [[nodiscard]] std::string_view view() const noexcept { return text_; }

    // Finalization applies the truncation marker and consumes the accumulated text.
    [[nodiscard]] std::string finish() noexcept
    {
        try {
            if (truncated_ && limit_ >= 3) {
                if (text_.size() < 3) text_.append(3 - text_.size(), '.');
                else text_.replace(text_.size() - 3, 3, "...");
            }
            return std::move(text_);
        } catch (...) { return "<format error>"; }
    }

    template<class T>
        requires (std::is_pointer_v<std::remove_cvref_t<T>> ||
                  requires(std::ostream& stream, const T& item) { stream << item; })
    writer& operator<<(const T& value) noexcept
    {
        try {
            std::ostream stream{this};
            using type = std::remove_reference_t<T>;
            if constexpr (std::is_array_v<type> &&
                          std::same_as<std::remove_cv_t<std::remove_extent_t<type>>, char>) {
                constexpr auto size = std::extent_v<type>;
                append({value, size != 0 && value[size - 1] == '\0' ? size - 1 : size});
            } else if constexpr (std::is_pointer_v<std::remove_cvref_t<T>>) {
                if (value == nullptr) append("nullptr");
                else stream << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(value);
            } else {
                stream << value;
            }
        } catch (...) { append("<format error>"); }
        return *this;
    }

private:
    std::streamsize xsputn(const char* text, std::streamsize count) override
    {
        if (count > 0) append({text, static_cast<std::size_t>(count)});
        return count;
    }

    int_type overflow(int_type value) override
    {
        if (!traits_type::eq_int_type(value, traits_type::eof()))
            push(traits_type::to_char_type(value));
        return traits_type::not_eof(value);
    }

    std::string text_;
    std::size_t limit_;
    bool truncated_ = false;
};

// A sink is called synchronously with one complete, newline-terminated record.
// Calls are serialized; the supplied view is valid only for the duration of the call.
using sink = std::function<void(std::string_view)>;

namespace detail {

using buffer = writer;

// Output -----------------------------------------------------------------------

struct errno_guard {
    // Diagnostics must not overwrite errno observed by the diagnosed program.
    errno_guard() noexcept : saved(errno) {}
    ~errno_guard() noexcept { errno = saved; }
    int saved;
};

inline void write_stderr(std::string_view record)
{
    static_cast<void>(std::fwrite(record.data(), 1, record.size(), stderr));
    static_cast<void>(std::fflush(stderr));
}

inline sink active_sink = write_stderr;
inline std::recursive_mutex sink_mutex;
inline thread_local bool inside_sink = false;

inline void emit(std::string_view record) noexcept
{
    // Preserve diagnosed program state, serialize complete sink calls, suppress
    // recursive emission, and let no sink exception escape.
    errno_guard preserve_errno;
    if (inside_sink) return;
    try {
        std::lock_guard lock{sink_mutex};
        sink destination = active_sink;
        if (!destination) return;
        inside_sink = true;
        try { destination(record); } catch (...) {}
        inside_sink = false;
    } catch (...) { inside_sink = false; }
}

template<class T>
using plain_t = std::remove_cvref_t<T>;

// Formatting -------------------------------------------------------------------

template<class T>
inline constexpr bool is_character_v =
    std::same_as<plain_t<T>, char> || std::same_as<plain_t<T>, signed char> ||
    std::same_as<plain_t<T>, unsigned char>;

template<class T> inline constexpr bool is_string_v = false;
template<class Traits, class Allocator>
inline constexpr bool is_string_v<std::basic_string<char, Traits, Allocator>> = true;
template<class Traits>
inline constexpr bool is_string_v<std::basic_string_view<char, Traits>> = true;

template<class T>
inline constexpr bool is_char_array_v =
    std::is_array_v<std::remove_reference_t<T>> &&
    std::same_as<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>;

template<class T> inline constexpr bool is_optional_v = false;
template<class Value> inline constexpr bool is_optional_v<std::optional<Value>> = true;
template<class T> inline constexpr bool is_variant_v = false;
template<class... Values> inline constexpr bool is_variant_v<std::variant<Values...>> = true;

template<class T>
concept tuple_like = requires { typename std::tuple_size<plain_t<T>>::type; };

// Single-pass input ranges are excluded so diagnostics never consume them.
template<class T>
concept map_like = std::ranges::forward_range<const plain_t<T>&> && requires {
    typename plain_t<T>::key_type;
    typename plain_t<T>::mapped_type;
};

template<class T>
concept stream_insertable = requires(std::ostream& stream, const T& item) { stream << item; };

template<class T>
concept adl_formattable = requires(writer& out, const T& item) { diagnostic_format(out, item); };

inline void append_escaped(buffer& out, char value, char quote) noexcept
{
    // Escaping ASCII controls and delimiters prevents embedded LF record breaks.
    switch (value) {
    case '\\': out.append("\\\\"); break;
    case '\n': out.append("\\n"); break;
    case '\r': out.append("\\r"); break;
    case '\t': out.append("\\t"); break;
    case '\0': out.append("\\0"); break;
    default:
        if (value == quote) out.push('\\');
        if (static_cast<unsigned char>(value) < 0x20U ||
            static_cast<unsigned char>(value) == 0x7fU) {
            constexpr char hex[] = "0123456789abcdef";
            const auto byte = static_cast<unsigned char>(value);
            out.append("\\x");
            out.push(hex[(byte >> 4U) & 0x0fU]);
            out.push(hex[byte & 0x0fU]);
        } else out.push(value);
    }
}

inline void append_string(buffer& out, std::string_view value, const format_options& options) noexcept
{
    out.push('"');
    const auto count = value.size() < options.max_string ? value.size() : options.max_string;
    for (std::size_t index = 0; index < count; ++index) {
        append_escaped(out, value[index], '"');
    }
    if (count != value.size()) out.append("...");
    out.push('"');
}

template<class T>
void format_value(buffer& out, const T& value, const format_options& options, std::size_t depth);

template<class Tuple>
void format_tuple(buffer& out, const Tuple& value, const format_options& options, std::size_t depth)
{
    out.push('(');
    std::size_t index = 0;
    std::apply(
        [&](const auto&... items) {
            ((out.append(index++ == 0 ? "" : ", "),
              format_value(out, items, options, depth + 1)),
             ...);
        },
        value);
    out.push(')');
}

template<bool Map, class Range>
void format_range(buffer& out,
                  const Range& value,
                  const format_options& options,
                  std::size_t depth)
{
    // Stop at max_items without traversing the remainder. Sized ranges can still
    // report the omitted count without consuming additional elements.
    out.push(Map ? '{' : '[');
    auto iterator = std::ranges::begin(value);
    const auto end = std::ranges::end(value);
    std::size_t count = 0;
    for (; iterator != end && count < options.max_items; ++iterator, ++count) {
        out.append(count == 0 ? "" : ", ");
        if constexpr (Map) {
            format_value(out, std::get<0>(*iterator), options, depth + 1);
            out.append(": ");
            format_value(out, std::get<1>(*iterator), options, depth + 1);
        } else {
            format_value(out, *iterator, options, depth + 1);
        }
    }
    if (iterator != end) {
        out.append(count == 0 ? "..." : ", ...");
        if constexpr (std::ranges::sized_range<Range>) {
            const auto size = static_cast<std::uintmax_t>(std::ranges::size(value));
            if (size >= count) out << ' ' << size - count << " more";
        }
    }
    out.push(Map ? '}' : ']');
}

template<class T>
void format_value(buffer& out, const T& value, const format_options& options, std::size_t depth)
{
    // User-defined ADL formatting wins. Native scalar and vocabulary types then
    // precede structural ranges, with stream insertion as the final fallback.
    using value_type = plain_t<T>;
    if constexpr (adl_formattable<T>) {
        writer custom{options};
        diagnostic_format(custom, value);
        out.append(custom.view());
    } else if constexpr (is_string_v<value_type>) {
        append_string(out, std::string_view{value}, options);
    } else if constexpr (is_char_array_v<T>) {
        constexpr auto extent = std::extent_v<std::remove_reference_t<T>>;
        const auto size = extent != 0 && value[extent - 1] == '\0' ? extent - 1 : extent;
        append_string(out, std::string_view{value, size}, options);
    } else if constexpr (is_character_v<T>) {
        out.push('\'');
        append_escaped(out, static_cast<char>(value), '\'');
        out.push('\'');
    } else if constexpr (std::same_as<value_type, bool>) {
        out.append(value ? "true" : "false");
    } else if constexpr (std::integral<value_type> || std::floating_point<value_type>) {
        std::ostream stream{&out};
        stream << value;
    } else if constexpr (std::is_enum_v<value_type>) {
        format_value(out, static_cast<std::underlying_type_t<value_type>>(value), options, depth);
    } else if constexpr (std::same_as<value_type, std::nullptr_t>) {
        out.append("nullptr");
    } else if constexpr (std::is_pointer_v<value_type>) {
        out << value;
    } else if constexpr (is_optional_v<value_type>) {
        if (!value) out.append("nullopt");
        else if (depth >= options.max_depth) out.append("...");
        else format_value(out, *value, options, depth + 1);
    } else if constexpr (is_variant_v<value_type>) {
        if (value.valueless_by_exception()) out.append("variant<valueless>");
        else if (depth >= options.max_depth) out.append("variant<...>");
        else {
            out.append("variant<");
            out << value.index();
            out.append(">(");
            std::visit([&](const auto& item) { format_value(out, item, options, depth + 1); }, value);
            out.push(')');
        }
    } else if constexpr (tuple_like<T>) {
        if (depth >= options.max_depth) out.append("(...)");
        else format_tuple(out, value, options, depth);
    } else if constexpr (map_like<T>) {
        if (depth >= options.max_depth) out.append("{...}");
        else format_range<true>(out, value, options, depth);
    } else if constexpr (std::ranges::forward_range<const value_type&>) {
        if (depth >= options.max_depth) out.append("[...]");
        else format_range<false>(out, value, options, depth);
    } else if constexpr (stream_insertable<T>) {
        std::ostream stream{&out};
        stream << value;
    }
    else out.append("<unformattable>");
}

template<class T>
std::string safe_format(const T& value, format_options options = {}) noexcept
{
    // Keep formatting failure out of program control flow: preserve errno and
    // contain all formatter exceptions behind a stable fallback value.
    errno_guard preserve_errno;
    try {
        buffer output{options.max_record};
        format_value(output, value, options, 0);
        return output.finish();
    } catch (...) {
        return "<format error>";
    }
}

// Expression-name parsing ------------------------------------------------------

// Macro spelling is not a C++ syntax tree. Nested delimiters and likely
// template angles are handled best-effort, with stable fallback names.
inline std::string_view trim(std::string_view text) noexcept
{
    const auto first = text.find_first_not_of(" \t\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\n");
    return text.substr(first, last - first + 1);
}

inline bool scan_quote(char value, char& quote, bool& escaped) noexcept
{
    if (quote == 0) {
        if (value != '"' && value != '\'') return false;
        quote = value;
    } else if (escaped) escaped = false;
    else if (value == '\\') escaped = true;
    else if (value == quote) quote = 0;
    return true;
}

inline bool plausible_template(std::string_view text, std::size_t opening) noexcept
{
    // Treat angle brackets as template syntax only when a plausible closing token
    // exists; this helps avoid hiding commas after comparison expressions.
    std::size_t depth = 1;
    char quote = 0;
    bool escaped = false;
    for (std::size_t index = opening + 1; index < text.size(); ++index) {
        const char value = text[index];
        if (scan_quote(value, quote, escaped)) continue;
        if (value == '<') ++depth;
        else if (value == '>' && --depth == 0) {
            const auto following = text.find_first_not_of(" \t\n", index + 1);
            if (following == std::string_view::npos) return true;
            const char next = text[following];
            const bool operand = (next >= 'a' && next <= 'z') ||
                                 (next >= 'A' && next <= 'Z') ||
                                 (next >= '0' && next <= '9') || next == '_' ||
                                 next == '\'' || next == '"' || next == '=';
            return !operand;
        }
    }
    return false;
}

inline std::vector<std::string_view> split_names(std::string_view text,
                                                  std::size_t expected,
                                                  bool angles = true)
{
    // If the angle heuristic disagrees with the value count, retry without it.
    // Truly ambiguous spellings receive stable generic names instead of failing.
    std::vector<std::string_view> names;
    names.reserve(expected);
    if (expected < 2) {
        if (expected == 1) names.push_back(trim(text));
        return names;
    }

    std::size_t start = 0;
    std::size_t groups = 0;
    std::size_t angle = 0;
    char quote = 0;
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (scan_quote(value, quote, escaped)) continue;
        if (value == '(' || value == '[' || value == '{') ++groups;
        else if ((value == ')' || value == ']' || value == '}') && groups != 0) --groups;
        else if (angles && value == '<' && plausible_template(text, index)) ++angle;
        else if (angles && value == '>' && angle != 0) --angle;
        else if (value == ',' && groups == 0 && angle == 0) {
            names.push_back(trim(text.substr(start, index - start)));
            start = index + 1;
        }
    }
    names.push_back(trim(text.substr(start)));
    if (names.size() != expected && angles) return split_names(text, expected, false);
    if (names.size() != expected) names.assign(expected, "value");
    return names;
}

inline std::string_view basename(std::string_view path) noexcept
{
    // Accept both POSIX and Windows source paths in compiler-provided locations.
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Output records and scopes ----------------------------------------------------

// Each thread owns its active scope path so concurrent nested scopes do not mix.
inline std::vector<std::string>& scope_stack() noexcept
{
    thread_local std::vector<std::string> stack;
    return stack;
}

inline void append_context(buffer& out, const std::source_location& location) noexcept
{
    out.append(basename(location.file_name()));
    out.push(':');
    out << location.line();
    out.append("  ");
    const auto& stack = scope_stack();
    for (std::size_t index = 0; index < stack.size(); ++index) {
        out.append(index == 0 ? "" : "/");
        out << stack[index];
    }
    if (!stack.empty()) out.append("  ");
}

template<class Body>
void emit_record(const std::source_location& location, Body&& body) noexcept
{
    // Reserve the final byte for '\n' so the sink always sees a bounded record.
    errno_guard preserve_errno;
    try {
        buffer output{format_options{}.max_record - 1};
        append_context(output, location);
        std::forward<Body>(body)(output);
        auto record = output.finish();
        record.push_back('\n');
        emit(record);
    } catch (...) {}
}

class dump {
public:
    template<class... Values>
    dump(std::source_location location,
         std::string_view condition,
         std::string_view expressions,
         Values&&... values) noexcept
    {
        // The macro supplies source spellings beside the captured values. Parsing
        // failure affects labels only; formatting and emission still proceed.
        std::vector<std::string_view> names;
        try { names = split_names(expressions, sizeof...(Values)); } catch (...) {}
        emit_record(location, [&](buffer& output) noexcept {
            if (!condition.empty()) output << "if(" << trim(condition) << "): ";
            if constexpr (sizeof...(Values) == 0) output.append("<no values>");
            else {
                std::size_t index = 0;
                auto append = [&](const auto& value) noexcept {
                    output.append(index == 0 ? "" : ", ");
                    output << (index < names.size() ? names[index] : std::string_view{"value"})
                           << " = " << safe_format(value);
                    ++index;
                };
                (append(values), ...);
            }
        });
    }
};

template<class Name>
std::string scope_label(const Name& name) noexcept
{
    // String labels stay unquoted for readable paths; all other labels use the
    // same bounded, exception-contained formatter as ordinary values.
    errno_guard preserve_errno;
    try {
        buffer output{format_options{}.max_string};
        if constexpr (is_string_v<plain_t<Name>>) {
            output.append(std::string_view{name});
        } else if constexpr (is_char_array_v<Name>) {
            constexpr auto extent = std::extent_v<std::remove_reference_t<Name>>;
            const auto size = extent != 0 && name[extent - 1] == '\0' ? extent - 1 : extent;
            output.append(std::string_view{name, size});
        } else output.append(safe_format(name));
        return output.finish();
    } catch (...) { return "<scope>"; }
}

inline void emit_scope(const std::source_location& location,
                       std::string_view action,
                       std::optional<double> elapsed_ms = std::nullopt,
                       bool unwinding = false) noexcept
{
    emit_record(location, [&](buffer& output) {
        output.append(action);
        if (elapsed_ms) {
            output.append(" in ");
            std::ostream stream{&output};
            stream << std::fixed << std::setprecision(2) << *elapsed_ms << " ms";
        }
        if (unwinding) output.append(" while unwinding");
    });
}

} // namespace detail

// Public API -------------------------------------------------------------------

// Replacing the sink is serialized with emission. An empty sink disables output;
// reset_sink restores the default stderr destination.
inline void set_sink(sink destination) noexcept
{
    detail::errno_guard preserve_errno;
    try {
        std::lock_guard lock{detail::sink_mutex};
        detail::active_sink = std::move(destination);
    } catch (...) {}
}

inline void reset_sink() noexcept { set_sink(detail::write_stderr); }

// format exposes the value renderer without adding source context or emitting.
template<class T>
[[nodiscard]] std::string format(const T& value, format_options options = {}) noexcept
{
    return detail::safe_format(value, options);
}

// scope is the RAII engine behind PEEK_SCOPE: it records entry, elapsed time,
// nesting, and whether destruction occurs during stack unwinding.
class scope {
public:
    template<class Name>
    scope(detail::errno_guard&&, std::source_location location, Name&& name) noexcept
        : location_(location), started_(std::chrono::steady_clock::now()),
          uncaught_(std::uncaught_exceptions())
    {
        try {
            detail::scope_stack().push_back(detail::scope_label(name));
            active_ = true;
            detail::emit_scope(location_, "entering");
        } catch (...) {
        }
    }

    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;

    ~scope() noexcept
    {
        detail::errno_guard preserve_errno;
        if (active_) {
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started_).count();
            detail::emit_scope(location_, "finished", elapsed,
                               std::uncaught_exceptions() > uncaught_);
            detail::scope_stack().pop_back();
        }
    }

private:
    std::source_location location_;
    std::chrono::steady_clock::time_point started_;
    int uncaught_;
    bool active_ = false;
};

} // namespace peek

// PEEK evaluates each value once. PEEK_IF evaluates its condition once and its
// values only when true. PEEK_SCOPE evaluates its label once and lasts for its block.
// Scope variables use __LINE__, so place at most one PEEK_SCOPE on a source line.
#define PEEK_DETAIL_JOIN_INNER(left, right) left##right
#define PEEK_DETAIL_JOIN(left, right) PEEK_DETAIL_JOIN_INNER(left, right)
#define PEEK_DETAIL_ERRNO_GUARD                                                    \
    ::peek::detail::errno_guard PEEK_DETAIL_JOIN(peek_errno_guard_, __LINE__)

#define PEEK(...)                                                                    \
    do {                                                                             \
        PEEK_DETAIL_ERRNO_GUARD;                                                     \
        ::peek::detail::dump{std::source_location::current(), {},                   \
                             std::string_view{#__VA_ARGS__}                          \
                                 __VA_OPT__(, ) __VA_ARGS__};                        \
    } while (false)

#define PEEK_IF(condition, ...)                                                       \
    do {                                                                             \
        PEEK_DETAIL_ERRNO_GUARD;                                                     \
        if (static_cast<bool>(condition))                                            \
            ::peek::detail::dump{std::source_location::current(),                    \
                                 std::string_view{#condition},                       \
                                 std::string_view{#__VA_ARGS__}                      \
                                     __VA_OPT__(, ) __VA_ARGS__};                    \
    } while (false)

#define PEEK_SCOPE(name)                                                             \
    [[maybe_unused]] ::peek::scope PEEK_DETAIL_JOIN(peek_scope_, __LINE__){          \
        ::peek::detail::errno_guard{}, std::source_location::current(), (name)}

#else

// Disabled macros do not mention their arguments: expressions are neither
// evaluated nor required to compile.
#define PEEK(...) do {} while (false)
#define PEEK_IF(...) do {} while (false)
#define PEEK_SCOPE(...) do {} while (false)

#endif

#endif
