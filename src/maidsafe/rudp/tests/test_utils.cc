/*******************************************************************************
 *  Copyright 2012 MaidSafe.net limited                                        *
 *                                                                             *
 *  The following source code is property of MaidSafe.net limited and is not   *
 *  meant for external use.  The use of this code is governed by the licence   *
 *  file licence.txt found in the root of this directory and also on           *
 *  www.maidsafe.net.                                                          *
 *                                                                             *
 *  You are not free to copy, amend or otherwise use this source code without  *
 *  the explicit written permission of the board of directors of MaidSafe.net. *
 ******************************************************************************/

#include "maidsafe/rudp/tests/test_utils.h"

#include <thread>
#include <set>

#include "boost/lexical_cast.hpp"
#include "boost/date_time/posix_time/posix_time.hpp"

#include "maidsafe/common/log.h"

#include "maidsafe/rudp/managed_connections.h"
#include "maidsafe/rudp/parameters.h"
#include "maidsafe/rudp/return_codes.h"
#include "maidsafe/rudp/utils.h"

namespace bptime = boost::posix_time;


namespace maidsafe {

namespace rudp {

namespace test {

uint16_t GetRandomPort() {
  static std::set<uint16_t> already_used_ports;
  bool unique(false);
  uint16_t port(0);
  do {
    port = (RandomUint32() % 48126) + 1025;
    unique = (already_used_ports.insert(port)).second;
  } while (!unique);
  return port;
}

testing::AssertionResult SetupNetwork(std::vector<NodePtr> &nodes,
                                      std::vector<Endpoint> &bootstrap_endpoints,
                                      const int& node_count) {
  if (node_count < 2)
    return testing::AssertionFailure() << "Network size must be greater than 1";

  nodes.clear();
  bootstrap_endpoints.clear();
  for (int i(0); i != node_count; ++i) {
    nodes.push_back(std::make_shared<Node>(i));
    LOG(kInfo) << nodes[i]->id() << " has NodeId " << nodes[i]->debug_node_id();
  }

  // Setting up first two nodes
  EndpointPair endpoints0, endpoints1;
  endpoints0.local = Endpoint(detail::GetLocalIp(), GetRandomPort());
  endpoints1.local = Endpoint(detail::GetLocalIp(), GetRandomPort());
  NodeId chosen_node_id;

  boost::thread thread([&] {
    chosen_node_id =
        nodes[0]->Bootstrap(std::vector<Endpoint>(1, endpoints1.local), endpoints0.local);
  });
  NodeId node1_bootstrap_res(
      nodes[1]->Bootstrap(std::vector<Endpoint>(1, endpoints0.local), endpoints1.local));
  thread.join();

  //if (node1_bootstrap_res != nodes[0]->node_id())
  //  return testing::AssertionFailure() << "Bootstrapping failed for Node 1.  Result using "
  //                                     << endpoints0.local << " was "
  //                                     << DebugId(node1_bootstrap_res);
  //if (chosen_node_id != nodes[1]->node_id())
  //  return testing::AssertionFailure() << "Bootstrapping failed for Node 0.  Result using "
  //                                     << endpoints1.local << " was " << DebugId(chosen_node_id);

  auto futures0(nodes[0]->GetFutureForMessages(1));
  auto futures1(nodes[1]->GetFutureForMessages(1));
  LOG(kInfo) << "Calling Add from " << endpoints0.local << " to " << endpoints1.local;
  if (nodes[0]->managed_connections()->Add(nodes[1]->node_id(),
                                           endpoints1,
                                           nodes[0]->validation_data()) != kSuccess) {
    return testing::AssertionFailure() << "Node 0 failed to add Node 1";
  }
  nodes[0]->AddConnectedNodeId(nodes[1]->node_id());
  LOG(kInfo) << "Calling Add from " << endpoints1.local << " to " << endpoints0.local;
  if (nodes[1]->managed_connections()->Add(nodes[0]->node_id(),
                                           endpoints0,
                                           nodes[1]->validation_data()) != kSuccess) {
    return testing::AssertionFailure() << "Node 1 failed to add Node 0";
  }
  nodes[1]->AddConnectedNodeId(nodes[0]->node_id());

  if (!futures0.timed_wait(Parameters::rendezvous_connect_timeout)) {
    return testing::AssertionFailure() << "Failed waiting for " << nodes[0]->id()
        << " to receive " << nodes[1]->id() << "'s validation data.";
  }
  if (!futures1.timed_wait(Parameters::rendezvous_connect_timeout)) {
    return testing::AssertionFailure() << "Failed waiting for " << nodes[1]->id()
        << " to receive " << nodes[0]->id() << "'s validation data.";
  }
  auto messages0(futures0.get());
  auto messages1(futures1.get());
  if (messages0.size() != 1U) {
    return testing::AssertionFailure() << nodes[0]->id() << " has "
        << messages0.size() << " messages [should be 1].";
  }
  if (messages1.size() != 1U) {
    return testing::AssertionFailure() << nodes[1]->id() << " has "
        << messages1.size() << " messages [should be 1].";
  }
  if (messages0[0] != nodes[1]->validation_data()) {
    return testing::AssertionFailure() << nodes[0]->id() << " has received " << nodes[1]->id()
        << "'s validation data as " << messages0[0] << " [should be \""
        << nodes[1]->validation_data() << "\"].";
  }
  if (messages1[0] != nodes[0]->validation_data()) {
    return testing::AssertionFailure() << nodes[1]->id() << " has received " << nodes[0]->id()
        << "'s validation data as " << messages1[0] << " [should be \""
        << nodes[0]->validation_data() << "\"].";
  }

  bootstrap_endpoints.push_back(endpoints0.local);
  bootstrap_endpoints.push_back(endpoints1.local);
  nodes[0]->ResetData();
  nodes[1]->ResetData();

  LOG(kInfo) << "Setting up remaining " << (node_count - 2) << " nodes";

  // Adding nodes to each other
  for (int i(2); i != node_count; ++i) {
    NodeId chosen_node_id(nodes[i]->Bootstrap(bootstrap_endpoints));
    if (chosen_node_id != NodeId())
      return testing::AssertionFailure() << "Bootstrapping failed for " << nodes[i]->id();

    NatType nat_type;
    for (int j(0); j != i; ++j) {
      nodes[i]->ResetData();
      nodes[j]->ResetData();
      EndpointPair empty_endpoint_pair;
      //if (chosen_node_id == nodes[j]->node_id())
      //  peer_endpoints = chosen_endpoint;
      EndpointPair this_endpoint_pair, peer_endpoint_pair;
      int result(nodes[i]->managed_connections()->GetAvailableEndpoint(nodes[j]->node_id(),
                                                                       empty_endpoint_pair,
                                                                       this_endpoint_pair,
                                                                       nat_type));
      if (result != kSuccess) {
        return testing::AssertionFailure() << "GetAvailableEndpoint failed for "
                                           << nodes[i]->id() << " with result " << result
                                           << ".  Local: " << this_endpoint_pair.local
                                           << "  External: " << this_endpoint_pair.external;
      } else {
        LOG(kInfo) << "GetAvailableEndpoint on " << nodes[i]->id() << " to "
                   << nodes[j]->id() << " with peer_id " << nodes[j]->debug_node_id()
                   << " returned " << this_endpoint_pair.external << " / "
                   << this_endpoint_pair.local;
      }
      result = nodes[j]->managed_connections()->GetAvailableEndpoint(nodes[i]->node_id(),
                                                                     this_endpoint_pair,
                                                                     peer_endpoint_pair,
                                                                     nat_type);
      if (result != kSuccess) {
        return testing::AssertionFailure() << "GetAvailableEndpoint failed for "
                                           << nodes[j]->id() << " with result " << result
                                           << ".  Local: " << peer_endpoint_pair.local
                                           << "  External: " << peer_endpoint_pair.external
                                           << "  Peer: " << this_endpoint_pair.local;
      } else {
        LOG(kInfo) << "Calling GetAvailableEndpoint on " << nodes[j]->id() << " to "
                   << nodes[i]->id() << " with peer_endpoint " << this_endpoint_pair.local
                   << " returned " << peer_endpoint_pair.external << " / "
                   << peer_endpoint_pair.local;
      }

      futures0 = nodes[i]->GetFutureForMessages(1);
      futures1 = nodes[j]->GetFutureForMessages(1);

      LOG(kInfo) << "Calling Add from " << nodes[j]->id() << " on "
                 << peer_endpoint_pair.local << " to " << nodes[i]->id()
                 << " on " << this_endpoint_pair.local;
      result = nodes[j]->managed_connections()->Add(nodes[i]->node_id(),
                                                    this_endpoint_pair,
                                                    nodes[j]->validation_data());
      nodes[j]->AddConnectedNodeId(nodes[i]->node_id());
      if (result != kSuccess) {
        return testing::AssertionFailure() << "Add failed for " << nodes[j]->id()
                                           << " with result " << result;
      }

      LOG(kInfo) << "Calling Add from " << nodes[i]->id() << " on "
                 << this_endpoint_pair.local << " to " << nodes[j]->id()
                 << " on " << peer_endpoint_pair.local;
      result = nodes[i]->managed_connections()->Add(nodes[j]->node_id(),
                                                    peer_endpoint_pair,
                                                    nodes[i]->validation_data());
      nodes[i]->AddConnectedNodeId(nodes[j]->node_id());
      if (result != kSuccess) {
        return testing::AssertionFailure() << "Add failed for " << nodes[i]->id()
                                           << " with result " << result;
      }

      if (!futures0.timed_wait(Parameters::rendezvous_connect_timeout)) {
        return testing::AssertionFailure() << "Failed waiting for " << nodes[i]->id()
            << " to receive " << nodes[j]->id() << "'s validation data.";
      }
      if (!futures1.timed_wait(Parameters::rendezvous_connect_timeout)) {
        return testing::AssertionFailure() << "Failed waiting for " << nodes[j]->id()
            << " to receive " << nodes[i]->id() << "'s validation data.";
      }
      messages0 = futures0.get();
      messages1 = futures1.get();
      if (messages0.size() != 1U) {
        return testing::AssertionFailure() << nodes[i]->id() << " has "
            << messages0.size() << " messages [should be 1].";
      }
      if (messages1.size() != 1U) {
        return testing::AssertionFailure() << nodes[j]->id() << " has "
            << messages1.size() << " messages [should be 1].";
      }
      if (messages0[0] != nodes[j]->validation_data()) {
        return testing::AssertionFailure() << nodes[i]->id() << " has received " << nodes[j]->id()
            << "'s validation data as " << messages0[0] << " [should be \""
            << nodes[j]->validation_data() << "\"].";
      }
      if (messages1[0] != nodes[i]->validation_data()) {
        return testing::AssertionFailure() << nodes[j]->id() << " has received " << nodes[i]->id()
            << "'s validation data as " << messages1[0] << " [should be \""
            << nodes[i]->validation_data() << "\"].";
      }
      bootstrap_endpoints.push_back(this_endpoint_pair.local);
    }
  }
  return testing::AssertionSuccess();
}


Node::Node(int id)
    : node_id_(NodeId::kRandomId),
      key_pair_(),
      mutex_(),
      connection_lost_node_ids_(),
      connected_node_ids_(),
      messages_(),
      managed_connections_(new ManagedConnections),
      promised_(false),
      total_message_count_expectation_(0),
      message_promise_() {
  key_pair_.identity = "Node " + boost::lexical_cast<std::string>(id);
  asymm::GenerateKeyPair(&key_pair_);
  key_pair_.validation_token = key_pair_.identity + "'s validation data";
}

std::vector<NodeId> Node::connection_lost_node_ids() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return connection_lost_node_ids_;
}

std::vector<std::string> Node::messages() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return messages_;
}

NodeId Node::Bootstrap(const std::vector<Endpoint> &bootstrap_endpoints,
                       Endpoint local_endpoint) {
  NatType nat_type(NatType::kUnknown);
  return managed_connections_->Bootstrap(
      bootstrap_endpoints,
      false,
      [&](const std::string& message) {
        bool is_printable(true);
        for (auto itr(message.begin()); itr != message.end(); ++itr) {
          if (*itr < 32) {
            is_printable = false;
            break;
          }
        }
        LOG(kInfo) << id() << " -- Received: " << (is_printable ? message.substr(0, 30) :
                                                   EncodeToHex(message.substr(0, 15)));
        std::lock_guard<std::mutex> guard(mutex_);
        messages_.emplace_back(message);
        SetPromiseIfDone();
      },
      [&](const NodeId& peer_id) {
        LOG(kInfo) << id() << " -- Lost connection to " << DebugId(node_id_);
        std::lock_guard<std::mutex> guard(mutex_);
        connection_lost_node_ids_.emplace_back(peer_id);
        connected_node_ids_.erase(std::remove(connected_node_ids_.begin(),
                                              connected_node_ids_.end(),
                                              peer_id),
                                  connected_node_ids_.end());
      },
      node_id_,
      private_key(),
      public_key(),
      nat_type,
      local_endpoint);
}

int Node::GetReceivedMessageCount(const std::string& message) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return static_cast<int>(std::count(messages_.begin(), messages_.end(), message));
}

void Node::ResetData() {
  std::lock_guard<std::mutex> guard(mutex_);
  connection_lost_node_ids_.clear();
  messages_.clear();
  total_message_count_expectation_ = 0;
}

boost::unique_future<std::vector<std::string>> Node::GetFutureForMessages(
    const uint32_t& message_count) {
  BOOST_ASSERT(message_count > 0);
  total_message_count_expectation_ = message_count;
  promised_ = true;
  boost::promise<std::vector<std::string>> message_promise;
  message_promise_.swap(message_promise);
  return message_promise_.get_future();
}

void Node::SetPromiseIfDone() {
  if (promised_ && messages_.size() >= total_message_count_expectation_) {
    message_promise_.set_value(messages_);
    promised_ = false;
    total_message_count_expectation_ = 0;
  }
}


}  // namespace test

}  // namespace rudp

}  // namespace maidsafe