#pragma once
#ifndef HELPERS_H
#define HELPERS_H

#include <cstdint>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <cmath>
// #include <immintrim.h>

using namespace std;

// Helper structs:

// Basic types, but shortened:
typedef int8_t byte;
typedef uint64_t ulong;
typedef uint32_t uint;
typedef uint16_t ushort;
typedef uint8_t ubyte;

// 3D math concepts:
struct Point3i { uint32_t x; uint32_t y; uint32_t z; };
struct Point3f { float x; float y; float z; };
struct Point4i { uint32_t x; uint32_t y; uint32_t z; };
struct Point4f { float x; float y; float z; };
struct Quat3   { float x; float y; float z; };
struct Quat4   { float x; float y; float z; float w; };
struct Vector3i { Point3i position; Quat4 rotation; };
struct Vector3f { Point3f position; Quat4 rotation; };
struct Matrix3i { std::vector<Point3i> rows; static constexpr uint8_t cols = 3; };
struct Matrix3f { std::vector<Point3f> rows; static constexpr uint8_t cols = 3; };
struct Matrix4i { std::vector<Point4i> rows; static constexpr uint8_t cols = 4; };
struct Matrix4f { std::vector<Point4f> rows; static constexpr uint8_t cols = 4; };
struct Color3i { uint8_t r; uint8_t g; uint8_t b; };
struct Color4i { uint8_t r; uint8_t g; uint8_t b; float a; };

// WARNING: Fast inverse square root only works on Clang!
inline float FastInverseSquareRoot(float inVal)
{
    // Prevent division by zero.
    if (inVal < 1e-8f) return 0.0f;

    /*
    float invSqrt;
    __m128 inner = _mm_set_ss(dot);
    _mm_store_ss(&invSqrt, _mm_rsqrt_ss(inner)); // Equal to RSQRTSS in assembly.
    */

    long i;
    float y = inVal; // This is copied so that it can be moved.
    float x2 = inVal * 0.5f;
    const float threehalfs = 1.5f; // Semantic, for mathematicians reading this.

    // Move the float bits to an int register.
    asm("movd %1, %0" : "=r"(i) : "x"(y));
    // This one came from Quake III Arena, not gonna lie.
    i = 0x5f3759df - (i >> 1);
    // Move the int bits back to a float register.
    asm("movd %1, %0" : "=x"(y) : "r"(i));
    // Iterate once of Newton-Raphson.
    y = y * (threehalfs - (x2 * y * y));

    return y;
}

inline void Normalize(Quat3& in)
{
    float len = (std::pow(in.x, 2)) + (std::pow(in.y, 2)) + (std::pow(in.z, 2));

    if (len > 0.0f)
    {
        float mag = FastInverseSquareRoot(len);
        in.x /= mag;
        in.y /= mag;
        in.z /= mag;

        // You don't need to return because that was an
        // in-place operation. (hence the &)
    }
}

// Functions:

inline vector<char> ReadFile(const string& path)
{
    ifstream file(path, ios::ate | ios::binary);

    if (!file.is_open())
    {
        return {};
    }

    size_t fileSize = file.tellg();
    vector<char> buffer;
    buffer.resize(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

#endif