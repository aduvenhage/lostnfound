#ifndef LIBS_HEADER_CONSTANTS_H
#define LIBS_HEADER_CONSTANTS_H

#define _USE_MATH_DEFINES
#include <cmath>
#include <random>


constexpr double deg2rad(double _dDeg) {
    return _dDeg / 180.0 * M_PI;
}


constexpr double sqr(double _a) {
    return _a * _a;
}


namespace LNF
{
    using RandomGen = std::mt19937_64;

};  //namespace LNF


#endif  // #ifndef LIBS_HEADER_CONSTANTS_H
