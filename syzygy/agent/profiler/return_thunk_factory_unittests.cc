// Copyright 2012 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/agent/profiler/return_thunk_factory.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using testing::_;
using call_trace::client::ReturnThunkFactory;
using testing::StrictMock;

class MockDelegate : public ReturnThunkFactory::Delegate {
 public:
  MOCK_METHOD2(OnFunctionExit, void(const ReturnThunkFactory::Thunk*, uint64));
};

// Version of ReturnThunkFactory that exposes various private bits for testing.
class TestFactory : public ReturnThunkFactory {
 public:
  explicit TestFactory(Delegate* delegate) : ReturnThunkFactory(delegate) {
  }

  using ReturnThunkFactory::MakeThunk;
  using ReturnThunkFactory::PageFromThunk;
  using ReturnThunkFactory::ThunkMain;
  using ReturnThunkFactory::kNumThunksPerPage;
};

class ReturnThunkTest : public testing::Test {
 public:
  void SetUp() {
    factory_ = new TestFactory(&delegate_);
  }

  void TearDown() {
    delete factory_;
  }

  static RetAddr WINAPI StaticMakeHook(RetAddr real_ret) {
    return factory_->MakeThunk(real_ret);
  }

 protected:
  StrictMock<MockDelegate> delegate_;

  // Valid during tests.
  static TestFactory* factory_;
};

TestFactory* ReturnThunkTest::factory_ = NULL;

// This assembly function indirectly calls ReturnThunkFactory::MakeThunk
// and switches its return address with the returned thunk.
extern "C" void __declspec(naked) create_and_return_via_thunk() {
  __asm {
    // Stash volatile registers.
    push eax
    push ecx
    push edx
    pushfd

    // Push the real return address, get the thunk, and replace
    // the return address on stack with the thunk.
    push DWORD PTR[esp + 0x10]
    call ReturnThunkTest::StaticMakeHook
    xchg eax, DWORD PTR[esp + 0x10]

    // Restore volatile registers.
    popfd
    pop edx
    pop ecx
    pop eax

    // Return to the thunk.
    ret
  }
}

// Each of the tests call a corresponding DoXyz method on the ReturnThunkTest
// object.  This is so that the actual test code is friends with
// ReturnThunkFactory.
TEST_F(ReturnThunkTest, AllocateSeveralPages) {
  ReturnThunkFactory::Thunk* previous_thunk = NULL;
  for (size_t i = 0; i < 3 * TestFactory::kNumThunksPerPage; ++i) {
    ReturnThunkFactory::Thunk* thunk = factory_->MakeThunk(NULL);
    ASSERT_TRUE(thunk);
    ASSERT_TRUE(
        (TestFactory::PageFromThunk(thunk) !=
         TestFactory::PageFromThunk(previous_thunk)) ||
        (thunk > previous_thunk));
    previous_thunk = thunk;
  }
}

TEST_F(ReturnThunkTest, ReturnViaThunk) {
  EXPECT_CALL(delegate_, OnFunctionExit(_, _));
  create_and_return_via_thunk();
}

TEST_F(ReturnThunkTest, ReuseThunks) {
  ReturnThunkFactory::Thunk* first_thunk = factory_->MakeThunk(NULL);
  factory_->MakeThunk(NULL);
  ReturnThunkFactory::Thunk* third_thunk = factory_->MakeThunk(NULL);

  // This simulates a return via the first thunk.
  EXPECT_CALL(delegate_, OnFunctionExit(_, _));
  TestFactory::ThunkMain(first_thunk, 0LL);

  factory_->MakeThunk(NULL);
  factory_->MakeThunk(NULL);
  ReturnThunkFactory::Thunk* new_third_thunk = factory_->MakeThunk(NULL);
  ASSERT_EQ(third_thunk, new_third_thunk);
}

TEST_F(ReturnThunkTest, ReusePages) {
  ReturnThunkFactory::Thunk* first_thunk = factory_->MakeThunk(NULL);
  ReturnThunkFactory::Thunk* last_thunk = NULL;
  for (size_t i = 0; i < TestFactory::kNumThunksPerPage; ++i) {
    last_thunk = factory_->MakeThunk(NULL);
  }

  // last_thunk should be the first thunk of the next page.
  ASSERT_NE(TestFactory::PageFromThunk(first_thunk),
            TestFactory::PageFromThunk(last_thunk));

  // This simulates a return via the first thunk, after which
  // we need to make kNumThunksPerPage + 1 thunks to again get
  // to the first thunk of the second page.
  EXPECT_CALL(delegate_, OnFunctionExit(_, _));
  TestFactory::ThunkMain(first_thunk, 0LL);

  ReturnThunkFactory::Thunk* new_last_thunk = NULL;
  for (size_t i = 0; i < TestFactory::kNumThunksPerPage + 1; ++i) {
    new_last_thunk = factory_->MakeThunk(NULL);
  }

  // We should reuse the previously-allocated second page.
  ASSERT_EQ(last_thunk, new_last_thunk);
}

}  // namespace