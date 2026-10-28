#pragma once
#ifndef VOLCANO_MATH_H
#define VOLCANO_MATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include "Logger.hpp"

typedef double		f64;
typedef float		f32;
typedef uint64_t	u64;
typedef int64_t		i64;
typedef uint32_t	u32;
typedef int32_t		i32;
typedef uint16_t	u16;
typedef int16_t		i16;
typedef uint8_t		u8;
typedef int8_t		i8;

// Logs `message` when `condition` is false and returns the condition, so a caller can bail out without crashing.
inline bool NonFatalAssert(bool condition, const std::string& message)
{
	if (!condition)
	{
		Volcano::Log::Error(message);
	}
	return condition;
}

/**
 * Shared helpers for animation and movement smoothing: entity interpolation, the sneak dip, the hand height.
 */
namespace Volcano::Math {

inline bool IsFinite(f32 value) { return std::isfinite(value); }

inline bool IsFinite(const glm::vec3& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/**
 * Limits a value to [min, max]. A NaN becomes min.
 */
template<typename T>
constexpr T Clamp(T value, T min, T max)
{
	if (!(value >= min)) return min;
	if (value > max) return max;
	return value;
}

/**
 * Samples a point between a start and an end, given a scalar. The scalar is usually a time from 0 to 1 and is not
 * clamped, so values past either end extrapolate. Works for f32 and glm vectors.
 * Formula: I(s, e, m) = s + (e - s) * m
 */
template<typename T>
T Interpolate(const T& start, const T& end, f32 scalar)
{
	return start + (end - start) * scalar;
}

/**
 * Interpolate for a position: if either end or the scalar is not finite the end is returned, so a bad packet snaps an
 * entity rather than sending it to NaN.
 */
inline glm::vec3 InterpolatePosition(const glm::vec3& start, const glm::vec3& end, f32 scalar)
{
	if (!IsFinite(start) || !IsFinite(end) || !IsFinite(scalar)) return end;
	return Interpolate(start, end, scalar);
}

/**
 * Interpolates an angle in radians the short way around the circle, so a step across +-pi doesn't spin the long way.
 */
inline f32 InterpolateAngle(f32 start, f32 end, f32 scalar)
{
	f32 delta = std::fmod(end - start + glm::pi<f32>(), glm::two_pi<f32>());
	if (delta < 0.0f) delta += glm::two_pi<f32>();
	delta -= glm::pi<f32>();
	return start + delta * scalar;
}

/**
 * Moves `current` toward `target` by at most `maxDelta` and never past it. Linear, for speed-limited motion such as
 * the hand height (vanilla's 0.4 per tick).
 */
inline f32 MoveToward(f32 current, f32 target, f32 maxDelta)
{
	return current + Clamp(target - current, -maxDelta, maxDelta);
}

/**
 * Eases `current` toward `target` so the same time gives the same result at any frame rate: the gap shrinks by a
 * factor of e every 1 / rate seconds. For smoothing that should look the same at 60 and 600 FPS (the sneak dip).
 * Formula: D(c, t, r, dt) = c + (t - c) * (1 - e^(-r * dt))
 */
inline f32 Damp(f32 current, f32 target, f32 rate, f32 deltaSeconds)
{
	return current + (target - current) * (1.0f - std::exp(-rate * deltaSeconds));
}

/**
 * Passes a linear value through a curve, then limits the result.
 * Formula: C(v, b, t, f) = clamp(f(v), b, t)
 */
template<typename T, typename Curve>
T Curved(T value, T min, T max, Curve curve)
{
	return Clamp<T>(curve(value), min, max);
}

/**
 * Smoothstep: 0 at 0, 1 at 1, flat at both ends. Input is clamped to [0, 1].
 */
inline f32 SmoothStep(f32 t)
{
	t = Clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

} // namespace Volcano::Math

#endif
