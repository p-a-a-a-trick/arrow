// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <algorithm>  // Missing include in boost/process

// This boost/asio/io_context.hpp include is needless for no MinGW
// build.
//
// This is for including boost/asio/detail/socket_types.hpp before any
// "#include <windows.h>". boost/asio/detail/socket_types.hpp doesn't
// work if windows.h is already included. boost/process.h ->
// boost/process/args.hpp -> boost/process/detail/basic_cmd.hpp
// includes windows.h. boost/process/args.hpp is included before
// boost/process/async.h that includes
// boost/asio/detail/socket_types.hpp implicitly is included.
#include <boost/asio/io_context.hpp>
// We need BOOST_USE_WINDOWS_H definition with MinGW when we use
// boost/process.hpp. See BOOST_USE_WINDOWS_H=1 in
// cpp/cmake_modules/ThirdpartyToolchain.cmake for details.
#include <boost/process.hpp>

#include "arrow/filesystem/azurefs.h"

#include <random>
#include <string>

#include <gmock/gmock-matchers.h>
#include <gmock/gmock-more-matchers.h>
#include <gtest/gtest.h>
#include <azure/identity/client_secret_credential.hpp>
#include <azure/identity/default_azure_credential.hpp>
#include <azure/identity/managed_identity_credential.hpp>
#include <azure/storage/blobs.hpp>
#include <azure/storage/common/storage_credential.hpp>

#include "arrow/testing/gtest_util.h"
#include "arrow/testing/util.h"
#include "arrow/util/io_util.h"
#include "arrow/util/key_value_metadata.h"
#include "arrow/util/string.h"
#include "arrow/util/value_parsing.h"

namespace arrow {
using internal::TemporaryDir;
namespace fs {
namespace {
namespace bp = boost::process;

using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::NotNull;

auto const* kLoremIpsum = R"""(
Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor
incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis
nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.
Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu
fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in
culpa qui officia deserunt mollit anim id est laborum.
)""";

class AzuriteEnv : public ::testing::Environment {
 public:
  AzuriteEnv() {
    account_name_ = "devstoreaccount1";
    account_key_ =
        "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/"
        "KBHBeksoGMGw==";
    auto exe_path = bp::search_path("azurite");
    if (exe_path.empty()) {
      auto error = std::string("Could not find Azurite emulator.");
      status_ = Status::Invalid(error);
      return;
    }
    auto temp_dir_ = *TemporaryDir::Make("azurefs-test-");
    server_process_ = bp::child(boost::this_process::environment(), exe_path, "--silent",
                                "--location", temp_dir_->path().ToString(), "--debug",
                                temp_dir_->path().ToString() + "/debug.log");
    if (!(server_process_.valid() && server_process_.running())) {
      auto error = "Could not start Azurite emulator.";
      server_process_.terminate();
      server_process_.wait();
      status_ = Status::Invalid(error);
      return;
    }
    status_ = Status::OK();
  }

  ~AzuriteEnv() override {
    server_process_.terminate();
    server_process_.wait();
  }

  const std::string& account_name() const { return account_name_; }
  const std::string& account_key() const { return account_key_; }
  const Status status() const { return status_; }

 private:
  std::string account_name_;
  std::string account_key_;
  bp::child server_process_;
  Status status_;
  std::unique_ptr<TemporaryDir> temp_dir_;
};

auto* azurite_env = ::testing::AddGlobalTestEnvironment(new AzuriteEnv);

AzuriteEnv* GetAzuriteEnv() {
  return ::arrow::internal::checked_cast<AzuriteEnv*>(azurite_env);
}

// Placeholder tests
// TODO: GH-18014 Remove once a proper test is added
TEST(AzureFileSystem, InitializeCredentials) {
  auto default_credential = std::make_shared<Azure::Identity::DefaultAzureCredential>();
  auto managed_identity_credential =
      std::make_shared<Azure::Identity::ManagedIdentityCredential>();
  auto service_principal_credential =
      std::make_shared<Azure::Identity::ClientSecretCredential>("tenant_id", "client_id",
                                                                "client_secret");
}

TEST(AzureFileSystem, OptionsCompare) {
  AzureOptions options;
  EXPECT_TRUE(options.Equals(options));
}

class TestAzureFileSystem : public ::testing::Test {
 public:
  std::shared_ptr<FileSystem> fs_;
  std::shared_ptr<Azure::Storage::Blobs::BlobServiceClient> service_client_;
  std::mt19937_64 generator_;
  std::string container_name_;

  TestAzureFileSystem() : generator_(std::random_device()()) {}

  AzureOptions MakeOptions() {
    const std::string& account_name = GetAzuriteEnv()->account_name();
    const std::string& account_key = GetAzuriteEnv()->account_key();
    AzureOptions options;
    options.backend = AzureBackend::Azurite;
    ARROW_EXPECT_OK(options.ConfigureAccountKeyCredentials(account_name, account_key));
    return options;
  }

  void SetUp() override {
    ASSERT_THAT(GetAzuriteEnv(), NotNull());
    ASSERT_OK(GetAzuriteEnv()->status());

    container_name_ = RandomChars(32);
    auto options = MakeOptions();
    service_client_ = std::make_shared<Azure::Storage::Blobs::BlobServiceClient>(
        options.account_blob_url, options.storage_credentials_provider);
    ASSERT_OK_AND_ASSIGN(fs_, AzureFileSystem::Make(options));
    auto container_client = service_client_->GetBlobContainerClient(container_name_);
    container_client.CreateIfNotExists();

    auto blob_client = container_client.GetBlockBlobClient(PreexistingObjectName());
    blob_client.UploadFrom(reinterpret_cast<const uint8_t*>(kLoremIpsum),
                           strlen(kLoremIpsum));
  }

  void TearDown() override {
    auto containers = service_client_->ListBlobContainers();
    for (auto container : containers.BlobContainers) {
      auto container_client = service_client_->GetBlobContainerClient(container.Name);
      container_client.DeleteIfExists();
    }
  }

  std::string PreexistingContainerName() const { return container_name_; }

  std::string PreexistingContainerPath() const {
    return PreexistingContainerName() + '/';
  }

  static std::string PreexistingObjectName() { return "test-object-name"; }

  std::string PreexistingObjectPath() const {
    return PreexistingContainerPath() + PreexistingObjectName();
  }

  std::string NotFoundObjectPath() { return PreexistingContainerPath() + "not-found"; }

  std::string RandomLine(int lineno, std::size_t width) {
    auto line = std::to_string(lineno) + ":    ";
    line += RandomChars(width - line.size() - 1);
    line += '\n';
    return line;
  }

  std::size_t RandomIndex(std::size_t end) {
    return std::uniform_int_distribution<std::size_t>(0, end - 1)(generator_);
  }

  std::string RandomChars(std::size_t count) {
    auto const fillers = std::string("abcdefghijlkmnopqrstuvwxyz0123456789");
    std::uniform_int_distribution<std::size_t> d(0, fillers.size() - 1);
    std::string s;
    std::generate_n(std::back_inserter(s), count, [&] { return fillers[d(generator_)]; });
    return s;
  }

  void UploadLines(const std::vector<std::string>& lines, const char* path_to_file,
                   int total_size) {
    // TODO(GH-38333): Switch to using Azure filesystem to write once its implemented.
    auto blob_client = service_client_->GetBlobContainerClient(PreexistingContainerName())
                           .GetBlockBlobClient(path_to_file);
    std::string all_lines = std::accumulate(lines.begin(), lines.end(), std::string(""));
    blob_client.UploadFrom(reinterpret_cast<const uint8_t*>(all_lines.data()),
                           total_size);
  }
};

TEST_F(TestAzureFileSystem, OpenInputStreamString) {
  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(PreexistingObjectPath()));

  ASSERT_OK_AND_ASSIGN(auto buffer, stream->Read(1024));
  EXPECT_EQ(buffer->ToString(), kLoremIpsum);
}

TEST_F(TestAzureFileSystem, OpenInputStreamStringBuffers) {
  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(PreexistingObjectPath()));

  std::string contents;
  std::shared_ptr<Buffer> buffer;
  do {
    ASSERT_OK_AND_ASSIGN(buffer, stream->Read(16));
    contents.append(buffer->ToString());
  } while (buffer && buffer->size() != 0);

  EXPECT_EQ(contents, kLoremIpsum);
}

TEST_F(TestAzureFileSystem, OpenInputStreamInfo) {
  // TODO(GH-38335): When implemented use ASSERT_OK_AND_ASSIGN(info,
  // fs->GetFileInfo(PreexistingObjectPath()));
  arrow::fs::FileInfo info(PreexistingObjectPath(), FileType::File);

  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(info));

  ASSERT_OK_AND_ASSIGN(auto buffer, stream->Read(1024));
  EXPECT_EQ(buffer->ToString(), kLoremIpsum);
}

TEST_F(TestAzureFileSystem, OpenInputStreamEmpty) {
  const auto path_to_file = "empty-object.txt";
  const auto path = PreexistingContainerPath() + path_to_file;
  service_client_->GetBlobContainerClient(PreexistingContainerName())
      .GetBlockBlobClient(path_to_file)
      .UploadFrom(nullptr, 0);

  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputStream(path));
  std::array<char, 1024> buffer{};
  std::int64_t size;
  ASSERT_OK_AND_ASSIGN(size, stream->Read(buffer.size(), buffer.data()));
  EXPECT_EQ(size, 0);
}

TEST_F(TestAzureFileSystem, OpenInputStreamNotFound) {
  ASSERT_RAISES(IOError, fs_->OpenInputStream(NotFoundObjectPath()));
}

TEST_F(TestAzureFileSystem, OpenInputStreamInfoInvalid) {
  // TODO(GH-38335): When implemented use ASSERT_OK_AND_ASSIGN(info,
  // fs->GetFileInfo(PreexistingBucketPath()));
  arrow::fs::FileInfo info(PreexistingContainerPath(), FileType::Directory);
  ASSERT_RAISES(IOError, fs_->OpenInputStream(info));

  // TODO(GH-38335): When implemented use ASSERT_OK_AND_ASSIGN(info,
  // fs->GetFileInfo(NotFoundObjectPath()));
  arrow::fs::FileInfo info2(PreexistingContainerPath(), FileType::NotFound);
  ASSERT_RAISES(IOError, fs_->OpenInputStream(info2));
}

TEST_F(TestAzureFileSystem, OpenInputStreamUri) {
  ASSERT_RAISES(Invalid, fs_->OpenInputStream("abfss://" + PreexistingObjectPath()));
}

TEST_F(TestAzureFileSystem, OpenInputStreamTrailingSlash) {
  ASSERT_RAISES(IOError, fs_->OpenInputStream(PreexistingObjectPath() + '/'));
}

namespace {
std::shared_ptr<const KeyValueMetadata> NormalizerKeyValueMetadata(
    std::shared_ptr<const KeyValueMetadata> metadata) {
  auto normalized = std::make_shared<KeyValueMetadata>();
  for (int64_t i = 0; i < metadata->size(); ++i) {
    auto key = metadata->key(i);
    auto value = metadata->value(i);
    if (key == "Content-Hash") {
      std::vector<uint8_t> output;
      output.reserve(value.size() / 2);
      if (ParseHexValues(value, output.data()).ok()) {
        // Valid value
        value = std::string(value.size(), 'F');
      }
    } else if (key == "Last-Modified" || key == "Created-On" ||
               key == "Access-Tier-Changed-On") {
      auto parser = TimestampParser::MakeISO8601();
      int64_t output;
      if ((*parser)(value.data(), value.size(), TimeUnit::NANO, &output)) {
        // Valid value
        value = "2023-10-31T08:15:20Z";
      }
    } else if (key == "ETag") {
      if (internal::StartsWith(value, "\"") && internal::EndsWith(value, "\"")) {
        // Valid value
        value = "\"ETagValue\"";
      }
    }
    normalized->Append(key, value);
  }
  return normalized;
}
};  // namespace

TEST_F(TestAzureFileSystem, OpenInputStreamReadMetadata) {
  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(PreexistingObjectPath()));

  std::shared_ptr<const KeyValueMetadata> actual;
  ASSERT_OK_AND_ASSIGN(actual, stream->ReadMetadata());
  ASSERT_EQ(
      "\n"
      "-- metadata --\n"
      "Content-Type: application/octet-stream\n"
      "Content-Encoding: \n"
      "Content-Language: \n"
      "Content-Hash: FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n"
      "Content-Disposition: \n"
      "Cache-Control: \n"
      "Last-Modified: 2023-10-31T08:15:20Z\n"
      "Created-On: 2023-10-31T08:15:20Z\n"
      "Blob-Type: BlockBlob\n"
      "Lease-State: available\n"
      "Lease-Status: unlocked\n"
      "Content-Length: 447\n"
      "ETag: \"ETagValue\"\n"
      "IsServerEncrypted: true\n"
      "Access-Tier: Hot\n"
      "Is-Access-Tier-Inferred: true\n"
      "Access-Tier-Changed-On: 2023-10-31T08:15:20Z\n"
      "Has-Legal-Hold: false",
      NormalizerKeyValueMetadata(actual)->ToString());
}

TEST_F(TestAzureFileSystem, OpenInputStreamClosed) {
  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputStream(PreexistingObjectPath()));
  ASSERT_OK(stream->Close());
  std::array<char, 16> buffer{};
  ASSERT_RAISES(Invalid, stream->Read(buffer.size(), buffer.data()));
  ASSERT_RAISES(Invalid, stream->Read(buffer.size()));
  ASSERT_RAISES(Invalid, stream->Tell());
}

TEST_F(TestAzureFileSystem, OpenInputFileMixedReadVsReadAt) {
  // Create a file large enough to make the random access tests non-trivial.
  auto constexpr kLineWidth = 100;
  auto constexpr kLineCount = 4096;
  std::vector<std::string> lines(kLineCount);
  int lineno = 0;
  std::generate_n(lines.begin(), lines.size(),
                  [&] { return RandomLine(++lineno, kLineWidth); });

  const auto path_to_file = "OpenInputFileMixedReadVsReadAt/object-name";
  const auto path = PreexistingContainerPath() + path_to_file;

  UploadLines(lines, path_to_file, kLineCount * kLineWidth);

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(path));
  for (int i = 0; i != 32; ++i) {
    SCOPED_TRACE("Iteration " + std::to_string(i));
    // Verify sequential reads work as expected.
    std::array<char, kLineWidth> buffer{};
    std::int64_t size;
    {
      ASSERT_OK_AND_ASSIGN(auto actual, file->Read(kLineWidth));
      EXPECT_EQ(lines[2 * i], actual->ToString());
    }
    {
      ASSERT_OK_AND_ASSIGN(size, file->Read(buffer.size(), buffer.data()));
      EXPECT_EQ(size, kLineWidth);
      auto actual = std::string{buffer.begin(), buffer.end()};
      EXPECT_EQ(lines[2 * i + 1], actual);
    }

    // Verify random reads interleave too.
    auto const index = RandomIndex(kLineCount);
    auto const position = index * kLineWidth;
    ASSERT_OK_AND_ASSIGN(size, file->ReadAt(position, buffer.size(), buffer.data()));
    EXPECT_EQ(size, kLineWidth);
    auto actual = std::string{buffer.begin(), buffer.end()};
    EXPECT_EQ(lines[index], actual);

    // Verify random reads using buffers work.
    ASSERT_OK_AND_ASSIGN(auto b, file->ReadAt(position, kLineWidth));
    EXPECT_EQ(lines[index], b->ToString());
  }
}

TEST_F(TestAzureFileSystem, OpenInputFileRandomSeek) {
  // Create a file large enough to make the random access tests non-trivial.
  auto constexpr kLineWidth = 100;
  auto constexpr kLineCount = 4096;
  std::vector<std::string> lines(kLineCount);
  int lineno = 0;
  std::generate_n(lines.begin(), lines.size(),
                  [&] { return RandomLine(++lineno, kLineWidth); });

  const auto path_to_file = "OpenInputFileRandomSeek/object-name";
  const auto path = PreexistingContainerPath() + path_to_file;
  std::shared_ptr<io::OutputStream> output;

  UploadLines(lines, path_to_file, kLineCount * kLineWidth);

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(path));
  for (int i = 0; i != 32; ++i) {
    SCOPED_TRACE("Iteration " + std::to_string(i));
    // Verify sequential reads work as expected.
    auto const index = RandomIndex(kLineCount);
    auto const position = index * kLineWidth;
    ASSERT_OK(file->Seek(position));
    ASSERT_OK_AND_ASSIGN(auto actual, file->Read(kLineWidth));
    EXPECT_EQ(lines[index], actual->ToString());
  }
}

TEST_F(TestAzureFileSystem, OpenInputFileIoContext) {
  // Create a test file.
  const auto path_to_file = "OpenInputFileIoContext/object-name";
  const auto path = PreexistingContainerPath() + path_to_file;
  const std::string contents = "The quick brown fox jumps over the lazy dog";

  auto blob_client = service_client_->GetBlobContainerClient(PreexistingContainerName())
                         .GetBlockBlobClient(path_to_file);
  blob_client.UploadFrom(reinterpret_cast<const uint8_t*>(contents.data()),
                         contents.length());

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(path));
  EXPECT_EQ(fs_->io_context().external_id(), file->io_context().external_id());
}

TEST_F(TestAzureFileSystem, OpenInputFileInfo) {
  // TODO(GH-38335): When implemented use ASSERT_OK_AND_ASSIGN(info,
  // fs->GetFileInfo(PreexistingObjectPath()));
  arrow::fs::FileInfo info(PreexistingObjectPath(), FileType::File);

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(info));

  std::array<char, 1024> buffer{};
  std::int64_t size;
  auto constexpr kStart = 16;
  ASSERT_OK_AND_ASSIGN(size, file->ReadAt(kStart, buffer.size(), buffer.data()));

  auto const expected = std::string(kLoremIpsum).substr(kStart);
  EXPECT_EQ(std::string(buffer.data(), size), expected);
}

TEST_F(TestAzureFileSystem, OpenInputFileNotFound) {
  ASSERT_RAISES(IOError, fs_->OpenInputFile(NotFoundObjectPath()));
}

TEST_F(TestAzureFileSystem, OpenInputFileInfoInvalid) {
  // TODO(GH-38335): When implemented use ASSERT_OK_AND_ASSIGN(info,
  // fs->GetFileInfo(PreexistingContainerPath()));
  arrow::fs::FileInfo info(PreexistingContainerPath(), FileType::File);
  ASSERT_RAISES(IOError, fs_->OpenInputFile(info));

  // TODO(GH-38335): When implemented use ASSERT_OK_AND_ASSIGN(info,
  // fs->GetFileInfo(NotFoundObjectPath()));
  arrow::fs::FileInfo info2(NotFoundObjectPath(), FileType::NotFound);
  ASSERT_RAISES(IOError, fs_->OpenInputFile(info2));
}

TEST_F(TestAzureFileSystem, OpenInputFileClosed) {
  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputFile(PreexistingObjectPath()));
  ASSERT_OK(stream->Close());
  std::array<char, 16> buffer{};
  ASSERT_RAISES(Invalid, stream->Tell());
  ASSERT_RAISES(Invalid, stream->Read(buffer.size(), buffer.data()));
  ASSERT_RAISES(Invalid, stream->Read(buffer.size()));
  ASSERT_RAISES(Invalid, stream->ReadAt(1, buffer.size(), buffer.data()));
  ASSERT_RAISES(Invalid, stream->ReadAt(1, 1));
  ASSERT_RAISES(Invalid, stream->Seek(2));
}

}  // namespace
}  // namespace fs
}  // namespace arrow
