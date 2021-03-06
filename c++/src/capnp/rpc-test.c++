// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "rpc.h"
#include "test-util.h"
#include <kj/debug.h>
#include <gtest/gtest.h>
#include <map>
#include <queue>

namespace capnp {
namespace _ {  // private
namespace {

class TestNetworkAdapter;

class TestNetwork {
public:
  ~TestNetwork() noexcept(false);

  TestNetworkAdapter& add(kj::StringPtr name);

  kj::Maybe<const TestNetworkAdapter&> find(kj::StringPtr name) const {
    auto lock = map.lockShared();
    auto iter = lock->find(name);
    if (iter == lock->end()) {
      return nullptr;
    } else {
      return *iter->second;
    }
  }

private:
  kj::MutexGuarded<std::map<kj::StringPtr, kj::Own<TestNetworkAdapter>>> map;
};

typedef VatNetwork<
    test::TestSturdyRef, test::TestProvisionId, test::TestRecipientId, test::TestThirdPartyCapId,
    test::TestJoinAnswer> TestNetworkAdapterBase;

class TestNetworkAdapter final: public TestNetworkAdapterBase {
public:
  TestNetworkAdapter(const TestNetwork& network): network(network) {}

  typedef TestNetworkAdapterBase::Connection Connection;

  class ConnectionImpl final: public Connection, public kj::Refcounted {
  public:
    ConnectionImpl() {}

    void attach(ConnectionImpl& other) {
      KJ_REQUIRE(partner == nullptr);
      KJ_REQUIRE(other.partner == nullptr);
      partner = other;
      other.partner = *this;
    }

    class IncomingRpcMessageImpl final: public IncomingRpcMessage {
    public:
      IncomingRpcMessageImpl(uint firstSegmentWordSize)
          : message(firstSegmentWordSize == 0 ? SUGGESTED_FIRST_SEGMENT_WORDS
                                              : firstSegmentWordSize) {}

      ObjectPointer::Reader getBody() override {
        return message.getRoot<ObjectPointer>().asReader();
      }

      MallocMessageBuilder message;
    };

    class OutgoingRpcMessageImpl final: public OutgoingRpcMessage {
    public:
      OutgoingRpcMessageImpl(const ConnectionImpl& connection, uint firstSegmentWordSize)
          : connection(connection),
            message(kj::heap<IncomingRpcMessageImpl>(firstSegmentWordSize)) {}

      ObjectPointer::Builder getBody() override {
        return message->message.getRoot<ObjectPointer>();
      }
      void send() override {
        KJ_IF_MAYBE(p, connection.partner) {
          auto lock = p->queues.lockExclusive();
          if (lock->fulfillers.empty()) {
            lock->messages.push(kj::mv(message));
          } else {
            lock->fulfillers.front()->fulfill(kj::mv(message));
            lock->fulfillers.pop();
          }
        }
      }

    private:
      const ConnectionImpl& connection;
      kj::Own<IncomingRpcMessageImpl> message;
    };

    kj::Own<OutgoingRpcMessage> newOutgoingMessage(uint firstSegmentWordSize) const override {
      return kj::heap<OutgoingRpcMessageImpl>(*this, firstSegmentWordSize);
    }
    kj::Promise<kj::Own<IncomingRpcMessage>> receiveIncomingMessage() override {
      auto lock = queues.lockExclusive();
      if (lock->messages.empty()) {
        auto paf = kj::newPromiseAndFulfiller<kj::Own<IncomingRpcMessage>>();
        lock->fulfillers.push(kj::mv(paf.fulfiller));
        return kj::mv(paf.promise);
      } else {
        auto result = kj::mv(lock->messages.front());
        lock->messages.pop();
        return kj::mv(result);
      }
    }
    void introduceTo(Connection& recipient,
        test::TestThirdPartyCapId::Builder sendToRecipient,
        test::TestRecipientId::Builder sendToTarget) override {
      KJ_FAIL_ASSERT("not implemented");
    }
    ConnectionAndProvisionId connectToIntroduced(
        test::TestThirdPartyCapId::Reader capId) override {
      KJ_FAIL_ASSERT("not implemented");
    }
    kj::Own<Connection> acceptIntroducedConnection(
        test::TestRecipientId::Reader recipientId) override {
      KJ_FAIL_ASSERT("not implemented");
    }

  private:
    kj::Maybe<ConnectionImpl&> partner;

    struct Queues {
      std::queue<kj::Own<kj::PromiseFulfiller<kj::Own<IncomingRpcMessage>>>> fulfillers;
      std::queue<kj::Own<IncomingRpcMessage>> messages;
    };
    kj::MutexGuarded<Queues> queues;
  };

  kj::Own<Connection> connectToHostOf(test::TestSturdyRef::Reader ref) override {
    const TestNetworkAdapter& dst = KJ_REQUIRE_NONNULL(network.find(ref.getHost()));

    kj::Locked<State> myLock;
    kj::Locked<State> dstLock;

    if (&dst < this) {
      dstLock = dst.state.lockExclusive();
      myLock = state.lockExclusive();
    } else {
      myLock = state.lockExclusive();
      dstLock = dst.state.lockExclusive();
    }

    auto iter = myLock->connections.find(&dst);
    if (iter == myLock->connections.end()) {
      auto local = kj::refcounted<ConnectionImpl>();
      auto remote = kj::refcounted<ConnectionImpl>();
      local->attach(*remote);

      myLock->connections[&dst] = kj::addRef(*local);
      dstLock->connections[this] = kj::addRef(*remote);

      if (dstLock->fulfillerQueue.empty()) {
        dstLock->connectionQueue.push(kj::mv(remote));
      } else {
        dstLock->fulfillerQueue.front()->fulfill(kj::mv(remote));
        dstLock->fulfillerQueue.pop();
      }

      return kj::mv(local);
    } else {
      return kj::addRef(*iter->second);
    }
  }

  kj::Promise<kj::Own<Connection>> acceptConnectionAsRefHost() override {
    auto lock = state.lockExclusive();
    if (lock->connectionQueue.empty()) {
      auto paf = kj::newPromiseAndFulfiller<kj::Own<Connection>>();
      lock->fulfillerQueue.push(kj::mv(paf.fulfiller));
      return kj::mv(paf.promise);
    } else {
      auto result = kj::mv(lock->connectionQueue.front());
      lock->connectionQueue.pop();
      return kj::mv(result);
    }
  }

private:
  const TestNetwork& network;

  struct State {
    std::map<const TestNetworkAdapter*, kj::Own<ConnectionImpl>> connections;
    std::queue<kj::Own<kj::PromiseFulfiller<kj::Own<Connection>>>> fulfillerQueue;
    std::queue<kj::Own<Connection>> connectionQueue;
  };
  kj::MutexGuarded<State> state;
};

TestNetwork::~TestNetwork() noexcept(false) {}

TestNetworkAdapter& TestNetwork::add(kj::StringPtr name) {
  auto lock = map.lockExclusive();
  return *((*lock)[name] = kj::heap<TestNetworkAdapter>(*this));
}

// =======================================================================================

class TestInterfaceImpl final: public test::TestInterface::Server {
public:
  TestInterfaceImpl(int& callCount): callCount(callCount) {}

  int& callCount;

  ::kj::Promise<void> foo(
      test::TestInterface::FooParams::Reader params,
      test::TestInterface::FooResults::Builder result) override {
    ++callCount;
    EXPECT_EQ(123, params.getI());
    EXPECT_TRUE(params.getJ());
    result.setX("foo");
    return kj::READY_NOW;
  }

  ::kj::Promise<void> bazAdvanced(
      ::capnp::CallContext<test::TestInterface::BazParams,
                           test::TestInterface::BazResults> context) override {
    ++callCount;
    auto params = context.getParams();
    checkTestMessage(params.getS());
    context.releaseParams();
    EXPECT_ANY_THROW(context.getParams());

    return kj::READY_NOW;
  }
};

class TestExtendsImpl final: public test::TestExtends::Server {
public:
  TestExtendsImpl(int& callCount): callCount(callCount) {}

  int& callCount;

  ::kj::Promise<void> foo(
      test::TestInterface::FooParams::Reader params,
      test::TestInterface::FooResults::Builder result) override {
    ++callCount;
    EXPECT_EQ(321, params.getI());
    EXPECT_FALSE(params.getJ());
    result.setX("bar");
    return kj::READY_NOW;
  }

  ::kj::Promise<void> graultAdvanced(
      ::capnp::CallContext<test::TestExtends::GraultParams, test::TestAllTypes> context) override {
    ++callCount;
    context.releaseParams();

    initTestMessage(context.getResults());

    return kj::READY_NOW;
  }
};

class TestPipelineImpl final: public test::TestPipeline::Server {
public:
  TestPipelineImpl(int& callCount): callCount(callCount) {}

  int& callCount;

  ::kj::Promise<void> getCapAdvanced(
      capnp::CallContext<test::TestPipeline::GetCapParams,
                         test::TestPipeline::GetCapResults> context) override {
    ++callCount;

    auto params = context.getParams();
    EXPECT_EQ(234, params.getN());

    auto cap = params.getInCap();
    context.releaseParams();

    auto request = cap.fooRequest();
    request.setI(123);
    request.setJ(true);

    return request.send().then(
        [this,context](capnp::Response<test::TestInterface::FooResults>&& response) mutable {
          EXPECT_EQ("foo", response.getX());

          auto result = context.getResults();
          result.setS("bar");
          result.initOutBox().setCap(kj::heap<TestExtendsImpl>(callCount));
        });
  }
};

class TestRestorer final: public SturdyRefRestorer<test::TestSturdyRef> {
public:
  int callCount = 0;

  Capability::Client restore(test::TestSturdyRef::Reader ref) override {
    switch (ref.getTag()) {
      case test::TestSturdyRef::Tag::TEST_INTERFACE:
        return kj::heap<TestInterfaceImpl>(callCount);
      case test::TestSturdyRef::Tag::TEST_PIPELINE:
        return kj::heap<TestPipelineImpl>(callCount);
    }
    KJ_UNREACHABLE;
  }
};

class RpcTest: public testing::Test {
protected:
  TestNetwork network;
  TestRestorer restorer;
  kj::SimpleEventLoop loop;
  RpcSystem<test::TestSturdyRef> rpcClient;
  RpcSystem<test::TestSturdyRef> rpcServer;

  Capability::Client connect(test::TestSturdyRef::Tag tag) {
    MallocMessageBuilder refMessage(128);
    auto ref = refMessage.initRoot<test::TestSturdyRef>();
    ref.setHost("server");
    ref.setTag(tag);

    return rpcClient.connect(ref);
  }

  RpcTest()
      : rpcClient(makeRpcClient(network.add("client"), loop)),
        rpcServer(makeRpcServer(network.add("server"), restorer, loop)) {}

  ~RpcTest() noexcept {}
  // Need to declare this with explicit noexcept otherwise it conflicts with testing::Test::~Test.
  // (Urgh, C++11, why did you change this?)
};

TEST_F(RpcTest, Basic) {
  auto client = connect(test::TestSturdyRef::Tag::TEST_INTERFACE).castAs<test::TestInterface>();

  auto request1 = client.fooRequest();
  request1.setI(123);
  request1.setJ(true);
  auto promise1 = request1.send();

  auto request2 = client.bazRequest();
  initTestMessage(request2.initS());
  auto promise2 = request2.send();

  bool barFailed = false;
  auto request3 = client.barRequest();
  auto promise3 = loop.there(request3.send(),
      [](Response<test::TestInterface::BarResults>&& response) {
        ADD_FAILURE() << "Expected bar() call to fail.";
      }, [&](kj::Exception&& e) {
        barFailed = true;
      });

  EXPECT_EQ(0, restorer.callCount);

  auto response1 = loop.wait(kj::mv(promise1));

  EXPECT_EQ("foo", response1.getX());

  auto response2 = loop.wait(kj::mv(promise2));

  loop.wait(kj::mv(promise3));

  EXPECT_EQ(2, restorer.callCount);
  EXPECT_TRUE(barFailed);
}

TEST_F(RpcTest, Pipelining) {
  auto client = connect(test::TestSturdyRef::Tag::TEST_PIPELINE).castAs<test::TestPipeline>();

  int chainedCallCount = 0;

  auto request = client.getCapRequest();
  request.setN(234);
  request.setInCap(test::TestInterface::Client(
      kj::heap<TestInterfaceImpl>(chainedCallCount), loop));

  auto promise = request.send();

  auto pipelineRequest = promise.getOutBox().getCap().fooRequest();
  pipelineRequest.setI(321);
  auto pipelinePromise = pipelineRequest.send();

  auto pipelineRequest2 = promise.getOutBox().getCap().castAs<test::TestExtends>().graultRequest();
  auto pipelinePromise2 = pipelineRequest2.send();

  promise = nullptr;  // Just to be annoying, drop the original promise.

  EXPECT_EQ(0, restorer.callCount);
  EXPECT_EQ(0, chainedCallCount);

  auto response = loop.wait(kj::mv(pipelinePromise));
  EXPECT_EQ("bar", response.getX());

  auto response2 = loop.wait(kj::mv(pipelinePromise2));
  checkTestMessage(response2);

  EXPECT_EQ(3, restorer.callCount);
  EXPECT_EQ(1, chainedCallCount);
}

}  // namespace
}  // namespace _ (private)
}  // namespace capnp
