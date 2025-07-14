#pragma once
#include <memory>
#include <vector>
#include "utils.h"

using namespace std;


// FIXME Extremly naive implementation
// maybe add debuging capability?
template<typename T>
class CopyPtr {
    unique_ptr<T> inner;
public:
    // CopyPtr(T* ptr) noexcept : inner(ptr) {}

    constexpr T* getPtr() {
        return inner.get();
    }

    CopyPtr(unique_ptr<T> ptr) noexcept: inner(std::move(ptr.release())) {}

    CopyPtr<T>& operator=(CopyPtr<T>&& other) noexcept {
        this->inner = std::move(other.inner);

        return *this;
    }

    CopyPtr(CopyPtr<T>&& rhs) noexcept {
        this->inner = std::move(rhs.inner);
    }

    constexpr explicit CopyPtr(const CopyPtr<T>& rhs) noexcept {
        if constexpr (std::constructible_from<T, const T&>) {
            this->inner = rhs.inner == nullptr ? nullptr : make_unique<T>(*rhs.inner.get());
        }
        else {
            this->inner = rhs.inner == nullptr ? nullptr : unique_ptr<T>(rhs->deepCopy());
        }
    }

    CopyPtr<T>& operator=(const CopyPtr<T>& rhs) noexcept {
        if constexpr (std::constructible_from<T, const T&>) {
            this->inner = rhs.inner == nullptr ? nullptr : make_unique<T>(*rhs.inner.get());
        }
        else {
            this->inner = rhs.inner == nullptr ? nullptr : unique_ptr<T>(rhs->deepCopy());
        }

        return *this;
    }

    T& operator*() const {
        validatePtr();
        return *inner;
    }

    T* operator->() const {
        return inner.get();
    }

    T* get() const {
        return inner.get();
    }

    unique_ptr<T>& uPtrRef() {
        return inner;
    }

    bool isValid() {
        return inner != nullptr;
    }

    bool operator==(nullptr_t) const {
        return inner == nullptr;
    }

    template<typename T1>
    CopyPtr<T1> cast() {
        return unique_ptr<T1>(dynamic_cast<T*>(inner.release()));
    }

    template<typename T1>
    T1* unsafeCast() {
        auto ptr = dynamic_cast<T1*>(inner.get());
        assert(ptr != nullptr);
        return ptr;
    }

private:
    void validatePtr() const {
        if (inner == nullptr) PANIC();
    }
};

template<typename T>
vector<CopyPtr<T>> vectorBridge(vector<unique_ptr<T>> items) {
    vector<CopyPtr<T>> buf;

    for (auto i = 0u; i < items.size(); i++) {
        unique_ptr<T> item = std::move(items[i]);
        buf.push_back(CopyPtr(std::move(item)));
    }

    return buf;
}

template<typename T, typename... Args>
CopyPtr<T> makeCpyPtr(Args&&... args) {
    return CopyPtr(make_unique<T>(std::forward<Args>(args)...));
}