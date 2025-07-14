#include <optional>

using namespace std;

template<typename T>
class InitVar {
    optional<T> inner;
public:
    InitVar& operator= (T value) {
        inner = value;

        return *this;
    }

    T& value() {
        if (not inner.has_value()) {
            PANIC();
        }
        return *inner;
    }
};