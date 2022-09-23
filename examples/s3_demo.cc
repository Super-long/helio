// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include <absl/strings/str_split.h>
#include <absl/strings/strip.h>
#include <absl/time/time.h>

#include "base/init.h"
#include "util/cloud/aws.h"
#include "util/cloud/s3.h"
#include "util/http/http_client.h"
#include "util/uring/uring_pool.h"

using namespace std;
using namespace util;
using cloud::AWS;

ABSL_FLAG(string, cmd, "ls", "");
ABSL_FLAG(string, region, "us-east-1", "");
ABSL_FLAG(string, path, "", "s3://bucket/path");

namespace h2 = boost::beast::http;

#define CHECK_EC(x)                                                                 \
  do {                                                                              \
    auto __ec$ = (x);                                                               \
    CHECK(!__ec$) << "Error: " << __ec$ << " " << __ec$.message() << " for " << #x; \
  } while (false)

template <typename Body> std::ostream& operator<<(std::ostream& os, const h2::request<Body>& msg) {
  os << msg.method_string() << " " << msg.target() << endl;
  for (const auto& f : msg) {
    os << f.name_string() << " : " << f.value() << endl;
  }
  os << "-------------------------";

  return os;
}

void ListBuckets(const AWS& aws, ProactorBase* proactor) {
  http::Client http_client{proactor};

  http_client.set_connect_timeout_ms(2000);
  auto list_res = proactor->Await([&] {
    CHECK_EC(http_client.Connect("s3.amazonaws.com", "80"));

    return ListS3Buckets(aws, &http_client);
  });

  CHECK(list_res);

  for (const auto& b : *list_res) {
    cout << b << endl;
  }
};

void ListObjects(const AWS& aws, string_view bucket, string_view path, ProactorBase* proactor) {
  LOG(INFO) << "list " << bucket << " " << path;

  http::Client http_client{proactor};
  http_client.set_connect_timeout_ms(2000);

  auto list_res = proactor->Await([&] {
    CHECK_EC(http_client.Connect("s3.amazonaws.com", "80"));
    return error_code{};
  });

  CHECK(!list_res);
}

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  unique_ptr<ProactorPool> pp;
  pp.reset(new uring::UringPool);

  pp->Run();

  AWS aws{absl::GetFlag(FLAGS_region), "s3"};
  CHECK_EC(aws.Init());

  string cmd = absl::GetFlag(FLAGS_cmd);
  string path = absl::GetFlag(FLAGS_path);

  if (cmd == "ls") {
    if (path.empty()) {
      ListBuckets(aws, pp->GetNextProactor());
    } else {
      string_view clean = absl::StripPrefix(path, "s3://");
      string_view obj_path;
      size_t pos = clean.find('/');
      string_view bucket = clean.substr(0, pos);

      if (pos != string_view::npos) {
        obj_path = clean.substr(pos + 1);
      }
      ListObjects(aws, bucket, obj_path, pp->GetNextProactor());
    }
  } else {
    LOG(ERROR) << "Unknown command " << cmd;
  }

  pp->Stop();

  return 0;
}