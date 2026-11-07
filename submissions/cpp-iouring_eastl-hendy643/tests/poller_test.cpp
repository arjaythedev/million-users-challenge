#include <gtest/gtest.h>
#include <liburing.h>

TEST(Poller, IoUringRingWorksAndHasTheFeaturesTheLoopUses)
{
    io_uring ring{};
    io_uring_params p{};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    int rc = io_uring_queue_init_params(8, &ring, &p);
    if (rc < 0)
    {
        p = io_uring_params{};
        rc = io_uring_queue_init_params(8, &ring, &p);
    }
    ASSERT_EQ(rc, 0) << "io_uring is unavailable";
    EXPECT_TRUE(p.features & IORING_FEAT_NODROP);

    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    ASSERT_NE(sqe, nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 42);
    ASSERT_EQ(io_uring_submit_and_wait(&ring, 1), 1);
    io_uring_cqe *cqe = nullptr;
    ASSERT_EQ(io_uring_peek_cqe(&ring, &cqe), 0);
    EXPECT_EQ(cqe->user_data, 42u);
    io_uring_cqe_seen(&ring, cqe);

    int err = 0;
    io_uring_buf_ring *br = io_uring_setup_buf_ring(&ring, 8, 0, 0, &err);
    EXPECT_NE(br, nullptr) << "err " << err;
    io_uring_queue_exit(&ring);
}
