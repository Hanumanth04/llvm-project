//===-- Unittests for atanhf16 --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/FPUtil/cast.h"
#include "src/__support/libc_errno.h"
#include "src/math/atanhf16.h"
#include "test/UnitTest/FPMatcher.h"
#include "test/UnitTest/Test.h"

using LlvmLibcAtanhf16Test = LIBC_NAMESPACE::testing::FPTest<float16>;

TEST_F(LlvmLibcAtanhf16Test, SpecialNumbers) {
  libc_errno = 0;
  EXPECT_FP_EQ_WITH_EXCEPTION_ALL_ROUNDING(aNaN, LIBC_NAMESPACE::atanhf16(sNaN),
                                           FE_INVALID);
  EXPECT_MATH_ERRNO(0);

  EXPECT_FP_EQ_ALL_ROUNDING(aNaN, LIBC_NAMESPACE::atanhf16(aNaN));
  EXPECT_MATH_ERRNO(0);

  EXPECT_FP_EQ_ALL_ROUNDING(zero, LIBC_NAMESPACE::atanhf16(zero));
  EXPECT_MATH_ERRNO(0);

  EXPECT_FP_EQ_ALL_ROUNDING(neg_zero, LIBC_NAMESPACE::atanhf16(neg_zero));
  EXPECT_MATH_ERRNO(0);

  EXPECT_FP_EQ_WITH_EXCEPTION(
      inf,
      LIBC_NAMESPACE::atanhf16(LIBC_NAMESPACE::fputil::cast<float16>(1.0f)),
      FE_DIVBYZERO);
  EXPECT_MATH_ERRNO(ERANGE);

  EXPECT_FP_EQ_WITH_EXCEPTION(
      neg_inf,
      LIBC_NAMESPACE::atanhf16(LIBC_NAMESPACE::fputil::cast<float16>(-1.0f)),
      FE_DIVBYZERO);
  EXPECT_MATH_ERRNO(ERANGE);

  EXPECT_FP_IS_NAN_WITH_EXCEPTION(
      LIBC_NAMESPACE::atanhf16(LIBC_NAMESPACE::fputil::cast<float16>(2.0f)),
      FE_INVALID);
  EXPECT_MATH_ERRNO(EDOM);

  EXPECT_FP_IS_NAN_WITH_EXCEPTION(
      LIBC_NAMESPACE::atanhf16(LIBC_NAMESPACE::fputil::cast<float16>(-2.0f)),
      FE_INVALID);
  EXPECT_MATH_ERRNO(EDOM);

  EXPECT_FP_IS_NAN_WITH_EXCEPTION(LIBC_NAMESPACE::atanhf16(inf), FE_INVALID);
  EXPECT_MATH_ERRNO(EDOM);

  EXPECT_FP_IS_NAN_WITH_EXCEPTION(LIBC_NAMESPACE::atanhf16(neg_inf),
                                  FE_INVALID);
  EXPECT_MATH_ERRNO(EDOM);
}
