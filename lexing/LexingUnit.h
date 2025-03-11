#pragma once

#include "forward.h"
#include <string>
#include <optional>
#include <expected>
#include <cassert>
#include <algorithm>
#include <utility>
#include <memory>

#include "../utils/utils.h"
#include "../utils/Errorable.h"
#include "Position.h"

using namespace std;

template <typename T>
class LexingUnit: public Debuggable {
public:
    virtual bool canParse(const SourceProvider<T> &) const = 0;

    virtual Result<optional<Token<T>>> parse(SourceProvider<T> &) const = 0;

    virtual ~LexingUnit() = default;
};