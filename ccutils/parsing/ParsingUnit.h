#pragma once
#include "forward.h"
#include "../utils/utils.h"
#include <expected>

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class ParsingUnit: public Debuggable {
public:
    virtual bool canParse(const BaseParser<IN1, OUT1, STATE, ERR>& parser) const = 0;

    virtual std::expected<OUT1, ERR> parse(BaseParser<IN1, OUT1, STATE, ERR>& parser) const = 0;

    virtual LookDirection lookDirection() const = 0;

    virtual int priority() const {
        return -1;
    }

    virtual ~ParsingUnit() = default;
};

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class AheadParsingUnit: public ParsingUnit<IN1, OUT1, STATE, ERR> {
public:
    LookDirection lookDirection() const final {
        return Ahead;
    }
};

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class BehindParsingUnit: public ParsingUnit<IN1, OUT1, STATE, ERR> {
public:
    LookDirection lookDirection() const final {
        return Behind;
    }
};

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class AroundParsingUnit: public ParsingUnit<IN1, OUT1, STATE, ERR> {
public:
    LookDirection lookDirection() const final {
        return Around;
    }
};