#include <cfenv>
#include <cmath>
#include "interpreter_insreg.h"
#include "interpreter_float.h"
#include "utils/floatutils.h"

// Register move / sign bit manipulation
enum MoveMode
{
   MoveDirect,
   MoveNegate,
   MoveAbsolute,
   MoveNegAbsolute,
};

template<MoveMode mode>
static void
moveGeneric(ThreadState *state, Instruction instr)
{
   uint32_t b0, b1, d0, d1;
   const bool ps0_nan = is_signalling_nan(state->fpr[instr.frB].paired0);
   if (!ps0_nan) {
      // We have to round this if it has excess precision, so we can't just
      // chop off the trailing bits.
      b0 = bit_cast<uint32_t>(static_cast<float>(state->fpr[instr.frB].paired0));
   } else {
      b0 = truncate_double_bits(state->fpr[instr.frB].idw);
   }
   b1 = state->fpr[instr.frB].iw_paired1;

   switch (mode) {
   case MoveDirect:
      d0 = b0;
      d1 = b1;
      break;
   case MoveNegate:
      d0 = b0 ^ 0x80000000;
      d1 = b1 ^ 0x80000000;
      break;
   case MoveAbsolute:
      d0 = b0 & ~0x80000000;
      d1 = b1 & ~0x80000000;
      break;
   case MoveNegAbsolute:
      d0 = b0 | 0x80000000;
      d1 = b1 | 0x80000000;
      break;
   }

   if (!ps0_nan) {
      state->fpr[instr.frD].paired0 = static_cast<double>(bit_cast<float>(d0));
   } else {
      state->fpr[instr.frD].idw = extend_float_nan_bits(d0);
   }
   state->fpr[instr.frD].iw_paired1 = d1;

   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

// Move Register
static void
ps_mr(ThreadState *state, Instruction instr)
{
   return moveGeneric<MoveDirect>(state, instr);
}

// Negate
static void
ps_neg(ThreadState *state, Instruction instr)
{
   return moveGeneric<MoveNegate>(state, instr);
}

// Absolute
static void
ps_abs(ThreadState *state, Instruction instr)
{
   return moveGeneric<MoveAbsolute>(state, instr);
}

// Negative Absolute
static void
ps_nabs(ThreadState *state, Instruction instr)
{
   return moveGeneric<MoveNegAbsolute>(state, instr);
}

// Paired-single arithmetic
enum PSArithOperator {
    PSAdd,
    PSSub,
    PSMul,
    PSDiv,
};

// Returns whether a result value was written (i.e., not aborted by an
// exception).
template<PSArithOperator op, int slotA, int slotB>
static bool
psArithSingle(ThreadState *state, Instruction instr, float *result)
{
   double a, b;
   if (slotA == 0) {
      a = state->fpr[instr.frA].paired0;
   } else {
      a = extend_float(state->fpr[instr.frA].paired1);
   }
   if (slotB == 0) {
      b = state->fpr[op == PSMul ? instr.frC : instr.frB].paired0;
   } else {
      b = extend_float(state->fpr[op == PSMul ? instr.frC : instr.frB].paired1);
   }

   const bool vxsnan = is_signalling_nan(a) || is_signalling_nan(b);
   bool vxisi, vximz, vxidi, vxzdz, zx;
   switch (op) {
   case PSAdd:
      vxisi = is_infinity(a) && is_infinity(b) && std::signbit(a) != std::signbit(b);
      vximz = false;
      vxidi = false;
      vxzdz = false;
      zx = false;
      break;
   case PSSub:
      vxisi = is_infinity(a) && is_infinity(b) && std::signbit(a) == std::signbit(b);
      vximz = false;
      vxidi = false;
      vxzdz = false;
      zx = false;
      break;
   case PSMul:
      vxisi = false;
      vximz = (is_infinity(a) && is_zero(b)) || (is_zero(a) && is_infinity(b));
      vxidi = false;
      vxzdz = false;
      zx = false;
      break;
   case PSDiv:
      vxisi = false;
      vximz = false;
      vxidi = is_infinity(a) && is_infinity(b);
      vxzdz = is_zero(a) && is_zero(b);
      zx = !(vxzdz || vxsnan) && is_zero(b);
      break;
   }

   state->fpscr.vxsnan |= vxsnan;
   state->fpscr.vxisi |= vxisi;
   state->fpscr.vximz |= vximz;
   state->fpscr.vxidi |= vxidi;
   state->fpscr.vxzdz |= vxzdz;
   state->fpscr.zx |= zx;

   const bool vxEnabled = (vxsnan || vxisi || vximz || vxidi || vxzdz) && state->fpscr.ve;
   const bool zxEnabled = zx && state->fpscr.ze;
   if (vxEnabled || zxEnabled) {
      return false;
   }

   float d;
   if (is_nan(a)) {
      d = make_quiet(truncate_double(a));
   } else if (is_nan(b)) {
      d = make_quiet(truncate_double(b));
   } else if (vxisi || vximz || vxidi || vxzdz) {
      d = make_nan<float>();
   } else {
      switch (op) {
      case PSAdd:
         d = static_cast<float>(a + b);
         break;
      case PSSub:
         d = static_cast<float>(a - b);
         break;
      case PSMul:
         d = static_cast<float>(a * b);
         break;
      case PSDiv:
         d = static_cast<float>(a / b);
         break;
      }
   }

   *result = d;
   return true;
}

template<PSArithOperator op, int slotB0, int slotB1>
static void
psArithGeneric(ThreadState *state, Instruction instr)
{
   const uint32_t oldFPSCR = state->fpscr.value;

   float d0, d1;
   const bool wrote0 = psArithSingle<op, 0, slotB0>(state, instr, &d0);
   const bool wrote1 = psArithSingle<op, 1, slotB1>(state, instr, &d1);
   if (wrote0 && wrote1) {
      state->fpr[instr.frD].paired0 = extend_float(d0);
      state->fpr[instr.frD].paired1 = d1;
   }

   if (wrote0) {
      updateFPRF(state, extend_float(d0));
   }
   updateFPSCR(state, oldFPSCR);

   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

// Add
static void
ps_add(ThreadState *state, Instruction instr)
{
   return psArithGeneric<PSAdd, 0, 1>(state, instr);
}

// Subtract
static void
ps_sub(ThreadState *state, Instruction instr)
{
   return psArithGeneric<PSSub, 0, 1>(state, instr);
}

// Multiply
static void
ps_mul(ThreadState *state, Instruction instr)
{
   return psArithGeneric<PSMul, 0, 1>(state, instr);
}

static void
ps_muls0(ThreadState *state, Instruction instr)
{
   return psArithGeneric<PSMul, 0, 0>(state, instr);
}

static void
ps_muls1(ThreadState *state, Instruction instr)
{
   return psArithGeneric<PSMul, 1, 1>(state, instr);
}

// Divide
static void
ps_div(ThreadState *state, Instruction instr)
{
   return psArithGeneric<PSDiv, 0, 1>(state, instr);
}

template<int slot>
static void
psSumGeneric(ThreadState *state, Instruction instr)
{
   const uint32_t oldFPSCR = state->fpscr.value;

   float d;
   if (psArithSingle<PSAdd, 0, 1>(state, instr, &d)) {
      updateFPRF(state, extend_float(d));
      if (slot == 0) {
          state->fpr[instr.frD].paired0 = extend_float(d);
          state->fpr[instr.frD].iw_paired1 = state->fpr[instr.frC].iw_paired1;
      } else {
          float ps0;
          if (is_nan(state->fpr[instr.frC].paired0)) {
             ps0 = truncate_double(state->fpr[instr.frC].paired0);
          } else {
             const bool inexact = std::fetestexcept(FE_INEXACT);
             const bool overflow = std::fetestexcept(FE_OVERFLOW);
             ps0 = static_cast<float>(state->fpr[instr.frC].paired0);
             if (!inexact) {
                 std::feclearexcept(FE_INEXACT);
             }
             if (!overflow) {
                 std::feclearexcept(FE_OVERFLOW);
             }
          }
          state->fpr[instr.frD].paired0 = extend_float(ps0);
          state->fpr[instr.frD].paired1 = d;
      }
   }

   updateFPSCR(state, oldFPSCR);

   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

// Sum High
static void
ps_sum0(ThreadState *state, Instruction instr)
{
   return psSumGeneric<0>(state, instr);
}

// Sum Low
static void
ps_sum1(ThreadState *state, Instruction instr)
{
   return psSumGeneric<1>(state, instr);
}

// Fused multiply-add instructions
enum FMAFlags
{
   FMASubtract   = 1 << 0, // Subtract instead of add
   FMANegate     = 1 << 1, // Negate result
};

// Returns whether a result value was written (i.e., not aborted by an
// exception).
template<unsigned flags, int slotAB, int slotC>
static bool
fmaSingle(ThreadState *state, Instruction instr, float *result)
{
   double a, b, c;
   if (slotAB == 0) {
      a = state->fpr[instr.frA].paired0;
      b = state->fpr[instr.frB].paired0;
   } else {
      a = extend_float(state->fpr[instr.frA].paired1);
      b = extend_float(state->fpr[instr.frB].paired1);
   }
   if (slotC == 0) {
      c = state->fpr[instr.frC].paired0;
   } else {
      c = extend_float(state->fpr[instr.frC].paired1);
   }
   const double addend = (flags & FMASubtract) ? -b : b;

   const bool vxsnan = is_signalling_nan(a) || is_signalling_nan(b) || is_signalling_nan(c);
   const bool vxisi = ((is_infinity(a) || is_infinity(c)) && is_infinity(b)
                       && (std::signbit(a) ^ std::signbit(c)) != std::signbit(addend));
   const bool vximz = (is_infinity(a) && is_zero(c)) || (is_zero(a) && is_infinity(c));

   const uint32_t oldFPSCR = state->fpscr.value;
   state->fpscr.vxsnan |= vxsnan;
   state->fpscr.vxisi |= vxisi;
   state->fpscr.vximz |= vximz;

   if ((vxsnan || vxisi || vximz) && state->fpscr.ve) {
      return false;
   }

   float d;
   if (is_nan(a)) {
      d = make_quiet(truncate_double(a));
   } else if (is_nan(b)) {
      d = make_quiet(truncate_double(b));
   } else if (is_nan(c)) {
      d = make_quiet(truncate_double(c));
   } else if (vxisi || vximz) {
      d = make_nan<float>();
   } else {
      d = static_cast<float>(std::fma(a, c, addend));
      if (flags & FMANegate) {
         d = -d;
      }
   }

   *result = d;
   return true;
}

template<unsigned flags, int slotC0, int slotC1>
static void
fmaGeneric(ThreadState *state, Instruction instr)
{
   const uint32_t oldFPSCR = state->fpscr.value;

   float d0, d1;
   const bool wrote0 = fmaSingle<flags, 0, slotC0>(state, instr, &d0);
   const bool wrote1 = fmaSingle<flags, 1, slotC1>(state, instr, &d1);
   if (wrote0 && wrote1) {
      state->fpr[instr.frD].paired0 = extend_float(d0);
      state->fpr[instr.frD].paired1 = d1;
   }

   if (wrote0) {
      updateFPRF(state, extend_float(d0));
   }
   updateFPSCR(state, oldFPSCR);

   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

static void
ps_madd(ThreadState *state, Instruction instr)
{
   return fmaGeneric<0, 0, 1>(state, instr);
}

static void
ps_madds0(ThreadState *state, Instruction instr)
{
   return fmaGeneric<0, 0, 0>(state, instr);
}

static void
ps_madds1(ThreadState *state, Instruction instr)
{
   return fmaGeneric<0, 1, 1>(state, instr);
}

static void
ps_msub(ThreadState *state, Instruction instr)
{
   return fmaGeneric<FMASubtract, 0, 1>(state, instr);
}

static void
ps_nmadd(ThreadState *state, Instruction instr)
{
   return fmaGeneric<FMANegate, 0, 1>(state, instr);
}

static void
ps_nmsub(ThreadState *state, Instruction instr)
{
   return fmaGeneric<FMANegate | FMASubtract, 0, 1>(state, instr);
}

// Merge registers
enum MergeFlags
{
   MergeValue0 = 1 << 0,
   MergeValue1 = 1 << 1
};

template<unsigned flags = 0>
static void
mergeGeneric(ThreadState *state, Instruction instr)
{
   float d0, d1;

   if (flags & MergeValue0) {
      d0 = state->fpr[instr.frA].paired1;
   } else {
      if (!is_signalling_nan(state->fpr[instr.frA].paired0)) {
         d0 = static_cast<float>(state->fpr[instr.frA].paired0);
      } else {
         d0 = truncate_double(state->fpr[instr.frA].paired0);
      }
   }

   if (flags & MergeValue1) {
      d1 = state->fpr[instr.frB].paired1;
   } else {
      // When inserting a double-precision value into slot 1, the mantissa
      // is truncated rather than rounded.
      d1 = truncate_double(state->fpr[instr.frB].paired0);
   }

   state->fpr[instr.frD].paired0 = extend_float(d0);
   state->fpr[instr.frD].paired1 = d1;

   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

static void
ps_merge00(ThreadState *state, Instruction instr)
{
   return mergeGeneric(state, instr);
}

static void
ps_merge01(ThreadState *state, Instruction instr)
{
   return mergeGeneric<MergeValue1>(state, instr);
}

static void
ps_merge11(ThreadState *state, Instruction instr)
{
   return mergeGeneric<MergeValue0 | MergeValue1>(state, instr);
}

static void
ps_merge10(ThreadState *state, Instruction instr)
{
   return mergeGeneric<MergeValue0>(state, instr);
}

// Reciprocal
static void
ps_res(ThreadState *state, Instruction instr)
{
   const double b0 = state->fpr[instr.frB].paired0;
   const double b1 = extend_float(state->fpr[instr.frB].paired1);

   const bool vxsnan0 = is_signalling_nan(b0);
   const bool vxsnan1 = is_signalling_nan(b1);
   const bool zx0 = is_zero(b0);
   const bool zx1 = is_zero(b1);

   const uint32_t oldFPSCR = state->fpscr.value;
   state->fpscr.vxsnan |= vxsnan0 || vxsnan1;
   state->fpscr.zx |= zx0 || zx1;

   float d0, d1;
   bool write = true;
   if ((vxsnan0 && state->fpscr.ve) || (zx0 && state->fpscr.ze)) {
      write = false;
   } else {
      if (is_nan(b0)) {
         d0 = make_quiet(truncate_double(b0));
      } else if (vxsnan0) {
         d0 = make_nan<float>();
      } else {
         d0 = 1.0f / static_cast<float>(b0);
      }
      updateFPRF(state, d0);
   }
   if ((vxsnan1 && state->fpscr.ve) || (zx1 && state->fpscr.ze)) {
      write = false;
   } else {
      if (is_nan(b1)) {
         d1 = make_quiet(truncate_double(b1));
      } else if (vxsnan1) {
         d1 = make_nan<float>();
      } else {
         d1 = 1.0f / static_cast<float>(b1);
      }
   }

   if (write) {
      state->fpr[instr.frD].paired0 = extend_float(d0);
      state->fpr[instr.frD].paired1 = d1;
   }

   updateFPSCR(state, oldFPSCR);
   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

// Reciprocal Square Root
static void
ps_rsqrte(ThreadState *state, Instruction instr)
{
   const double b0 = state->fpr[instr.frB].paired0;
   const double b1 = extend_float(state->fpr[instr.frB].paired1);

   const bool vxsnan0 = is_signalling_nan(b0);
   const bool vxsnan1 = is_signalling_nan(b1);
   const bool vxsqrt0 = !vxsnan0 && std::signbit(b0) && !is_zero(b0);
   const bool vxsqrt1 = !vxsnan1 && std::signbit(b1) && !is_zero(b1);
   const bool zx0 = is_zero(b0);
   const bool zx1 = is_zero(b1);

   const uint32_t oldFPSCR = state->fpscr.value;
   state->fpscr.vxsnan |= vxsnan0 || vxsnan1;
   state->fpscr.vxsqrt |= vxsqrt0 || vxsqrt1;
   state->fpscr.zx |= zx0 || zx1;

   float d0, d1;
   bool write = true;
   if (((vxsnan0 || vxsqrt0) && state->fpscr.ve) || (zx0 && state->fpscr.ze)) {
      write = false;
   } else {
      if (is_nan(b0)) {
         d0 = make_quiet(truncate_double(b0));
      } else if (vxsnan0 || vxsqrt0) {
         d0 = make_nan<float>();
      } else {
         d0 = 1.0f / std::sqrt(static_cast<float>(b0));
      }
      updateFPRF(state, d0);
   }
   if (((vxsnan1 || vxsqrt1) && state->fpscr.ve) || (zx1 && state->fpscr.ze)) {
      write = false;
   } else {
      if (is_nan(b1)) {
         d1 = make_quiet(truncate_double(b1));
      } else if (vxsnan1 || vxsqrt1) {
         d1 = make_nan<float>();
      } else {
         d1 = 1.0f / std::sqrt(static_cast<float>(b1));
      }
   }

   if (write) {
      state->fpr[instr.frD].paired0 = extend_float(d0);
      state->fpr[instr.frD].paired1 = d1;
   }

   updateFPSCR(state, oldFPSCR);
   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

// Select
static void
ps_sel(ThreadState *state, Instruction instr)
{
   float a0, a1, b0, b1, c0, c1, d0, d1;

   a0 = state->fpr[instr.frA].paired0;
   a1 = state->fpr[instr.frA].paired1;

   b0 = state->fpr[instr.frB].paired0;
   b1 = state->fpr[instr.frB].paired1;

   c0 = state->fpr[instr.frC].paired0;
   c1 = state->fpr[instr.frC].paired1;

   d1 = (a1 >= 0) ? c1 : b1;
   d0 = (a0 >= 0) ? c0 : b0;

   state->fpr[instr.frD].paired0 = d0;
   state->fpr[instr.frD].paired1 = d1;

   if (instr.rc) {
      updateFloatConditionRegister(state);
   }
}

void
cpu::interpreter::registerPairedInstructions()
{
   RegisterInstruction(ps_add);
   RegisterInstruction(ps_div);
   RegisterInstruction(ps_mul);
   RegisterInstruction(ps_sub);
   RegisterInstruction(ps_abs);
   RegisterInstruction(ps_nabs);
   RegisterInstruction(ps_neg);
   RegisterInstruction(ps_sel);
   RegisterInstruction(ps_res);
   RegisterInstruction(ps_rsqrte);
   RegisterInstruction(ps_msub);
   RegisterInstruction(ps_madd);
   RegisterInstruction(ps_nmsub);
   RegisterInstruction(ps_nmadd);
   RegisterInstruction(ps_mr);
   RegisterInstruction(ps_sum0);
   RegisterInstruction(ps_sum1);
   RegisterInstruction(ps_muls0);
   RegisterInstruction(ps_muls1);
   RegisterInstruction(ps_madds0);
   RegisterInstruction(ps_madds1);
   RegisterInstruction(ps_merge00);
   RegisterInstruction(ps_merge01);
   RegisterInstruction(ps_merge10);
   RegisterInstruction(ps_merge11);
}
