/*
 * Copyright 2018 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef WASM_RT_NO_SANDBOX_DECLARATIONS_H_
#define WASM_RT_NO_SANDBOX_DECLARATIONS_H_

#define FUNC_PROLOGUE
#define FUNC_EPILOGUE

#if WABT_BIG_ENDIAN
#error "Big endian not supported in --disable-sandbox mode."
#endif

static inline int wasm_rt_trap() {
    __builtin_abort();
    return 0;
}

#define TRAP(x) wasm_rt_trap()
#define UNREACHABLE (void)wasm_rt_trap()

#define DEFINE_LOAD(name, t1, t2, t3)                       \
  static inline t3 name(u64 addr) {                         \
    t1 result;                                              \
    __builtin_memcpy(&result, (const void*)addr, sizeof(t1)); \
    __asm__("" ::"r"(result));                             \
    return (t3)(t2)result;                                  \
  }

#define DEFINE_STORE(name, t1, t2)                     \
  static inline void name(u64 addr, t2 value) {        \
    t1 wrapped = (t1)value;                            \
    __builtin_memcpy((void*)addr, &wrapped, sizeof(t1)); \
  }

DEFINE_LOAD(i32_load, u32, u32, u32)
DEFINE_LOAD(i64_load, u64, u64, u64)
DEFINE_LOAD(f32_load, f32, f32, f32)
DEFINE_LOAD(f64_load, f64, f64, f64)
DEFINE_LOAD(i32_load8_s, s8, s32, u32)
DEFINE_LOAD(i64_load8_s, s8, s64, u64)
DEFINE_LOAD(i32_load8_u, u8, u32, u32)
DEFINE_LOAD(i64_load8_u, u8, u64, u64)
DEFINE_LOAD(i32_load16_s, s16, s32, u32)
DEFINE_LOAD(i64_load16_s, s16, s64, u64)
DEFINE_LOAD(i32_load16_u, u16, u32, u32)
DEFINE_LOAD(i64_load16_u, u16, u64, u64)
DEFINE_LOAD(i64_load32_s, s32, s64, u64)
DEFINE_LOAD(i64_load32_u, u32, u64, u64)
DEFINE_STORE(i32_store, u32, u32)
DEFINE_STORE(i64_store, u64, u64)
DEFINE_STORE(f32_store, f32, f32)
DEFINE_STORE(f64_store, f64, f64)
DEFINE_STORE(i32_store8, u8, u32)
DEFINE_STORE(i32_store16, u16, u32)
DEFINE_STORE(i64_store8, u8, u64)
DEFINE_STORE(i64_store16, u16, u64)
DEFINE_STORE(i64_store32, u32, u64)

#if defined(WASM_RT_ENABLE_SIMD)
#error "SIMD not yet supported with --disable-sandbox."
#endif

#define I32_CLZ(x) ((x) ? __builtin_clz(x) : 32)
#define I64_CLZ(x) ((x) ? __builtin_clzll(x) : 64)
#define I32_CTZ(x) ((x) ? __builtin_ctz(x) : 32)
#define I64_CTZ(x) ((x) ? __builtin_ctzll(x) : 64)
#define I32_POPCNT(x) (__builtin_popcount(x))
#define I64_POPCNT(x) (__builtin_popcountll(x))

// Unchecked variants of division and remainder.
#define DIV_S(ut, min, x, y) ((ut)((x) / (y)))
#define REM_S(ut, min, x, y) ((ut)((x) % (y)))
#define DIVREM_U(op, x, y) ((x)op(y))

#define I32_DIV_S(x, y) DIV_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_DIV_S(x, y) DIV_S(u64, INT64_MIN, (s64)x, (s64)y)
#define I32_REM_S(x, y) REM_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_REM_S(x, y) REM_S(u64, INT64_MIN, (s64)x, (s64)y)

#define DIV_U(x, y) DIVREM_U(/, x, y)
#define REM_U(x, y) DIVREM_U(%, x, y)

#define ROTL(x, y, mask) \
  (((x) << ((y) & (mask))) | ((x) >> (((mask) - (y) + 1) & (mask))))
#define ROTR(x, y, mask) \
  (((x) >> ((y) & (mask))) | ((x) << (((mask) - (y) + 1) & (mask))))

#define I32_ROTL(x, y) ROTL(x, y, 31)
#define I64_ROTL(x, y) ROTL(x, y, 63)
#define I32_ROTR(x, y) ROTR(x, y, 31)
#define I64_ROTR(x, y) ROTR(x, y, 63)

// TODO: floating point

extern u32 open(u64, u32, ...);

inline static u32 wasm_open2(u64 p, u32 f) {
  return open(p, f);
}

inline static u32 wasm_open3(u64 p, u32 f, u32 m) {
  return open(p, f, m);
}


#endif /* WASM_RT_NO_SANDBOX_DECLARATIONS_H_ */
