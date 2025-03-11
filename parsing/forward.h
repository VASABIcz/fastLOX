#pragma once

enum LookDirection {
    Ahead,
    Behind,
    Around
};

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class Parser;

template<typename IN1, typename OUT1, typename STATE, typename ERR>
class BaseParser;
