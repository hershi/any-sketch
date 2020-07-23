// Copyright 2020 The Any Sketch Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sketch_encrypter.h"

#include <openssl/obj_mac.h>

#include "absl/types/span.h"
// TODO(wangyaopw): use "external/*" path for blinders headers
#include "absl/strings/string_view.h"
#include "crypto/commutative_elgamal.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace wfa::any_sketch::crypto {
namespace {

using ::private_join_and_compute::CommutativeElGamal;
using ::private_join_and_compute::Context;
using ::private_join_and_compute::ECGroup;
using ::private_join_and_compute::ECPoint;
using ::private_join_and_compute::InternalError;
using ::testing::SizeIs;
using ::wfa::measurement::api::v1alpha::Sketch;
using ::wfa::measurement::api::v1alpha::SketchConfig;

constexpr int kTestCurveId = NID_X9_62_prime256v1;
constexpr int kMaxCounterValue = 100;

// Returns true if the decryption of expected is the same with that of arg.
MATCHER_P2(HasSameDecryption, original_cipher, expected, "") {
  auto decrypted_actual =
      original_cipher->Decrypt(std::make_pair(arg.u, arg.e)).value();
  auto decrypted_expected =
      original_cipher->Decrypt(std::make_pair(expected.u, expected.e)).value();
  return ExplainMatchResult(testing::Eq(decrypted_expected), decrypted_actual,
                            result_listener);
}

// Helper function to create a SketchConfig.
SketchConfig CreateSketchConfig(const int unique_cnt, const int sum_cnt) {
  SketchConfig sketch_config;
  for (int i = 0; i < unique_cnt; ++i) {
    sketch_config.add_values()->set_aggregator(SketchConfig::ValueSpec::UNIQUE);
  }
  for (int i = 0; i < sum_cnt; ++i) {
    sketch_config.add_values()->set_aggregator(SketchConfig::ValueSpec::SUM);
  }
  return sketch_config;
}

// Helper function to add random registers to a sketch according to its
// sketchConfig.
void AddRandomRegisters(const int register_cnt, Sketch& sketch) {
  for (int i = 0; i < register_cnt; ++i) {
    Sketch::Register* last_register = sketch.add_registers();
    last_register->set_index(rand());
    for (int value_i = 0; value_i < sketch.config().values_size(); ++value_i) {
      // The Aggregate type doesn't matter here.
      // Mod(kMaxCounterValue * 2) so we have some but not too many values
      // exceed the max.
      last_register->add_values(rand() % (kMaxCounterValue * 2));
    }
  }
}

// Partition the char vector 33 by 33, and convert the results to strings
std::vector<std::string> GetCipherStrings(absl::string_view bytes) {
  EXPECT_EQ(bytes.size() % 66, 0);
  std::vector<std::string> result;
  int word_cnt = bytes.size() / 33;
  result.reserve(word_cnt);
  for (int i = 0; i < word_cnt; ++i) {
    result.emplace_back(bytes.substr(i * 33, 33));
  }
  return result;
}

// Add two ElGamal ciphertexts on the specified ECGroup.
CiphertextString AddCiphertext(const CiphertextString& a,
                               const CiphertextString& b,
                               const ECGroup& ec_group) {
  std::string sum_u = ec_group.CreateECPoint(a.u)
                          .value()
                          .Add(ec_group.CreateECPoint(b.u).value())
                          .value()
                          .ToBytesCompressed()
                          .value();
  std::string sum_e = ec_group.CreateECPoint(a.e)
                          .value()
                          .Add(ec_group.CreateECPoint(b.e).value())
                          .value()
                          .ToBytesCompressed()
                          .value();
  return {
      .u = sum_u,
      .e = sum_e,
  };
}

class SketchEncrypterTest : public ::testing::Test {
 protected:
  SketchEncrypterTest() {
    original_cipher_ =
        CommutativeElGamal::CreateWithNewKeyPair(kTestCurveId).value();
    auto public_key_pair = original_cipher_->GetPublicKeyBytes().value();
    CiphertextString public_key = {
        .u = public_key_pair.first,
        .e = public_key_pair.second,
    };
    sketch_encrypter_ =
        CreateWithPublicKey(kTestCurveId, kMaxCounterValue, public_key).value();
  }

  ~SketchEncrypterTest() override {}

  // The ElGamal Cipher whose public key is used to create the SketchEncrypter.
  std::unique_ptr<CommutativeElGamal> original_cipher_;
  // The SketchEncrypter used in this test.
  std::unique_ptr<SketchEncrypter> sketch_encrypter_;
};

TEST_F(SketchEncrypterTest, ByteSizeShouldBeCorrect) {
  const int unique_cnt = 2;
  const int sum_cnt = 3;
  const int register_size = 1000;
  Sketch plain_sketch;
  *plain_sketch.mutable_config() = CreateSketchConfig(unique_cnt, sum_cnt);
  AddRandomRegisters(register_size, plain_sketch);

  auto result = sketch_encrypter_->Encrypt(plain_sketch).value();

  EXPECT_THAT(result, SizeIs(register_size * (1 + unique_cnt + sum_cnt) * 66));
}

TEST_F(SketchEncrypterTest, EncryptionShouldBeNonDeterministic) {
  Sketch plain_sketch;
  *plain_sketch.mutable_config() =
      CreateSketchConfig(/* unique_cnt = */ 0, /* sum_cnt = */ 0);
  plain_sketch.add_registers()->set_index(1);
  plain_sketch.add_registers()->set_index(1);

  auto result = sketch_encrypter_->Encrypt(plain_sketch).value();
  std::vector<std::string> cipher_words = GetCipherStrings(result);
  EXPECT_THAT(cipher_words, SizeIs(4));  // 2 regs * 1 vals * 2 words

  CiphertextString cipher_index_1_a = {cipher_words[0], cipher_words[1]};
  CiphertextString cipher_index_1_b = {cipher_words[2], cipher_words[3]};
  // Multiple encryption of the same index value should be different.
  EXPECT_NE(cipher_index_1_a.u, cipher_index_1_b.u);
  EXPECT_NE(cipher_index_1_a.e, cipher_index_1_b.e);
  EXPECT_THAT(cipher_index_1_a,
              HasSameDecryption(original_cipher_.get(), cipher_index_1_b));
}

TEST_F(SketchEncrypterTest, EncryptionOfCountValueShouldBeAdditiveHomomorphic) {
  Context ctx;
  ECGroup ec_group = ECGroup::Create(kTestCurveId, &ctx).value();

  Sketch plain_sketch;
  *plain_sketch.mutable_config() = CreateSketchConfig(
      /* unique_cnt = */ 0, /* sum_cnt = */ 1);
  plain_sketch.add_registers()->add_values(1);
  plain_sketch.add_registers()->add_values(2);
  plain_sketch.add_registers()->add_values(3);
  plain_sketch.add_registers()->add_values(4);
  plain_sketch.add_registers()->add_values(5);

  auto result = sketch_encrypter_->Encrypt(plain_sketch).value();
  std::vector<std::string> cipher_words = GetCipherStrings(result);
  EXPECT_THAT(cipher_words, SizeIs(20));  // 5 regs * 2 vals * 2 words

  CiphertextString cipher_1 = {cipher_words[2], cipher_words[3]};
  CiphertextString cipher_2 = {cipher_words[6], cipher_words[7]};
  CiphertextString cipher_3 = {cipher_words[10], cipher_words[11]};
  CiphertextString cipher_4 = {cipher_words[14], cipher_words[15]};
  CiphertextString cipher_5 = {cipher_words[18], cipher_words[19]};

  CiphertextString cipher_1_add_4 = AddCiphertext(cipher_1, cipher_4, ec_group);
  CiphertextString cipher_2_add_3 = AddCiphertext(cipher_2, cipher_3, ec_group);

  EXPECT_THAT(cipher_5,
              HasSameDecryption(original_cipher_.get(), cipher_1_add_4));
  EXPECT_THAT(cipher_5,
              HasSameDecryption(original_cipher_.get(), cipher_2_add_3));
}

TEST_F(SketchEncrypterTest, MaximumCountValueShouldWork) {
  Context ctx;
  ECGroup ec_group = ECGroup::Create(kTestCurveId, &ctx).value();

  Sketch plain_sketch;
  *plain_sketch.mutable_config() =
      CreateSketchConfig(/* unique_cnt = */ 0, /* sum_cnt = */ 1);
  plain_sketch.add_registers()->add_values(kMaxCounterValue + 10);
  plain_sketch.add_registers()->add_values(kMaxCounterValue);

  auto result = sketch_encrypter_->Encrypt(plain_sketch).value();
  std::vector<std::string> cipher_words = GetCipherStrings(result);
  EXPECT_THAT(cipher_words, SizeIs(8));  // 2 regs * 2 vals * 2 words

  CiphertextString count_a = {cipher_words[2], cipher_words[3]};
  CiphertextString count_b = {cipher_words[6], cipher_words[7]};

  // Encryption of kMaxCounterValue +10 and kMaxCounterValue should be the same.
  EXPECT_THAT(count_a, HasSameDecryption(original_cipher_.get(), count_b));
}

TEST_F(SketchEncrypterTest, ZeroCountValueShouldHaveValidEncrpytion) {
  Sketch plain_sketch;
  *plain_sketch.mutable_config() =
      CreateSketchConfig(/* unique_cnt = */ 0, /* sum_cnt = */ 1);
  plain_sketch.add_registers()->add_values(0);

  auto result = sketch_encrypter_->Encrypt(plain_sketch).value();
  std::vector<std::string> cipher_words = GetCipherStrings(result);
  EXPECT_THAT(cipher_words, SizeIs(4));

  auto decryption = original_cipher_->Decrypt(
      std::make_pair(cipher_words[2], cipher_words[3]));
  // The decryption of 0 should return internal Error: POINT_AT_INFINITY.
  ASSERT_FALSE(decryption.status().ok());
  EXPECT_NE(decryption.status().message().find("POINT_AT_INFINITY"),
            std::string::npos);
}

}  // namespace
}  // namespace wfa::any_sketch::crypto