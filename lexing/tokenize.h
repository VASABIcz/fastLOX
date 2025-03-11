#pragma once

#include "LexingUnit.h"

template<typename T>
Result<bool> tokenajz(SourceProvider<T>& src, const vector<unique_ptr<LexingUnit<T>>>& units, vector<Token<T>>& buf) {
    for (const auto& unit: units) {
        if (!unit->canParse(src)) {
            continue;
        }
        auto res = TRY(unit->parse(src));

        if (!res.has_value())
            return true;

        buf.push_back(std::move(*res));
        return true;
    }
    return false;
}

// FIXME hack to just comoile FIXME
template<typename T>
Result<vector<Token<T>>> tokenize1(SourceProvider<T>& src, const vector<unique_ptr<LexingUnit<T>>>& units) {
    vector<Token<T>> buf{};

    while (!src.isDone()) {
        auto res = tokenajz(src, units, buf);
        if (not res.has_value())
            return std::unexpected(std::move(res.error()));
        if (!*res) {
            string strBuf;

            for (auto i = 0; i < 10; i++) {
                if (src.isPeekChar(' ')) break;
                strBuf += src.consume();
            }

            println("GGGGGGGGGGGGGGGGGGGGGGGGGGGG");
            return FAILF("unknown token: '{}'", strBuf);
        }
    }
    // IFD cout << "success " << buf.SIZE() << endl;

    return {std::move(buf)};
}