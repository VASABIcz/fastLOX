#pragma once
#include <string>
#include <cassert>

#include "SSARegisterHandle.h"
#include "../utils/VirtualCopy.h"
#include "../utils/utils.h"

using namespace std;

class SSARegister {
public:
    SSARegister(const SSARegister&) = default;
    SSARegister(SSARegister&&) = default;

    static constexpr string TEMP_NAME = "_";

    enum class Type {
        TMP,
        ARG,
        VAR
    };

    SSARegister(size_t blockId, string name, Type type) : name(std::move(name)), blockId(blockId), type(type) {

    }

    void incUseCount() {
        // println("INCING: {}", getHandle());
        useCount++;
    }

    void decUseCount() {
        useCount--;
    }

    bool isRoot() const {
        return !previous.has_value();
    }

    SSARegisterHandle getHandle() const {
        return SSARegisterHandle::valid(blockId, id);
    }

    void setPrevious(SSARegisterHandle prev) {
        previous = prev;
    }

    optional<SSARegisterHandle> getPrevious() const {
        return previous;
    }

    size_t getBlockId() const {
        return blockId;
    }

    void setBlockId(size_t blockId) {
        this->blockId = blockId;
    }

    [[nodiscard]] bool isTemp() const {
        return type == Type::TMP;
    }

    [[nodiscard]] bool isArgument() const {
        return type == Type::ARG;
    }

    string_view typeString() const {
        switch (type) {
            case Type::TMP: return "TMP";
            case Type::ARG: return "ARG";
            case Type::VAR: return "VAR";
            default: TODO();
        }
    }

    [[nodiscard]] SSARegister clone() const {
        return *this;
    }

    void forceStack() {
        mIsForceStack = true;
    }

    bool isForceStack() {
        return mIsForceStack;
    }

    size_t id = ~static_cast<size_t>(0);
    string name; // only root register has mName FIXME not anymore it would be so annoying :D
    size_t useCount = 0;
protected:
    size_t blockId = ~static_cast<size_t>(0);
    optional<SSARegisterHandle> previous{};
    Type type{};
    bool mIsForceStack = false;
};

template<typename T>
concept HasSizeBytes =  requires(T a) {
    { a.sizeBytes() } -> std::convertible_to<std::size_t>;
    { a.copy() } -> std::convertible_to<T>;
};

template<typename T>
concept SSAReg = HasSizeBytes<T> && std::derived_from<T, SSARegister>;