// Pins the image-model preference policy.
//
// This logic decides which model the frame uses tomorrow, and gets it wrong in a
// costly way: either probing models that cannot work (wasted requests and a
// worse picture), or demoting a healthy model because of one network blip. Both
// have already happened once, so the decision rules are pinned here.

#include <gtest/gtest.h>

#include "config.h"
#include "image_model_policy.h"

TEST(ImageModelPolicy, ListIsOrderedAndHasNoDuplicates)
{
    int n = 0;
    const char *const *cands = image_model_candidates(&n);
    ASSERT_GE(n, 2);
    for (int i = 0; i < n; i++) {
        EXPECT_NE(nullptr, cands[i]);
        for (int j = i + 1; j < n; j++) {
            EXPECT_STRNE(cands[i], cands[j]) << "duplicate at " << i << " and " << j;
        }
    }
}

TEST(ImageModelPolicy, NextWalksForwardAndStopsAtTheEnd)
{
    int n = 0;
    const char *const *cands = image_model_candidates(&n);
    for (int i = 0; i + 1 < n; i++) {
        EXPECT_STREQ(cands[i + 1], image_model_next(cands[i]));
    }
    // Last entry stays put rather than wrapping to the top: wrapping would
    // re-probe everything that already failed, every day.
    EXPECT_STREQ(cands[n - 1], image_model_next(cands[n - 1]));
}

TEST(ImageModelPolicy, UnknownModelFallsBackToTheHead)
{
    int n = 0;
    const char *const *cands = image_model_candidates(&n);
    EXPECT_STREQ(cands[0], image_model_next("some/model/not/in/the/list"));
    EXPECT_STREQ(cands[0], image_model_next(nullptr));
}

TEST(ImageModelPolicy, TransientFailuresDemoteOnlyAfterTheThreshold)
{
    EXPECT_FALSE(image_model_should_demote(0));
    EXPECT_FALSE(image_model_should_demote(1));
    EXPECT_FALSE(image_model_should_demote(IMAGE_MODEL_DEMOTE_AFTER_FAILS - 1));
    EXPECT_TRUE(image_model_should_demote(IMAGE_MODEL_DEMOTE_AFTER_FAILS));
    EXPECT_TRUE(image_model_should_demote(IMAGE_MODEL_DEMOTE_AFTER_FAILS + 1));
}
