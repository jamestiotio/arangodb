////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Julia Volmer
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>
#include <unordered_set>
#include "fmt/format.h"
#include "velocypack/SharedSlice.h"
#include "VelocypackUtils/VelocyPackStringLiteral.h"

#include "Actor/DistributedActorPID.h"
#include "Actor/DistributedRuntime.h"

#include "Actors/FinishingActor.h"
#include "Actors/SpawnActor.h"
#include "Actors/TrivialActor.h"
#include "Actors/PingPongActors.h"
#include "MockScheduler.h"
#include "ThreadPoolScheduler.h"

using namespace arangodb::actor;
using namespace arangodb::actor::test;

struct EmptyExternalDispatcher : IExternalDispatcher {
  void dispatch(DistributedActorPID sender, DistributedActorPID receiver,
                arangodb::velocypack::SharedSlice msg) override {}
};

template<typename T>
class DistributedRuntimeTest : public testing::Test {
 public:
  DistributedRuntimeTest() : scheduler{std::make_shared<T>()} {
    scheduler->start(number_of_threads);
  }

  std::shared_ptr<T> scheduler;
  size_t number_of_threads = 128;
};
using SchedulerTypes = ::testing::Types<MockScheduler, ThreadPoolScheduler>;
TYPED_TEST_SUITE(DistributedRuntimeTest, SchedulerTypes);

TYPED_TEST(DistributedRuntimeTest, formats_runtime_and_actor_state) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      ServerID{"PRMR-1234"}, "RuntimeTest", this->scheduler, dispatcher);
  auto actorID = runtime->template spawn<pong_actor::Actor>(
      std::make_unique<pong_actor::PongState>(), pong_actor::message::Start{});

  this->scheduler->stop();
  ASSERT_EQ(
      fmt::format(
          "{}", arangodb::inspection::json(
                    *runtime, arangodb::inspection::JsonPrintFormat::kMinimal)),
      R"({"myServerID":"PRMR-1234","runtimeID":"RuntimeTest","uniqueActorIDCounter":2,"actors":[{"id":1,"type":"PongActor"}]})");
  auto actor =
      runtime->template getActorStateByID<pong_actor::Actor>(actorID).value();
  ASSERT_EQ(fmt::format("{}", actor), R"({"called":1})");
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest,
           serializes_an_actor_including_its_actor_state) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      ServerID{"PRMR-1234"}, "RuntimeTest", this->scheduler, dispatcher);
  auto actor = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("foo"), test::message::TrivialStart());

  this->scheduler->stop();
  using namespace arangodb::velocypack;
  auto expected =
      R"({"pid":{"server":"PRMR-1234","database":"database","id":1},"state":{"state":"foo","called":1},"batchsize":16})"_vpack;
  ASSERT_EQ(runtime->getSerializedActorByID(actor.id)->toJson(),
            expected.toJson());
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest, spawns_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);

  auto actor = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("foo"), test::message::TrivialStart());

  this->scheduler->stop();
  auto state = runtime->template getActorStateByID<TrivialActor>(actor);
  ASSERT_EQ(state, (TrivialState("foo", 1)));
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest, sends_initial_message_when_spawning_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);

  auto actor = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("foo"),
      test::message::TrivialMessage("bar"));

  this->scheduler->stop();
  auto state = runtime->template getActorStateByID<TrivialActor>(actor);
  ASSERT_EQ(state, (TrivialState("foobar", 1)));
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest, gives_all_existing_actor_ids) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);

  ASSERT_TRUE(runtime->getActorIDs().empty());

  auto actor_foo = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("foo"), test::message::TrivialStart());
  auto actor_bar = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("bar"), test::message::TrivialStart());

  this->scheduler->stop();
  auto allActorIDs = runtime->getActorIDs();
  ASSERT_EQ(allActorIDs.size(), 2);
  ASSERT_EQ(
      (std::unordered_set<ActorID>(allActorIDs.begin(), allActorIDs.end())),
      (std::unordered_set<ActorID>{actor_foo.id, actor_bar.id}));
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest, sends_message_to_an_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);
  auto actor = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("foo"), test::message::TrivialStart{});

  runtime->dispatch(
      actor, actor,
      TrivialActor::Message{test::message::TrivialMessage("baz")});

  this->scheduler->stop();
  auto state = runtime->template getActorStateByID<TrivialActor>(actor);
  ASSERT_EQ(state, (TrivialState("foobaz", 2)));
  runtime->softShutdown();
}

struct SomeMessage {};
template<typename Inspector>
auto inspect(Inspector& f, SomeMessage& x) {
  return f.object(x).fields();
}
struct SomeMessages : std::variant<SomeMessage> {
  using std::variant<SomeMessage>::variant;
};
template<typename Inspector>
auto inspect(Inspector& f, SomeMessages& x) {
  return f.variant(x).unqualified().alternatives(
      arangodb::inspection::type<SomeMessage>("someMessage"));
}
TYPED_TEST(
    DistributedRuntimeTest,
    actor_receiving_wrong_message_type_sends_back_unknown_error_message) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);
  auto actor = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("foo"), test::message::TrivialStart{});

  runtime->dispatch(actor, actor, SomeMessages{SomeMessage{}});

  this->scheduler->stop();
  ASSERT_EQ(
      runtime->template getActorStateByID<TrivialActor>(actor),
      (TrivialState(fmt::format("sent unknown message to {}", actor), 2)));
  runtime->softShutdown();
}

TYPED_TEST(
    DistributedRuntimeTest,
    actor_receives_actor_not_found_message_after_trying_to_send_message_to_non_existent_actor) {
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      "PRMR-1234", "RuntimeTest", this->scheduler, dispatcher);
  auto actor = runtime->template spawn<TrivialActor>(
      std::make_unique<TrivialState>("foo"), test::message::TrivialStart{});

  auto unknown_actor = DistributedActorPID{
      .server = "PRMR-1234", .database = "database", .id = {999}};
  runtime->dispatch(
      actor, unknown_actor,
      TrivialActor::Message{test::message::TrivialMessage{"baz"}});

  this->scheduler->stop();
  ASSERT_EQ(
      runtime->template getActorStateByID<TrivialActor>(actor),
      (TrivialState(fmt::format("receiving actor {} not found", unknown_actor),
                    2)));
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest, ping_pong_game) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto pong_actor = runtime->template spawn<pong_actor::Actor>(
      std::make_unique<pong_actor::PongState>(), pong_actor::message::Start{});
  auto ping_actor = runtime->template spawn<ping_actor::Actor>(
      std::make_unique<ping_actor::PingState>(),
      ping_actor::message::Start{.pongActor = pong_actor});

  this->scheduler->stop();
  auto ping_actor_state =
      runtime->template getActorStateByID<ping_actor::Actor>(ping_actor);
  ASSERT_EQ(ping_actor_state,
            (ping_actor::PingState{.called = 2, .message = "hello world"}));
  auto pong_actor_state =
      runtime->template getActorStateByID<pong_actor::Actor>(pong_actor);
  ASSERT_EQ(pong_actor_state, (pong_actor::PongState{.called = 2}));
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest, spawn_game) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto spawn_actor = runtime->template spawn<SpawnActor>(
      std::make_unique<SpawnState>(), test::message::SpawnStartMessage{});

  runtime->dispatch(spawn_actor, spawn_actor,
                    SpawnActor::Message{test::message::SpawnMessage{"baz"}});

  this->scheduler->stop();
  ASSERT_EQ(runtime->getActorIDs().size(), 2);
  ASSERT_EQ(runtime->template getActorStateByID<SpawnActor>(spawn_actor),
            (SpawnState{.called = 2, .state = "baz"}));
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest, finishes_actor_when_actor_says_so) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto finishing_actor = runtime->template spawn<FinishingActor>(
      std::make_unique<FinishingState>(), test::message::FinishingStart{});

  runtime->dispatch(finishing_actor, finishing_actor,
                    FinishingActor::Message{test::message::FinishingFinish{}});

  this->scheduler->stop();
  ASSERT_FALSE(runtime->actors.find(finishing_actor.id).has_value());
  runtime->softShutdown();
}

TYPED_TEST(DistributedRuntimeTest,
           finished_actor_automatically_removes_itself) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);
  auto finishing_actor = runtime->template spawn<FinishingActor>(
      std::make_unique<FinishingState>(), test::message::FinishingStart{});

  runtime->dispatch(finishing_actor, finishing_actor,
                    FinishingActor::Message{test::message::FinishingFinish{}});
  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }

  this->scheduler->stop();
  ASSERT_EQ(runtime->actors.size(), 0);
}

TYPED_TEST(DistributedRuntimeTest,
           finished_actors_automatically_remove_themselves) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);

  auto actor_to_be_finished = runtime->template spawn<FinishingActor>(
      std::make_unique<FinishingState>(), test::message::FinishingStart{});
  runtime->template spawn<FinishingActor>(std::make_unique<FinishingState>(),
                                          test::message::FinishingStart{});
  runtime->template spawn<FinishingActor>(std::make_unique<FinishingState>(),
                                          test::message::FinishingStart{});
  auto another_actor_to_be_finished = runtime->template spawn<FinishingActor>(
      std::make_unique<FinishingState>(), test::message::FinishingStart{});
  runtime->template spawn<FinishingActor>(std::make_unique<FinishingState>(),
                                          test::message::FinishingStart{});

  runtime->dispatch(actor_to_be_finished, actor_to_be_finished,
                    FinishingActor::Message{test::message::FinishingFinish{}});
  runtime->dispatch(another_actor_to_be_finished, another_actor_to_be_finished,
                    FinishingActor::Message{test::message::FinishingFinish{}});
  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }

  this->scheduler->stop();
  ASSERT_EQ(runtime->actors.size(), 3);
  auto remaining_actor_ids = runtime->getActorIDs();
  std::unordered_set<ActorID> actor_ids(remaining_actor_ids.begin(),
                                        remaining_actor_ids.end());
  ASSERT_FALSE(actor_ids.contains(actor_to_be_finished.id));
  ASSERT_FALSE(actor_ids.contains(another_actor_to_be_finished.id));
  runtime->softShutdown();
  ASSERT_EQ(runtime->actors.size(), 0);
}

TYPED_TEST(DistributedRuntimeTest,
           finishes_and_garbage_collects_all_actors_when_shutting_down) {
  auto serverID = ServerID{"PRMR-1234"};
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(
      serverID, "RuntimeTest", this->scheduler, dispatcher);
  runtime->template spawn<TrivialActor>(std::make_unique<TrivialState>(),
                                        test::message::TrivialStart{});
  runtime->template spawn<TrivialActor>(std::make_unique<TrivialState>(),
                                        test::message::TrivialStart{});
  runtime->template spawn<TrivialActor>(std::make_unique<TrivialState>(),
                                        test::message::TrivialStart{});
  runtime->template spawn<TrivialActor>(std::make_unique<TrivialState>(),
                                        test::message::TrivialStart{});
  runtime->template spawn<TrivialActor>(std::make_unique<TrivialState>(),
                                        test::message::TrivialStart{});
  ASSERT_EQ(runtime->actors.size(), 5);
  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }
  this->scheduler->stop();
  runtime->softShutdown();
  ASSERT_EQ(runtime->actors.size(), 0);
}

TEST(DistributedRuntimeTest, sends_messages_between_lots_of_actors) {
  auto serverID = ServerID{"PRMR-1234"};
  auto scheduler = std::make_shared<ThreadPoolScheduler>();
  auto dispatcher = std::make_shared<EmptyExternalDispatcher>();
  auto runtime = std::make_shared<DistributedRuntime>(serverID, "RuntimeTest",
                                                      scheduler, dispatcher);
  scheduler->start(128);
  size_t actor_count = 128;

  for (size_t i = 0; i < actor_count; i++) {
    runtime->template spawn<TrivialActor>(std::make_unique<TrivialState>(),
                                          test::message::TrivialStart{});
  }

  // send from actor i to actor i+1 a message with content i
  for (size_t i = 1; i < actor_count; i++) {
    runtime->dispatch(
        DistributedActorPID{.server = serverID,
                            .database = "database",
                            .id = ActorID{(i + 1) % actor_count}},
        DistributedActorPID{
            .server = serverID, .database = "database", .id = ActorID{i}},
        TrivialActor::Message{
            test::message::TrivialMessage{std::to_string(i)}});
  }
  // send from actor actor_count to actor 1 (jump over special actor id 0)
  runtime->dispatch(
      DistributedActorPID{
          .server = serverID, .database = "database", .id = ActorID{1}},
      DistributedActorPID{.server = serverID,
                          .database = "database",
                          .id = ActorID{actor_count}},
      TrivialActor::Message{
          test::message::TrivialMessage{std::to_string(actor_count)}});

  // wait for actor to work off all messages
  while (not runtime->areAllActorsIdle()) {
  }

  scheduler->stop();
  ASSERT_EQ(runtime->actors.size(), actor_count);
  for (size_t i = 1; i < actor_count; i++) {
    ASSERT_EQ(runtime->template getActorStateByID<TrivialActor>(ActorID{i}),
              (TrivialState(std::to_string(i), 2)));
  }
  ASSERT_EQ(
      runtime->template getActorStateByID<TrivialActor>(ActorID{actor_count}),
      (TrivialState(std::to_string(actor_count), 2)));
  runtime->softShutdown();
}
