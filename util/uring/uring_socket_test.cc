// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/uring_socket.h"

#include "base/gtest.h"
#include "base/logging.h"

namespace util {
namespace uring {

constexpr uint32_t kRingDepth = 8;
using namespace std;
namespace fibers = boost::fibers;

class UringSocketTest : public testing::Test {
 protected:
  void SetUp() final;

  void TearDown() final {
    listen_socket_.reset();
    proactor_->Stop();
    proactor_thread_.join();
    proactor_.reset();
  }

  static void SetUpTestCase() {
    testing::FLAGS_gtest_death_test_style = "threadsafe";
  }

  using IoResult = Proactor::IoResult;

  unique_ptr<Proactor> proactor_;
  thread proactor_thread_;
  unique_ptr<LinuxSocketBase> listen_socket_;
  uint16_t listen_port_ = 0;
  fibers::fiber accept_fb_;
  std::error_code accept_ec_;
  FiberSocketBase::endpoint_type listen_ep_;
};

void UringSocketTest::SetUp() {
  proactor_ = std::make_unique<Proactor>();
  proactor_thread_ = thread{[this] {
    proactor_->Init(kRingDepth);
    proactor_->Run();
  }};
  listen_socket_.reset(proactor_->CreateSocket());
  auto ec = listen_socket_->Listen(0, 0);
  CHECK(!ec);
  listen_port_ = listen_socket_->LocalEndpoint().port();

  auto address = boost::asio::ip::make_address("127.0.0.1");
  listen_ep_ = FiberSocketBase::endpoint_type{address, listen_port_};

  accept_fb_ = proactor_->LaunchFiber([this] {
    auto accept_res = listen_socket_->Accept();
    if (accept_res) {
      delete *accept_res;
    } else {
      accept_ec_ = accept_res.error();
    }
  });
}

TEST_F(UringSocketTest, Basic) {
  unique_ptr<LinuxSocketBase> sock(proactor_->CreateSocket());

  proactor_->AwaitBlocking([&] {
    error_code ec = sock->Connect(listen_ep_);
    EXPECT_FALSE(ec);
    accept_fb_.join();

    ASSERT_FALSE(accept_ec_);
  });
}

TEST_F(UringSocketTest, Timeout) {
  unique_ptr<LinuxSocketBase> sock[2];
  for (size_t i = 0; i < 2; ++i) {
    sock[i].reset(proactor_->CreateSocket());
    sock[i]->set_timeout(5);  // we set timeout that won't supposed to trigger.
  }

  proactor_->AwaitBlocking([&] {
    for (size_t i = 0; i < 2; ++i) {
      error_code ec = sock[i]->Connect(listen_ep_);
      EXPECT_FALSE(ec);
    }
  });
  accept_fb_.join();
  ASSERT_FALSE(accept_ec_);

  unique_ptr<LinuxSocketBase> tm_sock(proactor_->CreateSocket());
  tm_sock->set_timeout(5);

  error_code tm_ec = proactor_->AwaitBlocking([&] {
    return tm_sock->Connect(listen_ep_);
  });
  EXPECT_EQ(tm_ec, errc::operation_canceled);

  // sock[0] was accepted and then its peer was deleted.
  // therefore, we read from sock[1] that was opportunistically accepted with the ack from peer.
  uint8_t buf[16];
  io::Result<size_t> read_res = proactor_->AwaitBlocking([&] { return sock[1]->Recv(buf); });
  EXPECT_EQ(read_res.error(), errc::operation_canceled);
}

}  // namespace uring
}  // namespace util