// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exprs/aggregate-functions.h"

#include <math.h>
#include <sstream>
#include <algorithm>

#include <boost/random/ranlux.hpp>
#include <boost/random/uniform_int.hpp>

#include "common/logging.h"
#include "runtime/decimal-value.h"
#include "runtime/string-value.h"
#include "runtime/timestamp-value.h"
#include "exprs/anyval-util.h"

using namespace std;
using namespace boost;
using namespace boost::random;

// TODO: this file should be cross compiled and then all of the builtin
// aggregate functions will have a codegen enabled path. Then we can remove
// the custom code in aggregation node.
namespace impala {

// Converts any UDF Val Type to a string representation
template <typename T>
StringVal ToStringVal(FunctionContext* context, T val) {
  stringstream ss;
  ss << val;
  const string &str = ss.str();
  StringVal string_val(context, str.size());
  memcpy(string_val.ptr, str.c_str(), str.size());
  return string_val;
}

// Delimiter to use if the separator is NULL.
static const StringVal DEFAULT_STRING_CONCAT_DELIM((uint8_t*)", ", 2);

// Hyperloglog precision. Default taken from paper. Doesn't seem to matter very
// much when between [6,12]
const int HLL_PRECISION = 10;
const int HLL_LEN = 1024; // 2^HLL_PRECISION

void AggregateFunctions::InitNull(FunctionContext*, AnyVal* dst) {
  dst->is_null = true;
}

template<typename T>
void AggregateFunctions::InitZero(FunctionContext*, T* dst) {
  dst->is_null = false;
  dst->val = 0;
}

template<>
void AggregateFunctions::InitZero(FunctionContext*, DecimalVal* dst) {
  dst->is_null = false;
  dst->val16 = 0;
}

StringVal AggregateFunctions::StringValGetValue(
    FunctionContext* ctx, const StringVal& src) {
  if (src.is_null) return src;
  StringVal result(ctx, src.len);
  memcpy(result.ptr, src.ptr, src.len);
  return result;
}

StringVal AggregateFunctions::StringValSerializeOrFinalize(
    FunctionContext* ctx, const StringVal& src) {
  StringVal result = StringValGetValue(ctx, src);
  ctx->Free(src.ptr);
  return result;
}

void AggregateFunctions::CountUpdate(
    FunctionContext*, const AnyVal& src, BigIntVal* dst) {
  DCHECK(!dst->is_null);
  if (!src.is_null) ++dst->val;
}

void AggregateFunctions::CountStarUpdate(FunctionContext*, BigIntVal* dst) {
  DCHECK(!dst->is_null);
  ++dst->val;
}

void AggregateFunctions::CountMerge(FunctionContext*, const BigIntVal& src,
    BigIntVal* dst) {
  DCHECK(!dst->is_null);
  DCHECK(!src.is_null);
  dst->val += src.val;
}

struct AvgState {
  double sum;
  int64_t count;
};

void AggregateFunctions::AvgInit(FunctionContext* ctx, StringVal* dst) {
  dst->is_null = false;
  dst->len = sizeof(AvgState);
  dst->ptr = ctx->Allocate(dst->len);
  memset(dst->ptr, 0, sizeof(AvgState));
}

template <typename T>
void AggregateFunctions::AvgUpdate(FunctionContext* ctx, const T& src,
    StringVal* dst) {
  if (src.is_null) return;
  DCHECK(dst->ptr != NULL);
  DCHECK_EQ(sizeof(AvgState), dst->len);
  AvgState* avg = reinterpret_cast<AvgState*>(dst->ptr);
  avg->sum += src.val;
  ++avg->count;
}

void AggregateFunctions::AvgMerge(FunctionContext* ctx, const StringVal& src,
    StringVal* dst) {
  const AvgState* src_struct = reinterpret_cast<const AvgState*>(src.ptr);
  DCHECK(dst->ptr != NULL);
  DCHECK_EQ(sizeof(AvgState), dst->len);
  AvgState* dst_struct = reinterpret_cast<AvgState*>(dst->ptr);
  dst_struct->sum += src_struct->sum;
  dst_struct->count += src_struct->count;
}

DoubleVal AggregateFunctions::AvgGetValue(FunctionContext* ctx, const StringVal& src) {
  AvgState* val_struct = reinterpret_cast<AvgState*>(src.ptr);
  if (val_struct->count == 0) return DoubleVal::null();
  return DoubleVal(val_struct->sum / val_struct->count);
}

DoubleVal AggregateFunctions::AvgFinalize(FunctionContext* ctx, const StringVal& src) {
  DoubleVal result = AvgGetValue(ctx, src);
  ctx->Free(src.ptr);
  return result;
}

void AggregateFunctions::TimestampAvgUpdate(FunctionContext* ctx,
    const TimestampVal& src, StringVal* dst) {
  if (src.is_null) return;
  DCHECK(dst->ptr != NULL);
  DCHECK_EQ(sizeof(AvgState), dst->len);
  AvgState* avg = reinterpret_cast<AvgState*>(dst->ptr);
  double val = TimestampValue::FromTimestampVal(src);
  avg->sum += val;
  ++avg->count;
}

TimestampVal AggregateFunctions::TimestampAvgGetValue(FunctionContext* ctx,
    const StringVal& src) {
  AvgState* val_struct = reinterpret_cast<AvgState*>(src.ptr);
  if (val_struct->count == 0) return TimestampVal::null();
  TimestampValue tv(val_struct->sum / val_struct->count);
  TimestampVal result;
  tv.ToTimestampVal(&result);
  return result;
}

TimestampVal AggregateFunctions::TimestampAvgFinalize(FunctionContext* ctx,
    const StringVal& src) {
  TimestampVal result = TimestampAvgGetValue(ctx, src);
  ctx->Free(src.ptr);
  return result;
}

struct DecimalAvgState {
  DecimalVal sum; // only using val16
  int64_t count;
};

void AggregateFunctions::DecimalAvgInit(FunctionContext* ctx, StringVal* dst) {
  dst->is_null = false;
  dst->len = sizeof(DecimalAvgState);
  dst->ptr = ctx->Allocate(dst->len);
  memset(dst->ptr, 0, sizeof(DecimalAvgState));
}

void AggregateFunctions::DecimalAvgUpdate(FunctionContext* ctx, const DecimalVal& src,
    StringVal* dst) {
  if (src.is_null) return;
  DCHECK(dst->ptr != NULL);
  DCHECK_EQ(sizeof(DecimalAvgState), dst->len);
  DecimalAvgState* avg = reinterpret_cast<DecimalAvgState*>(dst->ptr);
  const FunctionContext::TypeDesc* arg_desc = ctx->GetArgType(0);
  DCHECK(arg_desc != NULL);
  const ColumnType& arg_type = AnyValUtil::TypeDescToColumnType(*arg_desc);

  // Since the src and dst are guaranteed to be the same scale, we can just
  // do a simple add.
  switch (arg_type.GetByteSize()) {
    case 4:
      avg->sum.val16 += src.val4;
      break;
    case 8:
      avg->sum.val16 += src.val8;
      break;
    case 16:
      avg->sum.val16 += src.val4;
      break;
    default:
      DCHECK(false) << "Invalid byte size for type " << arg_type.DebugString();
  }
  ++avg->count;
}

void AggregateFunctions::DecimalAvgMerge(FunctionContext* ctx,
    const StringVal& src, StringVal* dst) {
  const DecimalAvgState* src_struct =
      reinterpret_cast<const DecimalAvgState*>(src.ptr);
  DCHECK(dst->ptr != NULL);
  DCHECK_EQ(sizeof(DecimalAvgState), dst->len);
  DecimalAvgState* dst_struct = reinterpret_cast<DecimalAvgState*>(dst->ptr);
  dst_struct->sum.val16 += src_struct->sum.val16;
  dst_struct->count += src_struct->count;
}

DecimalVal AggregateFunctions::DecimalAvgGetValue(FunctionContext* ctx,
    const StringVal& src) {
  DecimalAvgState* val_struct = reinterpret_cast<DecimalAvgState*>(src.ptr);
  if (val_struct->count == 0) return DecimalVal::null();
  const FunctionContext::TypeDesc& output_desc = ctx->GetReturnType();
  DCHECK_EQ(FunctionContext::TYPE_DECIMAL, output_desc.type);
  Decimal16Value sum(val_struct->sum.val16);
  Decimal16Value count(val_struct->count);
  // The scale of the accumulated sum must be the same as the scale of the return type.
  // TODO: Investigate whether this is always the right thing to do. Does the current
  // implementation result in an unacceptable loss of output precision?
  ColumnType sum_type = ColumnType::CreateDecimalType(38, output_desc.scale);
  ColumnType count_type = ColumnType::CreateDecimalType(38, 0);
  bool is_nan, overflow;
  Decimal16Value result = sum.Divide<int128_t>(sum_type, count, count_type,
      output_desc.scale, &is_nan, &overflow);
  if (UNLIKELY(is_nan)) return DecimalVal::null();
  if (UNLIKELY(overflow)) {
    ctx->AddWarning("Avg computation overflowed, returning NULL");
    return DecimalVal::null();
  }
  return DecimalVal(result.value());
}

DecimalVal AggregateFunctions::DecimalAvgFinalize(FunctionContext* ctx,
    const StringVal& src) {
  DecimalVal result = DecimalAvgGetValue(ctx, src);
  ctx->Free(src.ptr);
  return result;
}

template<typename SRC_VAL, typename DST_VAL>
void AggregateFunctions::Sum(FunctionContext* ctx, const SRC_VAL& src, DST_VAL* dst) {
  if (src.is_null) return;
  if (dst->is_null) InitZero<DST_VAL>(ctx, dst);
  dst->val += src.val;
}

void AggregateFunctions::SumUpdate(FunctionContext* ctx,
    const DecimalVal& src, DecimalVal* dst) {
  if (src.is_null) return;
  if (dst->is_null) InitZero<DecimalVal>(ctx, dst);
  const FunctionContext::TypeDesc* arg_desc = ctx->GetArgType(0);
  // Since the src and dst are guaranteed to be the same scale, we can just
  // do a simple add.
  if (arg_desc->precision <= 9) {
    dst->val16 += src.val4;
  } else if (arg_desc->precision <= 19) {
    dst->val16 += src.val8;
  } else {
    dst->val16 += src.val16;
  }
}

void AggregateFunctions::SumMerge(FunctionContext* ctx,
    const DecimalVal& src, DecimalVal* dst) {
  if (src.is_null) return;
  if (dst->is_null) InitZero<DecimalVal>(ctx, dst);
  dst->val16 += src.val16;
}

template<typename T>
void AggregateFunctions::Min(FunctionContext*, const T& src, T* dst) {
  if (src.is_null) return;
  if (dst->is_null || src.val < dst->val) *dst = src;
}

template<typename T>
void AggregateFunctions::Max(FunctionContext*, const T& src, T* dst) {
  if (src.is_null) return;
  if (dst->is_null || src.val > dst->val) *dst = src;
}

void AggregateFunctions::InitNullString(FunctionContext* c, StringVal* dst) {
  dst->is_null = true;
  dst->ptr = NULL;
  dst->len = 0;
}

template<>
void AggregateFunctions::Min(FunctionContext* ctx, const StringVal& src, StringVal* dst) {
  if (src.is_null) return;
  if (dst->is_null ||
      StringValue::FromStringVal(src) < StringValue::FromStringVal(*dst)) {
    if (!dst->is_null) ctx->Free(dst->ptr);
    uint8_t* copy = ctx->Allocate(src.len);
    memcpy(copy, src.ptr, src.len);
    *dst = StringVal(copy, src.len);
  }
}

template<>
void AggregateFunctions::Max(FunctionContext* ctx, const StringVal& src, StringVal* dst) {
  if (src.is_null) return;
  if (dst->is_null ||
      StringValue::FromStringVal(src) > StringValue::FromStringVal(*dst)) {
    if (!dst->is_null) ctx->Free(dst->ptr);
    uint8_t* copy = ctx->Allocate(src.len);
    memcpy(copy, src.ptr, src.len);
    *dst = StringVal(copy, src.len);
  }
}

template<>
void AggregateFunctions::Min(FunctionContext* ctx,
    const DecimalVal& src, DecimalVal* dst) {
  if (src.is_null) return;
  const FunctionContext::TypeDesc* arg = ctx->GetArgType(0);
  DCHECK(arg != NULL);
  if (arg->precision <= 9) {
    if (dst->is_null || src.val4 < dst->val4) *dst = src;
  } else if (arg->precision <= 19) {
    if (dst->is_null || src.val8 < dst->val8) *dst = src;
  } else {
    if (dst->is_null || src.val16 < dst->val16) *dst = src;
  }
}

template<>
void AggregateFunctions::Max(FunctionContext* ctx,
    const DecimalVal& src, DecimalVal* dst) {
  if (src.is_null) return;
  const FunctionContext::TypeDesc* arg = ctx->GetArgType(0);
  DCHECK(arg != NULL);
  if (arg->precision <= 9) {
    if (dst->is_null || src.val4 > dst->val4) *dst = src;
  } else if (arg->precision <= 19) {
    if (dst->is_null || src.val8 > dst->val8) *dst = src;
  } else {
    if (dst->is_null || src.val16 > dst->val16) *dst = src;
  }
}

template<>
void AggregateFunctions::Min(FunctionContext*,
    const TimestampVal& src, TimestampVal* dst) {
  if (src.is_null) return;
  if (dst->is_null) {
    *dst = src;
    return;
  }
  TimestampValue src_tv = TimestampValue::FromTimestampVal(src);
  TimestampValue dst_tv = TimestampValue::FromTimestampVal(*dst);
  if (src_tv < dst_tv) *dst = src;
}

template<>
void AggregateFunctions::Max(FunctionContext*,
    const TimestampVal& src, TimestampVal* dst) {
  if (src.is_null) return;
  if (dst->is_null) {
    *dst = src;
    return;
  }
  TimestampValue src_tv = TimestampValue::FromTimestampVal(src);
  TimestampValue dst_tv = TimestampValue::FromTimestampVal(*dst);
  if (src_tv > dst_tv) *dst = src;
}

// StringConcat intermediate state starts with the length of the first
// separator, followed by the accumulated string.  The accumulated
// string starts with the separator of the first value that arrived in
// StringConcatUpdate().
typedef int StringConcatHeader;

void AggregateFunctions::StringConcatUpdate(FunctionContext* ctx,
    const StringVal& src, StringVal* result) {
  StringConcatUpdate(ctx, src, DEFAULT_STRING_CONCAT_DELIM, result);
}

void AggregateFunctions::StringConcatUpdate(FunctionContext* ctx,
    const StringVal& src, const StringVal& separator, StringVal* result) {
  if (src.is_null) return;
  const StringVal* sep = separator.is_null ? &DEFAULT_STRING_CONCAT_DELIM : &separator;
  if (result->is_null) {
    // Header of the intermediate state holds the length of the first separator.
    const int header_len = sizeof(StringConcatHeader);
    DCHECK(header_len == sizeof(sep->len));
    *result = StringVal(ctx->Allocate(header_len), header_len);
    *reinterpret_cast<StringConcatHeader*>(result->ptr) = sep->len;
  }
  int new_len = result->len + sep->len + src.len;
  result->ptr = ctx->Reallocate(result->ptr, new_len);
  memcpy(result->ptr + result->len, sep->ptr, sep->len);
  result->len += sep->len;
  memcpy(result->ptr + result->len, src.ptr, src.len);
  result->len += src.len;
  DCHECK(result->len == new_len);
}

void AggregateFunctions::StringConcatMerge(FunctionContext* ctx,
    const StringVal& src, StringVal* result) {
  if (src.is_null) return;
  const int header_len = sizeof(StringConcatHeader);
  if (result->is_null) {
    // Copy the header from the first intermediate value.
    *result = StringVal(ctx->Allocate(header_len), header_len);
    *reinterpret_cast<StringConcatHeader*>(result->ptr) =
        *reinterpret_cast<StringConcatHeader*>(src.ptr);
  }
  // Append the string portion of the intermediate src to result (omit src's header).
  int new_len = result->len + src.len - header_len;
  result->ptr = ctx->Reallocate(result->ptr, new_len);
  memcpy(result->ptr + result->len, src.ptr + header_len, src.len - header_len);
  result->len += src.len - header_len;
  DCHECK(result->len == new_len);
}

StringVal AggregateFunctions::StringConcatFinalize(FunctionContext* ctx,
    const StringVal& src) {
  if (src.is_null) return src;
  const int header_len = sizeof(StringConcatHeader);
  DCHECK(src.len >= header_len);
  int sep_len = *reinterpret_cast<StringConcatHeader*>(src.ptr);
  DCHECK(src.len >= header_len + sep_len);
  // Remove the header and the first separator.
  StringVal result(ctx, src.len - header_len - sep_len);
  memcpy(result.ptr, src.ptr + header_len + sep_len, result.len);
  ctx->Free(src.ptr);
  return result;
}

// Compute distinctpc and distinctpcsa using Flajolet and Martin's algorithm
// (Probabilistic Counting Algorithms for Data Base Applications)
// We have implemented two variants here: one with stochastic averaging (with PCSA
// postfix) and one without.
// There are 4 phases to compute the aggregate:
//   1. allocate a bitmap, stored in the aggregation tuple's output string slot
//   2. update the bitmap per row (UpdateDistinctEstimateSlot)
//   3. for distributed plan, merge the bitmaps from all the nodes
//      (UpdateMergeEstimateSlot)
//   4. compute the estimate using the bitmaps when all the rows are processed
//      (FinalizeEstimateSlot)
const static int NUM_PC_BITMAPS = 64; // number of bitmaps
const static int PC_BITMAP_LENGTH = 32; // the length of each bit map
const static float PC_THETA = 0.77351f; // the magic number to compute the final result

void AggregateFunctions::PcInit(FunctionContext* c, StringVal* dst) {
  // Initialize the distinct estimate bit map - Probabilistic Counting Algorithms for Data
  // Base Applications (Flajolet and Martin)
  //
  // The bitmap is a 64bit(1st index) x 32bit(2nd index) matrix.
  // So, the string length of 256 byte is enough.
  // The layout is:
  //   row  1: 8bit 8bit 8bit 8bit
  //   row  2: 8bit 8bit 8bit 8bit
  //   ...     ..
  //   ...     ..
  //   row 64: 8bit 8bit 8bit 8bit
  //
  // Using 32bit length, we can count up to 10^8. This will not be enough for Fact table
  // primary key, but once we approach the limit, we could interpret the result as
  // "every row is distinct".
  //
  // We use "string" type for DISTINCT_PC function so that we can use the string
  // slot to hold the bitmaps.
  dst->is_null = false;
  int str_len = NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8;
  dst->ptr = c->Allocate(str_len);
  dst->len = str_len;
  memset(dst->ptr, 0, str_len);
}

static inline void SetDistinctEstimateBit(uint8_t* bitmap,
    uint32_t row_index, uint32_t bit_index) {
  // We need to convert Bitmap[alpha,index] into the index of the string.
  // alpha tells which of the 32bit we've to jump to.
  // index then lead us to the byte and bit.
  uint32_t *int_bitmap = reinterpret_cast<uint32_t*>(bitmap);
  int_bitmap[row_index] |= (1 << bit_index);
}

static inline bool GetDistinctEstimateBit(uint8_t* bitmap,
    uint32_t row_index, uint32_t bit_index) {
  uint32_t *int_bitmap = reinterpret_cast<uint32_t*>(bitmap);
  return ((int_bitmap[row_index] & (1 << bit_index)) > 0);
}

template<typename T>
void AggregateFunctions::PcUpdate(FunctionContext* c, const T& input, StringVal* dst) {
  if (input.is_null) return;
  // Core of the algorithm. This is a direct translation of the code in the paper.
  // Please see the paper for details. For simple averaging, we need to compute hash
  // values NUM_PC_BITMAPS times using NUM_PC_BITMAPS different hash functions (by using a
  // different seed).
  for (int i = 0; i < NUM_PC_BITMAPS; ++i) {
    uint32_t hash_value = AnyValUtil::Hash(input, *c->GetArgType(0), i);
    int bit_index = __builtin_ctz(hash_value);
    if (UNLIKELY(hash_value == 0)) bit_index = PC_BITMAP_LENGTH - 1;
    // Set bitmap[i, bit_index] to 1
    SetDistinctEstimateBit(dst->ptr, i, bit_index);
  }
}

template<typename T>
void AggregateFunctions::PcsaUpdate(FunctionContext* c, const T& input, StringVal* dst) {
  if (input.is_null) return;

  // Core of the algorithm. This is a direct translation of the code in the paper.
  // Please see the paper for details. Using stochastic averaging, we only need to
  // the hash value once for each row.
  uint32_t hash_value = AnyValUtil::Hash(input, *c->GetArgType(0), 0);
  uint32_t row_index = hash_value % NUM_PC_BITMAPS;

  // We want the zero-based position of the least significant 1-bit in binary
  // representation of hash_value. __builtin_ctz does exactly this because it returns
  // the number of trailing 0-bits in x (or undefined if x is zero).
  int bit_index = __builtin_ctz(hash_value / NUM_PC_BITMAPS);
  if (UNLIKELY(hash_value == 0)) bit_index = PC_BITMAP_LENGTH - 1;

  // Set bitmap[row_index, bit_index] to 1
  SetDistinctEstimateBit(dst->ptr, row_index, bit_index);
}

string DistinctEstimateBitMapToString(uint8_t* v) {
  stringstream debugstr;
  for (int i = 0; i < NUM_PC_BITMAPS; ++i) {
    for (int j = 0; j < PC_BITMAP_LENGTH; ++j) {
      // print bitmap[i][j]
      debugstr << GetDistinctEstimateBit(v, i, j);
    }
    debugstr << "\n";
  }
  debugstr << "\n";
  return debugstr.str();
}

void AggregateFunctions::PcMerge(FunctionContext* c,
    const StringVal& src, StringVal* dst) {
  DCHECK(!src.is_null);
  DCHECK(!dst->is_null);
  DCHECK_EQ(src.len, NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8);

  // Merge the bits
  // I think _mm_or_ps can do it, but perf doesn't really matter here. We call this only
  // once group per node.
  for (int i = 0; i < NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8; ++i) {
    *(dst->ptr + i) |= *(src.ptr + i);
  }

  VLOG_ROW << "UpdateMergeEstimateSlot Src Bit map:\n"
           << DistinctEstimateBitMapToString(src.ptr);
  VLOG_ROW << "UpdateMergeEstimateSlot Dst Bit map:\n"
           << DistinctEstimateBitMapToString(dst->ptr);
}

double DistinceEstimateFinalize(const StringVal& src) {
  DCHECK(!src.is_null);
  DCHECK_EQ(src.len, NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8);
  VLOG_ROW << "FinalizeEstimateSlot Bit map:\n"
           << DistinctEstimateBitMapToString(src.ptr);

  // We haven't processed any rows if none of the bits are set. Therefore, we have zero
  // distinct rows. We're overwriting the result in the same string buffer we've
  // allocated.
  bool is_empty = true;
  for (int i = 0; i < NUM_PC_BITMAPS * PC_BITMAP_LENGTH / 8; ++i) {
    if (src.ptr[i] != 0) {
      is_empty = false;
      break;
    }
  }
  if (is_empty) return 0;

  // Convert the bitmap to a number, please see the paper for details
  // In short, we count the average number of leading 1s (per row) in the bit map.
  // The number is proportional to the log2(1/NUM_PC_BITMAPS of  the actual number of
  // distinct).
  // To get the actual number of distinct, we'll do 2^avg / PC_THETA.
  // PC_THETA is a magic number.
  int sum = 0;
  for (int i = 0; i < NUM_PC_BITMAPS; ++i) {
    int row_bit_count = 0;
    // Count the number of leading ones for each row in the bitmap
    // We could have used the build in __builtin_clz to count of number of leading zeros
    // but we first need to invert the 1 and 0.
    while (GetDistinctEstimateBit(src.ptr, i, row_bit_count) &&
        row_bit_count < PC_BITMAP_LENGTH) {
      ++row_bit_count;
    }
    sum += row_bit_count;
  }
  double avg = static_cast<double>(sum) / static_cast<double>(NUM_PC_BITMAPS);
  double result = pow(static_cast<double>(2), avg) / PC_THETA;
  return result;
}

StringVal AggregateFunctions::PcFinalize(FunctionContext* c, const StringVal& src) {
  DCHECK(!src.is_null);
  double estimate = DistinceEstimateFinalize(src);
  int64_t result = estimate;
  c->Free(src.ptr);

  // TODO: this should return bigint. this is a hack
  stringstream ss;
  ss << result;
  string str = ss.str();
  StringVal dst(c, str.length());
  memcpy(dst.ptr, str.c_str(), str.length());
  return dst;
}

StringVal AggregateFunctions::PcsaFinalize(FunctionContext* c, const StringVal& src) {
  DCHECK(!src.is_null);
  // When using stochastic averaging, the result has to be multiplied by NUM_PC_BITMAPS.
  double estimate = DistinceEstimateFinalize(src) * NUM_PC_BITMAPS;
  int64_t result = estimate;
  c->Free(src.ptr);

  // TODO: this should return bigint. this is a hack
  stringstream ss;
  ss << result;
  string str = ss.str();
  StringVal dst(c, str.length());
  memcpy(dst.ptr, str.c_str(), str.length());
  return dst;
}

// Histogram constants
// TODO: Expose as constant argument parameters to the UDA.
const static int NUM_BUCKETS = 100;
const static int NUM_SAMPLES_PER_BUCKET = 200;
const static int NUM_SAMPLES = NUM_BUCKETS * NUM_SAMPLES_PER_BUCKET;
const static int MAX_STRING_SAMPLE_LEN = 10;

template <typename T>
struct ReservoirSample {
  // Sample value
  T val;
  // Key on which the samples are sorted.
  double key;

  ReservoirSample() : key(-1) { }
  ReservoirSample(const T& val) : val(val), key(-1) { }
};

// Template specialization for StringVal because we do not store the StringVal itself.
// Instead, we keep fixed size arrays and truncate longer strings if necessary.
template <>
struct ReservoirSample<StringVal> {
  uint8_t val[MAX_STRING_SAMPLE_LEN];
  int len; // Size of string (up to MAX_STRING_SAMPLE_LEN)
  double key;

  ReservoirSample() : len(0), key(-1) { }

  ReservoirSample(const StringVal& string_val) : key(-1) {
    len = min(string_val.len, MAX_STRING_SAMPLE_LEN);
    memcpy(&val[0], string_val.ptr, len);
  }
};

template <typename T>
struct ReservoirSampleState {
  ReservoirSample<T> samples[NUM_SAMPLES];

  // Number of collected samples.
  int num_samples;

  // Number of values over which the samples were collected.
  int64_t source_size;

  // Random number generator for generating 64-bit integers
  // TODO: Replace with mt19937_64 when upgrading boost
  ranlux64_3 rng;

  int64_t GetNext64(int64_t max) {
    uniform_int<int64_t> dist(0, max);
    return dist(rng);
  }
};

template <typename T>
void AggregateFunctions::ReservoirSampleInit(FunctionContext* ctx, StringVal* dst) {
  int str_len = sizeof(ReservoirSampleState<T>);
  dst->is_null = false;
  dst->ptr = ctx->Allocate(str_len);
  dst->len = str_len;
  memset(dst->ptr, 0, str_len);
  *reinterpret_cast<ReservoirSampleState<T>*>(dst->ptr) = ReservoirSampleState<T>();
}

template <typename T>
void AggregateFunctions::ReservoirSampleUpdate(FunctionContext* ctx, const T& src,
    StringVal* dst) {
  if (src.is_null) return;
  DCHECK(!dst->is_null);
  DCHECK_EQ(dst->len, sizeof(ReservoirSampleState<T>));
  ReservoirSampleState<T>* state = reinterpret_cast<ReservoirSampleState<T>*>(dst->ptr);

  if (state->num_samples < NUM_SAMPLES) {
    state->samples[state->num_samples++] = ReservoirSample<T>(src);
  } else {
    int64_t r = state->GetNext64(state->source_size);
    if (r < NUM_SAMPLES) state->samples[r] = ReservoirSample<T>(src);
  }
  ++state->source_size;
}

template <typename T>
const StringVal AggregateFunctions::ReservoirSampleSerialize(FunctionContext* ctx,
    const StringVal& src) {
  if (src.is_null) return src;
  StringVal result(ctx, src.len);
  memcpy(result.ptr, src.ptr, src.len);
  ctx->Free(src.ptr);

  ReservoirSampleState<T>* state = reinterpret_cast<ReservoirSampleState<T>*>(result.ptr);
  // Assign keys to the samples that haven't been set (i.e. if serializing after
  // Update()). In weighted reservoir sampling the keys are typically assigned as the
  // sources are being sampled, but this requires maintaining the samples in sorted order
  // (by key) and it accomplishes the same thing at this point because all data points
  // coming into Update() get the same weight. When the samples are later merged, they do
  // have different weights (set here) that are proportional to the source_size, i.e.
  // samples selected from a larger stream are more likely to end up in the final sample
  // set. In order to avoid the extra overhead in Update(), we approximate the keys by
  // picking random numbers in the range [(SOURCE_SIZE - SAMPLE_SIZE)/(SOURCE_SIZE), 1].
  // This weights the keys by SOURCE_SIZE and implies that the samples picked had the
  // highest keys, because values not sampled would have keys between 0 and
  // (SOURCE_SIZE - SAMPLE_SIZE)/(SOURCE_SIZE).
  for (int i = 0; i < state->num_samples; ++i) {
    if (state->samples[i].key >= 0) continue;
    int r = rand() % state->num_samples;
    state->samples[i].key = ((double) state->source_size - r) / state->source_size;
  }
  return result;
}

template <typename T>
bool SampleValLess(const ReservoirSample<T>& i, const ReservoirSample<T>& j) {
  return i.val.val < j.val.val;
}

template <>
bool SampleValLess(const ReservoirSample<StringVal>& i,
    const ReservoirSample<StringVal>& j) {
  int n = min(i.len, j.len);
  int result = memcmp(&i.val[0], &j.val[0], n);
  if (result == 0) return i.len < j.len;
  return result < 0;
}

template <>
bool SampleValLess(const ReservoirSample<DecimalVal>& i,
    const ReservoirSample<DecimalVal>& j) {
  return i.val.val16 < j.val.val16;
}

template <>
bool SampleValLess(const ReservoirSample<TimestampVal>& i,
    const ReservoirSample<TimestampVal>& j) {
  if (i.val.date == j.val.date) return i.val.time_of_day < j.val.time_of_day;
  else return i.val.date < j.val.date;
}

template <typename T>
bool SampleKeyGreater(const ReservoirSample<T>& i, const ReservoirSample<T>& j) {
  return i.key > j.key;
}

template <typename T>
void AggregateFunctions::ReservoirSampleMerge(FunctionContext* ctx,
    const StringVal& src_val, StringVal* dst_val) {
  if (src_val.is_null) return;
  DCHECK(!dst_val->is_null);
  DCHECK(!src_val.is_null);
  DCHECK_EQ(src_val.len, sizeof(ReservoirSampleState<T>));
  DCHECK_EQ(dst_val->len, sizeof(ReservoirSampleState<T>));
  ReservoirSampleState<T>* src = reinterpret_cast<ReservoirSampleState<T>*>(src_val.ptr);
  ReservoirSampleState<T>* dst = reinterpret_cast<ReservoirSampleState<T>*>(dst_val->ptr);

  int src_idx = 0;
  int src_max = src->num_samples;
  // First, fill up the dst samples if they don't already exist. The samples are now
  // ordered as a min-heap on the key.
  while (dst->num_samples < NUM_SAMPLES && src_idx < src_max) {
    DCHECK_GE(src->samples[src_idx].key, 0);
    dst->samples[dst->num_samples++] = src->samples[src_idx++];
    push_heap(dst->samples, dst->samples + dst->num_samples, SampleKeyGreater<T>);
  }
  // Then for every sample from source, take the sample if the key is greater than
  // the minimum key in the min-heap.
  while (src_idx < src_max) {
    DCHECK_GE(src->samples[src_idx].key, 0);
    if (src->samples[src_idx].key > dst->samples[0].key) {
      pop_heap(dst->samples, dst->samples + NUM_SAMPLES, SampleKeyGreater<T>);
      dst->samples[NUM_SAMPLES - 1] = src->samples[src_idx];
      push_heap(dst->samples, dst->samples + NUM_SAMPLES, SampleKeyGreater<T>);
    }
    ++src_idx;
  }
  dst->source_size += src->source_size;
}

template <typename T>
void PrintSample(const ReservoirSample<T>& v, ostream* os) { *os << v.val.val; }

template <>
void PrintSample(const ReservoirSample<TinyIntVal>& v, ostream* os) {
  *os << static_cast<int32_t>(v.val.val);
}

template <>
void PrintSample(const ReservoirSample<StringVal>& v, ostream* os) {
  string s(reinterpret_cast<const char*>(&v.val[0]), v.len);
  *os << s;
}

template <>
void PrintSample(const ReservoirSample<DecimalVal>& v, ostream* os) {
  *os << v.val.val16;
}

template <>
void PrintSample(const ReservoirSample<TimestampVal>& v, ostream* os) {
  *os << TimestampValue::FromTimestampVal(v.val).DebugString();
}

template <typename T>
StringVal AggregateFunctions::ReservoirSampleFinalize(FunctionContext* ctx,
    const StringVal& src_val) {
  DCHECK(!src_val.is_null);
  DCHECK_EQ(src_val.len, sizeof(ReservoirSampleState<T>));
  ReservoirSampleState<T>* src = reinterpret_cast<ReservoirSampleState<T>*>(src_val.ptr);

  stringstream out;
  for (int i = 0; i < src->num_samples; ++i) {
    PrintSample<T>(src->samples[i], &out);
    if (i < (src->num_samples - 1)) out << ", ";
  }
  const string& out_str = out.str();
  StringVal result_str(ctx, out_str.size());
  memcpy(result_str.ptr, out_str.c_str(), result_str.len);
  ctx->Free(src_val.ptr);
  return result_str;
}

template <typename T>
StringVal AggregateFunctions::HistogramFinalize(FunctionContext* ctx,
    const StringVal& src_val) {
  DCHECK(!src_val.is_null);
  DCHECK_EQ(src_val.len, sizeof(ReservoirSampleState<T>));

  ReservoirSampleState<T>* src = reinterpret_cast<ReservoirSampleState<T>*>(src_val.ptr);
  sort(src->samples, src->samples + src->num_samples, SampleValLess<T>);

  stringstream out;
  int num_buckets = min(src->num_samples, NUM_BUCKETS);
  int samples_per_bucket = max(src->num_samples / NUM_BUCKETS, 1);
  for (int bucket_idx = 0; bucket_idx < num_buckets; ++bucket_idx) {
    int sample_idx = (bucket_idx + 1) * samples_per_bucket - 1;
    PrintSample<T>(src->samples[sample_idx], &out);
    if (bucket_idx < (num_buckets - 1)) out << ", ";
  }
  const string& out_str = out.str();
  StringVal result_str(ctx, out_str.size());
  memcpy(result_str.ptr, out_str.c_str(), result_str.len);
  ctx->Free(src_val.ptr);
  return result_str;
}

template <typename T>
StringVal AggregateFunctions::AppxMedianFinalize(FunctionContext* ctx,
    const StringVal& src_val) {
  DCHECK(!src_val.is_null);
  DCHECK_EQ(src_val.len, sizeof(ReservoirSampleState<T>));

  ReservoirSampleState<T>* src = reinterpret_cast<ReservoirSampleState<T>*>(src_val.ptr);
  sort(src->samples, src->samples + src->num_samples, SampleValLess<T>);

  stringstream out;
  PrintSample<T>(src->samples[src->num_samples / 2], &out);
  const string& out_str = out.str();
  StringVal result_str(ctx, out_str.size());
  memcpy(result_str.ptr, out_str.c_str(), result_str.len);
  ctx->Free(src_val.ptr);
  return result_str;
}

void AggregateFunctions::HllInit(FunctionContext* ctx, StringVal* dst) {
  int str_len = HLL_LEN;
  dst->is_null = false;
  dst->ptr = ctx->Allocate(str_len);
  dst->len = str_len;
  memset(dst->ptr, 0, str_len);
}

template <typename T>
void AggregateFunctions::HllUpdate(FunctionContext* ctx, const T& src, StringVal* dst) {
  if (src.is_null) return;
  DCHECK(!dst->is_null);
  DCHECK_EQ(dst->len, HLL_LEN);
  uint64_t hash_value =
      AnyValUtil::Hash64(src, *ctx->GetArgType(0), HashUtil::FNV64_SEED);
  if (hash_value != 0) {
    // Use the lower bits to index into the number of streams and then
    // find the first 1 bit after the index bits.
    int idx = hash_value & (HLL_LEN - 1);
    uint8_t first_one_bit = __builtin_ctzl(hash_value >> HLL_PRECISION) + 1;
    dst->ptr[idx] = ::max(dst->ptr[idx], first_one_bit);
  }
}

void AggregateFunctions::HllMerge(FunctionContext* ctx, const StringVal& src,
    StringVal* dst) {
  DCHECK(!dst->is_null);
  DCHECK(!src.is_null);
  DCHECK_EQ(dst->len, HLL_LEN);
  DCHECK_EQ(src.len, HLL_LEN);
  for (int i = 0; i < src.len; ++i) {
    dst->ptr[i] = ::max(dst->ptr[i], src.ptr[i]);
  }
}

StringVal AggregateFunctions::HllFinalize(FunctionContext* ctx, const StringVal& src) {
  DCHECK(!src.is_null);
  DCHECK_EQ(src.len, HLL_LEN);

  const int num_streams = HLL_LEN;
  // Empirical constants for the algorithm.
  float alpha = 0;
  if (num_streams == 16) {
    alpha = 0.673f;
  } else if (num_streams == 32) {
    alpha = 0.697f;
  } else if (num_streams == 64) {
    alpha = 0.709f;
  } else {
    alpha = 0.7213f / (1 + 1.079f / num_streams);
  }

  float harmonic_mean = 0;
  int num_zero_registers = 0;
  for (int i = 0; i < src.len; ++i) {
    harmonic_mean += powf(2.0f, -src.ptr[i]);
    if (src.ptr[i] == 0) ++num_zero_registers;
  }
  harmonic_mean = 1.0f / harmonic_mean;
  int64_t estimate = alpha * num_streams * num_streams * harmonic_mean;

  if (num_zero_registers != 0) {
    // Estimated cardinality is too low. Hll is too inaccurate here, instead use
    // linear counting.
    estimate = num_streams * log(static_cast<float>(num_streams) / num_zero_registers);
  }
  ctx->Free(src.ptr);

  // Output the estimate as ascii string
  stringstream out;
  out << estimate;
  string out_str = out.str();
  StringVal result_str(ctx, out_str.size());
  memcpy(result_str.ptr, out_str.c_str(), result_str.len);
  return result_str;
}

// An implementation of a simple single pass variance algorithm. A standard UDA must
// be single pass (i.e. does not scan the table more than once), so the most canonical
// two pass approach is not practical.
struct KnuthVarianceState {
  double mean;
  double m2;
  int64_t count;
};

// Set pop=true for population variance, false for sample variance
double ComputeKnuthVariance(const KnuthVarianceState& state, bool pop) {
  // Return zero for 1 tuple specified by
  // http://docs.oracle.com/cd/B19306_01/server.102/b14200/functions212.htm
  if (state.count == 1) return 0.0;
  if (pop) return state.m2 / state.count;
  return state.m2 / (state.count - 1);
}

void AggregateFunctions::KnuthVarInit(FunctionContext* ctx, StringVal* dst) {
  dst->is_null = false;
  dst->len = sizeof(KnuthVarianceState);
  dst->ptr = ctx->Allocate(dst->len);
  memset(dst->ptr, 0, dst->len);
}

template <typename T>
void AggregateFunctions::KnuthVarUpdate(FunctionContext* ctx, const T& src,
                                        StringVal* dst) {
  if (src.is_null) return;
  KnuthVarianceState* state = reinterpret_cast<KnuthVarianceState*>(dst->ptr);
  double temp = 1 + state->count;
  double delta = src.val - state->mean;
  double r = delta / temp;
  state->mean += r;
  state->m2 += state->count * delta * r;
  state->count = temp;
}

void AggregateFunctions::KnuthVarMerge(FunctionContext* ctx, const StringVal& src,
                                       StringVal* dst) {
  // Reference implementation:
  // http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Parallel_algorithm
  KnuthVarianceState* src_state = reinterpret_cast<KnuthVarianceState*>(src.ptr);
  KnuthVarianceState* dst_state = reinterpret_cast<KnuthVarianceState*>(dst->ptr);
  if (src_state->count == 0) return;
  double delta = dst_state->mean - src_state->mean;
  double sum_count = dst_state->count + src_state->count;
  dst_state->mean = src_state->mean + delta * (dst_state->count / sum_count);
  dst_state->m2 = (src_state->m2) + dst_state->m2 +
      (delta * delta) * (src_state->count * dst_state->count / sum_count);
  dst_state->count = sum_count;
}

DoubleVal AggregateFunctions::KnuthVarFinalize(
    FunctionContext* ctx, const StringVal& state_sv) {
  KnuthVarianceState* state = reinterpret_cast<KnuthVarianceState*>(state_sv.ptr);
  if (state->count == 0) return DoubleVal::null();
  double variance = ComputeKnuthVariance(*state, false);
  return DoubleVal(variance);
}

StringVal AggregateFunctions::KnuthVarPopFinalize(FunctionContext* ctx,
                                                  const StringVal& state_sv) {
  DCHECK(!state_sv.is_null);
  KnuthVarianceState state = *reinterpret_cast<KnuthVarianceState*>(state_sv.ptr);
  ctx->Free(state_sv.ptr);
  if (state.count == 0) return StringVal::null();
  double variance = ComputeKnuthVariance(state, true);
  return ToStringVal(ctx, variance);
}

StringVal AggregateFunctions::KnuthStddevFinalize(FunctionContext* ctx,
                                                  const StringVal& state_sv) {
  DCHECK(!state_sv.is_null);
  KnuthVarianceState state = *reinterpret_cast<KnuthVarianceState*>(state_sv.ptr);
  ctx->Free(state_sv.ptr);
  if (state.count == 0) return StringVal::null();
  double variance = ComputeKnuthVariance(state, false);
  return ToStringVal(ctx, sqrt(variance));
}

StringVal AggregateFunctions::KnuthStddevPopFinalize(FunctionContext* ctx,
                                                     const StringVal& state_sv) {
  DCHECK(!state_sv.is_null);
  KnuthVarianceState state = *reinterpret_cast<KnuthVarianceState*>(state_sv.ptr);
  ctx->Free(state_sv.ptr);
  if (state.count == 0) return StringVal::null();
  double variance = ComputeKnuthVariance(state, true);
  return ToStringVal(ctx, sqrt(variance));
}

struct RankState {
  int64_t rank;
  int64_t count;
  RankState() : rank(1), count(0) { }
};

void AggregateFunctions::RankInit(FunctionContext* ctx, StringVal* dst) {
  int str_len = sizeof(RankState);
  dst->is_null = false;
  dst->ptr = ctx->Allocate(str_len);
  dst->len = str_len;
  *reinterpret_cast<RankState*>(dst->ptr) = RankState();
}

void AggregateFunctions::RankUpdate(FunctionContext* ctx, StringVal* dst) {
  DCHECK(!dst->is_null);
  DCHECK_EQ(dst->len, sizeof(RankState));
  RankState* state = reinterpret_cast<RankState*>(dst->ptr);
  ++state->count;
}

void AggregateFunctions::DenseRankUpdate(FunctionContext* ctx, StringVal* dst) { }

BigIntVal AggregateFunctions::RankGetValue(FunctionContext* ctx,
    StringVal& src_val) {
  DCHECK(!src_val.is_null);
  DCHECK_EQ(src_val.len, sizeof(RankState));
  RankState* state = reinterpret_cast<RankState*>(src_val.ptr);
  DCHECK_GT(state->count, 0);
  DCHECK_GT(state->rank, 0);
  int64_t result = state->rank;

  // Prepares future calls for the next rank
  state->rank += state->count;
  state->count = 0;
  return BigIntVal(result);
}

BigIntVal AggregateFunctions::DenseRankGetValue(FunctionContext* ctx,
    StringVal& src_val) {
  DCHECK(!src_val.is_null);
  DCHECK_EQ(src_val.len, sizeof(RankState));
  RankState* state = reinterpret_cast<RankState*>(src_val.ptr);
  DCHECK_EQ(state->count, 0);
  DCHECK_GT(state->rank, 0);
  int64_t result = state->rank;

  // Prepares future calls for the next rank
  ++state->rank;
  return BigIntVal(result);
}

BigIntVal AggregateFunctions::RankFinalize(FunctionContext* ctx,
    StringVal& src_val) {
  DCHECK(!src_val.is_null);
  DCHECK_EQ(src_val.len, sizeof(RankState));
  RankState* state = reinterpret_cast<RankState*>(src_val.ptr);
  int64_t result = state->rank;
  ctx->Free(src_val.ptr);
  return BigIntVal(result);
}

// Stamp out the templates for the types we need.
template void AggregateFunctions::InitZero<BigIntVal>(FunctionContext*, BigIntVal* dst);

template void AggregateFunctions::AvgUpdate<BigIntVal>(
    FunctionContext* ctx, const BigIntVal& input, StringVal* dst);
template void AggregateFunctions::AvgUpdate<DoubleVal>(
    FunctionContext* ctx, const DoubleVal& input, StringVal* dst);

template void AggregateFunctions::Sum<TinyIntVal, BigIntVal>(
    FunctionContext*, const TinyIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<SmallIntVal, BigIntVal>(
    FunctionContext*, const SmallIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<IntVal, BigIntVal>(
    FunctionContext*, const IntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<BigIntVal, BigIntVal>(
    FunctionContext*, const BigIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Sum<FloatVal, DoubleVal>(
    FunctionContext*, const FloatVal& src, DoubleVal* dst);
template void AggregateFunctions::Sum<DoubleVal, DoubleVal>(
    FunctionContext*, const DoubleVal& src, DoubleVal* dst);

template void AggregateFunctions::Min<BooleanVal>(
    FunctionContext*, const BooleanVal& src, BooleanVal* dst);
template void AggregateFunctions::Min<TinyIntVal>(
    FunctionContext*, const TinyIntVal& src, TinyIntVal* dst);
template void AggregateFunctions::Min<SmallIntVal>(
    FunctionContext*, const SmallIntVal& src, SmallIntVal* dst);
template void AggregateFunctions::Min<IntVal>(
    FunctionContext*, const IntVal& src, IntVal* dst);
template void AggregateFunctions::Min<BigIntVal>(
    FunctionContext*, const BigIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Min<FloatVal>(
    FunctionContext*, const FloatVal& src, FloatVal* dst);
template void AggregateFunctions::Min<DoubleVal>(
    FunctionContext*, const DoubleVal& src, DoubleVal* dst);
template void AggregateFunctions::Min<StringVal>(
    FunctionContext*, const StringVal& src, StringVal* dst);
template void AggregateFunctions::Min<DecimalVal>(
    FunctionContext*, const DecimalVal& src, DecimalVal* dst);

template void AggregateFunctions::Max<BooleanVal>(
    FunctionContext*, const BooleanVal& src, BooleanVal* dst);
template void AggregateFunctions::Max<TinyIntVal>(
    FunctionContext*, const TinyIntVal& src, TinyIntVal* dst);
template void AggregateFunctions::Max<SmallIntVal>(
    FunctionContext*, const SmallIntVal& src, SmallIntVal* dst);
template void AggregateFunctions::Max<IntVal>(
    FunctionContext*, const IntVal& src, IntVal* dst);
template void AggregateFunctions::Max<BigIntVal>(
    FunctionContext*, const BigIntVal& src, BigIntVal* dst);
template void AggregateFunctions::Max<FloatVal>(
    FunctionContext*, const FloatVal& src, FloatVal* dst);
template void AggregateFunctions::Max<DoubleVal>(
    FunctionContext*, const DoubleVal& src, DoubleVal* dst);
template void AggregateFunctions::Max<StringVal>(
    FunctionContext*, const StringVal& src, StringVal* dst);
template void AggregateFunctions::Max<DecimalVal>(
    FunctionContext*, const DecimalVal& src, DecimalVal* dst);

template void AggregateFunctions::PcUpdate(
    FunctionContext*, const BooleanVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const TinyIntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const SmallIntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const IntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const BigIntVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const FloatVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const DoubleVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const TimestampVal&, StringVal*);
template void AggregateFunctions::PcUpdate(
    FunctionContext*, const DecimalVal&, StringVal*);

template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const BooleanVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const TinyIntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const SmallIntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const IntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const BigIntVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const FloatVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const DoubleVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const TimestampVal&, StringVal*);
template void AggregateFunctions::PcsaUpdate(
    FunctionContext*, const DecimalVal&, StringVal*);

template void AggregateFunctions::ReservoirSampleInit<BooleanVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<TinyIntVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<SmallIntVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<IntVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<BigIntVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<FloatVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<DoubleVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<StringVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<TimestampVal>(
    FunctionContext*, StringVal*);
template void AggregateFunctions::ReservoirSampleInit<DecimalVal>(
    FunctionContext*, StringVal*);

template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const BooleanVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const TinyIntVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const SmallIntVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const IntVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const BigIntVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const FloatVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const DoubleVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const TimestampVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleUpdate(
    FunctionContext*, const DecimalVal&, StringVal*);

template const StringVal AggregateFunctions::ReservoirSampleSerialize<BooleanVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<TinyIntVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<SmallIntVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<IntVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<BigIntVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<FloatVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<DoubleVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<StringVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<TimestampVal>(
    FunctionContext*, const StringVal&);
template const StringVal AggregateFunctions::ReservoirSampleSerialize<DecimalVal>(
    FunctionContext*, const StringVal&);

template void AggregateFunctions::ReservoirSampleMerge<BooleanVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<TinyIntVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<SmallIntVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<IntVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<BigIntVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<FloatVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<DoubleVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<StringVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<TimestampVal>(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::ReservoirSampleMerge<DecimalVal>(
    FunctionContext*, const StringVal&, StringVal*);

template StringVal AggregateFunctions::ReservoirSampleFinalize<BooleanVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<TinyIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<SmallIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<IntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<BigIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<FloatVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<DoubleVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<StringVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<TimestampVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::ReservoirSampleFinalize<DecimalVal>(
    FunctionContext*, const StringVal&);

template StringVal AggregateFunctions::HistogramFinalize<BooleanVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<TinyIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<SmallIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<IntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<BigIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<FloatVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<DoubleVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<StringVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<TimestampVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::HistogramFinalize<DecimalVal>(
    FunctionContext*, const StringVal&);

template StringVal AggregateFunctions::AppxMedianFinalize<BooleanVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<TinyIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<SmallIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<IntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<BigIntVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<FloatVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<DoubleVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<StringVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<TimestampVal>(
    FunctionContext*, const StringVal&);
template StringVal AggregateFunctions::AppxMedianFinalize<DecimalVal>(
    FunctionContext*, const StringVal&);

template void AggregateFunctions::HllUpdate(
    FunctionContext*, const BooleanVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const TinyIntVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const SmallIntVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const IntVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const BigIntVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const FloatVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const DoubleVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const StringVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const TimestampVal&, StringVal*);
template void AggregateFunctions::HllUpdate(
    FunctionContext*, const DecimalVal&, StringVal*);

template void AggregateFunctions::KnuthVarUpdate(
    FunctionContext*, const TinyIntVal&, StringVal*);
template void AggregateFunctions::KnuthVarUpdate(
    FunctionContext*, const SmallIntVal&, StringVal*);
template void AggregateFunctions::KnuthVarUpdate(
    FunctionContext*, const IntVal&, StringVal*);
template void AggregateFunctions::KnuthVarUpdate(
    FunctionContext*, const BigIntVal&, StringVal*);
template void AggregateFunctions::KnuthVarUpdate(
    FunctionContext*, const FloatVal&, StringVal*);
template void AggregateFunctions::KnuthVarUpdate(
    FunctionContext*, const DoubleVal&, StringVal*);
}
