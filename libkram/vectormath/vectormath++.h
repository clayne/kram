// kram - Copyright 2020-2024 by Alec Miller. - MIT License
// The license and copyright notice shall be included
// in all copies or substantial portions of the Software.

#pragma once

#if USE_SIMDLIB

// This requires __clang__ or gcc.
// Only really targeting __clang__ for development.
// Targeting x64 running AVX2 and Neon.
//
// gcc/clang vector extension:
// .lo and .hi are the first second halves of a vector,
// .even and .odd are the even- and odd-indexed elements of a vector.
// __builtin_shufflevector and __builtin_convertvector.
// also have .rgba built into the types
// So can emulate double4 on Neon using 2 registers.
// These extensions only work on a C typedef and auto convert to
//  _m128, _m128i, _m256, .. on Intel, and to float32x4_t on Neon.
//
// Apple simd lib has versioning, so the Accelerate lib provides optimized
// simd calls.  And if not present, then those calls use 4 math.h function calls
// or fail to build.  So v0 is basic functionaliy, and it's up to v6 on iOS 18.
//
// x64: Compile this with -march x86_64h (haswell) or -mavx2 -mf16c -fma
// arm64: Compile this with Neon enabled (should have fma)
//
// Intel SSE core has 3 or 4 32B units internally.
//   One core is shared by 2 Hyperthreads, but HT is being removed by Intel.
//   e-cores can only run AVX2, and don't have HT support.
// AVX-512 can drop to 1 unit rounding down.
// Chip frequencies drop using AVX2 and AVX-512.
// Trying to run AVX2 (32B) can even run slow on older Intel/AMD
//   with double-pumped 16B internal cores.
// New AMD runs double-pumped 32B instructions for AVX-512 on all cores.
//  AMD has e-cores, but no instruction limits like Intel.
//
// Intel SSE scalar ops used to run 2:1 for 1:4 ops of work
// but now it's 1:1, so these only keep value in sse register.
//
// This passes vector by value, and matrix by const reference to the ops.
// May want to reconsider const ref for long uint/float/double vectors.
// This assumes x64 calling conventions for passing registers.
// There are differing function calling conventions for passing first 4 values.
//
// Neon    32x 32B 128-bt
// SVE2    ?
// +fp16   _Float16 support
//
// SSE     16x 16B 128-bit
// AVX/2   16x 32B 256-bit
// AVX-512 16x 64B 512-bit (disabled on p-cores and dropped from e-cores on i9), 4 variants
// AVX10   32x 32B 256-bit (emulates 512-bit), 3 variants
//
// FMA     fp multiply add (clang v14)
// F16C    2 ops fp16 <-> fp32
// CRC32   instructions to enable fast crc ops (not using yet, but is in sse2neon.h)
//
// max vec size per register
// 16B      32B
// char16   char16?
// short8   short16
// uint4    uint8
// float4   float8
// double2  double4
//
// Metal Shading Language (MSL)
// supports up to half4
// supports up to float4
// no support for double.  cpu only
//
// HLSL and DX12/Vulkan support double on desktop, but not mobile?
//   and likely not on arm64 gpus.
//
//------------
// x64 -> arm64 emulators
// Prism   supports SSE4.2, no fma, no f16c
// Rosetta supports SSE4.2, no fma, no f16c
// Rosetta supports AVX2 (macOS 15.0)
//
//------------
// Types for 32B max vector size (2 Neon reg, 1 AVX/AVX2 reg)
// char2,3,4,8,16,32
// int2,3,4,8
//
// half2,3,4,8,16
// float2,3,4,8
// double2,3,4
//
//------------
// APX first introduced in 10th gen has APX extension
//   expands general purpose registers from 16 to 32.
//
// Intel chips
//  1 Nehalem,
//  2 Sandy Bridge,
//  3 Ivy Bridge,
//  4 Haswell,       AVX2
//  5 Broadwell,
//  6 Sky Lake,
//  7 Kaby Lake,
//  8 Coffee Lake,
//  9 Coffee Lake Refresh
// 10 Comet Lake,    APX
// 11 Rocket Lake,
// 12 Alder Lake,
// 13 Raptor Lake
//
// AMD chips
//
//
// Apple Silicon
// iPhone 5S has arm64 arm64-v?
//

//-----------------------------------
// config

// Can override the namespace to your own.  This avoids collision with Apple simd.
#ifndef SIMD_NAMESPACE
#define SIMD_NAMESPACE simdk
#endif // SIMD_NAMESPACE

// only support avx2 and Neon, no avx-512 at first
#if defined __ARM_NEON
#define SIMD_SSE  0
#define SIMD_NEON 1
#elif defined __AVX2__ // x64 AVX2 or higher, can lower to AVX
#define SIMD_SSE  1
#define SIMD_NEON 0
#else
#warning unuspported simd arch
#endif

// a define to override setings from prefix file
#ifndef SIMD_CONFIG

// Vector and matrix types.  Currently only matrix types for SIMD_FLOAT, SIMD_DOUBLE.
// SIMD_INT must be kept on for conditional tests.
#define SIMD_HALF   1
#define SIMD_FLOAT  1
#define SIMD_DOUBLE 1

#define SIMD_INT    1
#define SIMD_CHAR   0
//#define SIMD_UCHAR  0
#define SIMD_SHORT  0
//#define SIMD_USHORT 0
#define SIMD_LONG   0
//#define SIMD_ULONG  0

// Whether to support > 4 length vecs with some ops
#define SIMD_FLOAT_EXT 0

// This means simd_float4 will come from this file instead of simd.h
#define SIMD_RENAME_TO_SIMD_NAMESPACE 0

#endif // SIMD_CONFIG

//-----------------------------------

// simplify calls
// const means it doesn't pull from global changing state (what about constants)
// and inline is needed or get unused static calls, always_inline forces inline
// of these mostly wrapper calls.
#define SIMD_CALL static inline __attribute__((__always_inline__,__const__,__nodebug__))

// op *=, +=, -=, /= mods the calling object, so can't be const
#define SIMD_CALL_OP static inline __attribute__((__always_inline__,__nodebug__))

// TODO: type1 simdk::log(float1)
// type##1 cppfunc(type##1 x);

#define macroVectorRepeatFnDecl(type, cppfunc) \
type##2 cppfunc(type##2 x); \
type##3 cppfunc(type##3 x); \
type##4 cppfunc(type##4 x); \

//------------

// aligned
#define macroVector1TypesStorage(type, name) \
typedef type name##1s; \
typedef __attribute__((__ext_vector_type__(2)))  type name##2s; \
typedef __attribute__((__ext_vector_type__(3)))  type name##3s; \
typedef __attribute__((__ext_vector_type__(4)))  type name##4s; \
typedef __attribute__((__ext_vector_type__(8)))  type name##8s; \
typedef __attribute__((__ext_vector_type__(16))) type name##16s; \
typedef __attribute__((__ext_vector_type__(32),__aligned__(16))) type name##32s; \

// packed
#define macroVector1TypesPacked(type, name) \
typedef type name##1p; \
typedef __attribute__((__ext_vector_type__(2),__aligned__(1)))  type name##2p; \
typedef __attribute__((__ext_vector_type__(3),__aligned__(1)))  type name##3p; \
typedef __attribute__((__ext_vector_type__(4),__aligned__(1)))  type name##4p; \
typedef __attribute__((__ext_vector_type__(8),__aligned__(1)))  type name##8p; \
typedef __attribute__((__ext_vector_type__(16),__aligned__(1))) type name##16p; \
typedef __attribute__((__ext_vector_type__(32),__aligned__(1))) type name##32p; \

// cpp rename for u/char
#define macroVector1TypesStorageRenames(cname, cppname) \
typedef ::cname##1s cppname##1; \
typedef ::cname##2s cppname##2; \
typedef ::cname##3s cppname##3; \
typedef ::cname##4s cppname##4; \
typedef ::cname##8s cppname##8; \
typedef ::cname##16s cppname##16; \
typedef ::cname##32s cppname##32; \

//------------

// aligned
#define macroVector2TypesStorage(type, name) \
typedef type name##1s; \
typedef __attribute__((__ext_vector_type__(2)))  type name##2s; \
typedef __attribute__((__ext_vector_type__(3)))  type name##3s; \
typedef __attribute__((__ext_vector_type__(4)))  type name##4s; \
typedef __attribute__((__ext_vector_type__(8)))  type name##8s; \
typedef __attribute__((__ext_vector_type__(16),__aligned__(16))) type name##16s; \
// also a 32

// packed
#define macroVector2TypesPacked(type, name) \
typedef type name##1p; \
typedef __attribute__((__ext_vector_type__(2),__aligned__(2)))  type name##2p; \
typedef __attribute__((__ext_vector_type__(3),__aligned__(2)))  type name##3p; \
typedef __attribute__((__ext_vector_type__(4),__aligned__(2)))  type name##4p; \
typedef __attribute__((__ext_vector_type__(8),__aligned__(2)))  type name##8p; \
typedef __attribute__((__ext_vector_type__(16),__aligned__(2))) type name##16p; \

// cpp rename for half, u/short
#define macroVector2TypesStorageRenames(cname, cppname) \
typedef ::cname##1s cppname##1; \
typedef ::cname##2s cppname##2; \
typedef ::cname##3s cppname##3; \
typedef ::cname##4s cppname##4; \
typedef ::cname##8s cppname##8; \
typedef ::cname##16s cppname##16; \

//------------

// aligned
#define macroVector4TypesStorage(type, name) \
typedef type name##1s; \
typedef __attribute__((__ext_vector_type__(2)))  type name##2s; \
typedef __attribute__((__ext_vector_type__(3)))  type name##3s; \
typedef __attribute__((__ext_vector_type__(4)))  type name##4s; \
typedef __attribute__((__ext_vector_type__(8),__aligned__(16)))  type name##8s; \
typedef __attribute__((__ext_vector_type__(16),__aligned__(16))) type name##16s; \

// packed
#define macroVector4TypesPacked(type, name) \
typedef type name##1p; \
typedef __attribute__((__ext_vector_type__(2),__aligned__(4)))  type name##2p; \
typedef __attribute__((__ext_vector_type__(3),__aligned__(4)))  type name##3p; \
typedef __attribute__((__ext_vector_type__(4),__aligned__(4)))  type name##4p; \
typedef __attribute__((__ext_vector_type__(8),__aligned__(4)))  type name##8p; \
typedef __attribute__((__ext_vector_type__(16),__aligned__(4))) type name##16p; \

// cpp rename for float, u/int
#define macroVector4TypesStorageRenames(cname, cppname) \
typedef ::cname##1s cppname##1; \
typedef ::cname##2s cppname##2; \
typedef ::cname##3s cppname##3; \
typedef ::cname##4s cppname##4; \
typedef ::cname##8s cppname##8; \
typedef ::cname##16s cppname##16; \

//------------

// aligned
#define macroVector8TypesStorage(type, name) \
typedef type name##1s; \
typedef __attribute__((__ext_vector_type__(2))) type name##2s; \
typedef __attribute__((__ext_vector_type__(3),__aligned__(16))) type name##3s; \
typedef __attribute__((__ext_vector_type__(4),__aligned__(16))) type name##4s; \
typedef __attribute__((__ext_vector_type__(8),__aligned__(16))) type name##8s; \

// packed
#define macroVector8TypesPacked(type, name) \
typedef type name##1p; \
typedef __attribute__((__ext_vector_type__(2),__aligned__(8))) type name##2p; \
typedef __attribute__((__ext_vector_type__(3),__aligned__(8))) type name##3p; \
typedef __attribute__((__ext_vector_type__(4),__aligned__(8))) type name##4p; \
typedef __attribute__((__ext_vector_type__(8),__aligned__(8))) type name##8p; \

// cpp rename for double, u/long
#define macroVector8TypesStorageRenames(cname, cppname) \
typedef ::cname##1s cppname##1; \
typedef ::cname##2s cppname##2; \
typedef ::cname##3s cppname##3; \
typedef ::cname##4s cppname##4; \
typedef ::cname##8s cppname##8; \

//-----------------------------------

#define macroMatrixOps(type) \
SIMD_CALL_OP type& operator*=(type& x, const type& y) { x = mul(x, y); return x; } \
SIMD_CALL_OP type& operator+=(type& x, const type& y) { x = add(x, y); return x; } \
SIMD_CALL_OP type& operator-=(type& x, const type& y) { x = sub(x, y); return x; } \
SIMD_CALL bool operator==(const type& x, const type& y) { return equal(x, y); } \
SIMD_CALL bool operator!=(const type& x, const type& y) { return !(x == y); } \
\
SIMD_CALL type operator-(const type& x, const type& y) { return sub(x,y); } \
SIMD_CALL type operator+(const type& x, const type& y) { return add(x,y); } \
SIMD_CALL type operator*(const type& x, const type& y) { return mul(x,y); } \
SIMD_CALL type::column_t operator*(const type::column_t& v, const type& y) { return mul(v,y); } \
SIMD_CALL type::column_t operator*(const type& x, const type::column_t& v) { return mul(x,v); } \





//-----------------------------------

#include <math.h> // for sqrt

#if SIMD_NEON
// neon types and intrinsics, 16B
#include <arm_neon.h>
// This converts sse to neon intrinsics
#include "sse2neon-arm64.h"
#else
// SSE intrinsics up to AVX-512, but depends on -march avx2 -mf16c -fma
#include <immintrin.h>
#endif // SIMD_NEON

// using macros here cuts the ifdefs a lot
#define vec2to4(x) (x).xyyy
#define vec3to4(x) (x).xyzz
#define vec4to2(x) (x).xy
#define vec4to3(x) (x).xyz

// moved vec/matrix ops into secondary headers
#include "int234.h"
#include "half234.h"
#include "float234.h"
#include "double234.h"

//---------------------------

#if SIMD_CHAR

#ifdef __cplusplus
extern "C" {
#endif

// define c vector types
macroVector1TypesStorage(char, char)
macroVector1TypesPacked(char, char)

#if SIMD_RENAME_TO_SIMD_NAMESPACE
macroVector1TypesStorageRenames(char, simd_char)
#endif // SIMD_RENAME_TO_SIMD_NAMESPACE

#ifdef __cplusplus
}

namespace SIMD_NAMESPACE {
#if SIMD_CHAR
macroVector4TypesStorageRenames(char, char)
#endif
}
#endif

#endif // SIMD_CHAR

//------------
#if SIMD_SHORT

#ifdef __cplusplus
extern "C" {
#endif

// define c vector types
macroVector2TypesStorage(short, short)
macroVector2TypesPacked(short, short)

#if SIMD_RENAME_TO_SIMD_NAMESPACE
macroVector2TypesStorageRenames(short, simd_short)
#endif // SIMD_RENAME_TO_SIMD_NAMESPACE

#ifdef __cplusplus
}

namespace SIMD_NAMESPACE {
#if SIMD_CHAR
macroVector2TypesStorageRenames(short, short)
#endif
}
#endif

#endif // SIMD_SHORT

//------------
#if SIMD_LONG

#ifdef __cplusplus
extern "C" {
#endif

// define c vector types
macroVector8TypesStorage(long, long)
macroVector8TypesPacked(long, long)

#if SIMD_RENAME_TO_SIMD_NAMESPACE
macroVector8TypesStorageRenames(long, simd_long)
#endif // SIMD_RENAME_TO_SIMD_NAMESPACE

#ifdef __cplusplus
}

namespace SIMD_NAMESPACE {
macroVector8TypesStorageRenames(long, long)
}
#endif

#endif // SIMD_LONG

//-------------------
#ifdef __cplusplus

namespace SIMD_NAMESPACE {


//-----------------------------------
// conversions
// keeping these here due to ordering issues of header includes

#if SIMD_FLOAT

#if SIMD_INT // && SIMD_FLOAT
SIMD_CALL float2 float2m(int2 x) { return __builtin_convertvector(x, float2); }
SIMD_CALL float3 float3m(int3 x) { return __builtin_convertvector(x, float3); }
SIMD_CALL float4 float4m(int4 x) { return __builtin_convertvector(x, float4); }

SIMD_CALL int2 float2m(float2 x) { return __builtin_convertvector(x, int2); }
SIMD_CALL int3 float3m(float3 x) { return __builtin_convertvector(x, int3); }
SIMD_CALL int4 float4m(float4 x) { return __builtin_convertvector(x, int4); }

#endif

#if SIMD_HALF // && SIMD_FLOAT

// keep this for now, until know if Android has the builtin
//#if SIMD_HALF4_ONLY
//
//half4 half4m(float4 );
//SIMD_CALL half2 half2m(float2 x) { return vec4to2(half4m(vec2to4(x))); }
//SIMD_CALL half3 half3m(float3 x) { return vec4to3(half4m(vec3to4(x))); }
//
//float4 float4m(half4 );
//SIMD_CALL float2 float2m(half2 x) { return vec4to2(float4m(vec2to4(x))); }
//SIMD_CALL float3 float3m(half3 x) { return vec4to3(float4m(vec3to4(x))); }
//
//#else

// this probably goes to correct call, could elim SIMD_HALF4_ONLY
SIMD_CALL float2 float2m(half2 x) { return __builtin_convertvector(x, float2); }
SIMD_CALL float3 float3m(half3 x) { return __builtin_convertvector(x, float3); }
SIMD_CALL float4 float4m(half4 x) { return __builtin_convertvector(x, float4); }

SIMD_CALL half2 half2m(float2 x) { return __builtin_convertvector(x, half2); }
SIMD_CALL half3 half3m(float3 x) { return __builtin_convertvector(x, half3); }
SIMD_CALL half4 half4m(float4 x) { return __builtin_convertvector(x, half4); }
//#endif

#endif

#if SIMD_DOUBLE // && SIMD_FLOAT
SIMD_CALL double2 double2m(float2 x) { return __builtin_convertvector(x, double2); }
SIMD_CALL double3 double3m(float3 x) { return __builtin_convertvector(x, double3); }
SIMD_CALL double4 double4m(float4 x) { return __builtin_convertvector(x, double4); }

SIMD_CALL float2 float2m(double2 x) { return __builtin_convertvector(x, float2); }
SIMD_CALL float3 float3m(double3 x) { return __builtin_convertvector(x, float3); }
SIMD_CALL float4 float4m(double4 x) { return __builtin_convertvector(x, float4); }
#endif

#endif // SIMD_FLOAT

//---------------------------

using namespace STL_NAMESPACE;

// Usage:
// vecf vfmt(fmtToken);
// fprintf(stdout, "%s", vfmt.str(v1).c_str() );
// This may seem extreme to pass string, but it has SSO and keeps temp alive to printf.
struct vecf {
    // TODO: add formatting options too
    vecf() {
    }
   
#if SIMD_FLOAT
    // vector
    string str(float2 v) const;
    string str(float3 v) const;
    string str(float4 v) const;
    
    // matrix
    string str(const float2x2& m) const;
    string str(const float3x3& m) const;
    string str(const float4x4& m) const;
    
    string quat(quatf q) { return str(q.v); }
    
#endif // SIMD_FLOAT
    
    // Just stuffing this here for now
    string simd_configs() const;
    string simd_alignments() const;
    
    // TODO: add double, int, half printing
};

} // namespace SIMD_NAMESPACE

#endif // __cplusplus

#endif

