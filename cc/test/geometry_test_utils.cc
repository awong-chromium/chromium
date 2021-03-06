// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/test/geometry_test_utils.h"

#include "base/logging.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/transform.h"

namespace cc {

// NOTE: even though transform data types use double precision, we only check
// for equality within single-precision error bounds because many transforms
// originate from single-precision data types such as quads/rects/etc.

void ExpectTransformationMatrixEq(const gfx::Transform& expected,
                                  const gfx::Transform& actual)
{
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(0, 0), (actual).matrix().getDouble(0, 0));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(1, 0), (actual).matrix().getDouble(1, 0));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(2, 0), (actual).matrix().getDouble(2, 0));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(3, 0), (actual).matrix().getDouble(3, 0));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(0, 1), (actual).matrix().getDouble(0, 1));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(1, 1), (actual).matrix().getDouble(1, 1));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(2, 1), (actual).matrix().getDouble(2, 1));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(3, 1), (actual).matrix().getDouble(3, 1));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(0, 2), (actual).matrix().getDouble(0, 2));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(1, 2), (actual).matrix().getDouble(1, 2));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(2, 2), (actual).matrix().getDouble(2, 2));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(3, 2), (actual).matrix().getDouble(3, 2));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(0, 3), (actual).matrix().getDouble(0, 3));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(1, 3), (actual).matrix().getDouble(1, 3));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(2, 3), (actual).matrix().getDouble(2, 3));
    EXPECT_FLOAT_EQ((expected).matrix().getDouble(3, 3), (actual).matrix().getDouble(3, 3));
}

gfx::Transform inverse(const gfx::Transform& transform)
{
    gfx::Transform result(gfx::Transform::kSkipInitialization);
    bool invertedSuccessfully = transform.GetInverse(&result);
    DCHECK(invertedSuccessfully);
    return result;
}

}  // namespace cc
