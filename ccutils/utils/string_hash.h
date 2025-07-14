#pragma once

#include <string_view>

using namespace std;

// https://www.reddit.com/r/cpp_questions/comments/12xw3sn/find_stdstring_view_in_unordered_map_with/
// https://stackoverflow.com/questions/34596768/stdunordered-mapfind-using-a-type-different-than-the-key-type

template<typename ... Bases>
struct overload : Bases ... {
    using is_transparent = void;
    using Bases::operator() ...;
};


struct char_pointer_hash {
    auto operator()(const char* ptr) const noexcept {
        return std::hash<std::string_view>{}(ptr);
    }
};

using transparent_string_hash = overload<
        std::hash<std::string>,
        std::hash<std::string_view>,
        char_pointer_hash
>;