////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "ClusterTrxMethods.h"

#include "Basics/NumberUtils.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterComm.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/FollowerInfo.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Utils/OperationOptions.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Buffer.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
  
namespace {

  // Timeout for read operations:
  static double const CL_DEFAULT_TIMEOUT = 120.0;
  
  
void buildTransactionBody(TransactionState& state, ServerID const& server,
                          VPackBuilder& builder) {
  // std::vector<ServerID> DBservers = ci->getCurrentDBServers();
  builder.openObject();
  state.options().toVelocyPack(builder);
  builder.add("collections", VPackValue(VPackValueType::Object));
  auto addCollections = [&](std::string const& key, AccessMode::Type t) {
    builder.add(key, VPackValue(VPackValueType::Array));
    state.allCollections([&](TransactionCollection& col) {
      if (col.accessType() == t) {
        if (state.isCoordinator()) {
          std::shared_ptr<ShardMap> shardMap = col.collection()->shardIds();
          // coordinator starts transaction on shard leader
          for (auto const& pair : *shardMap) {
            TRI_ASSERT(!pair.second.empty());
            // only add shard where server is leader
            if (!pair.second.empty() && pair.second[0] == server) {
              builder.add(VPackValue(pair.first));
            }
          }
        } else {
          if (col.collection()->followers()->contains(server)) {
            builder.add(VPackValue(col.collectionName()));
          }
        }
      }
      return true;
    });
    builder.close();
  };
  addCollections("read", AccessMode::Type::READ);
  addCollections("write", AccessMode::Type::WRITE);
  addCollections("exclusive", AccessMode::Type::EXCLUSIVE);
  
  builder.close();  // </collections>
  builder.close();  // </openObject>
}
  
/// @brief lazy begin a transaction on subordinate servers
ClusterCommRequest beginTransactionRequest(transaction::Methods const* trx,
                                           TransactionState& state,
                                           ServerID const& server) {
  TRI_voc_tid_t tid = state.id() + 1;
  TRI_ASSERT(!transaction::isLegacyTransactionId(tid));
  
  VPackBuilder builder;
  buildTransactionBody(state, server, builder);
  
  const std::string url = "/_db/" + StringUtils::urlEncode(state.vocbase().name()) +
  "/_api/transaction/begin";
  auto headers = std::make_unique<std::unordered_map<std::string, std::string>>();
  headers->emplace(StaticStrings::ContentTypeHeader, StaticStrings::MimeTypeJson);
  headers->emplace(StaticStrings::TransactionId, std::to_string(tid));
  auto body = std::make_shared<std::string>(builder.slice().toJson());
  return ClusterCommRequest("server:" + server, RequestType::POST, url, body,
                            std::move(headers));
}

Result checkTransactionResult(TransactionState const& state,
                              transaction::Status desiredStatus,
                              ClusterCommRequest const& request) {
  Result result;
  
  ClusterCommResult const& res = request.result;
  
  int commError = ::handleGeneralCommErrors(&res);
  if (commError != TRI_ERROR_NO_ERROR) {
    // oh-oh cluster is in a bad state
    return result.reset(commError);
  }
  TRI_ASSERT(res.status == CL_COMM_RECEIVED);
  
  VPackSlice answer = res.answer->payload();
  if ((res.answer_code == rest::ResponseCode::OK ||
       res.answer_code == rest::ResponseCode::CREATED) && answer.isObject()) {
    
    VPackSlice idSlice = answer.get(std::vector<std::string>{"result", "id"});
    VPackSlice statusSlice = answer.get(std::vector<std::string>{"result", "status"});

    if (!idSlice.isString() || !statusSlice.isString()) {
      return result.reset(TRI_ERROR_TRANSACTION_INTERNAL, "transaction has wrong format");
    }
    TRI_voc_tid_t tid = StringUtils::uint64(idSlice.copyString());
    VPackValueLength len = 0;
    const char* str = statusSlice.getStringUnchecked(len);
    if (tid == state.id() + 1 &&
        transaction::statusFromString(str, len) == desiredStatus) {
      return result;  // success
    }
  } else if (answer.isObject()) {
    return result.reset(VelocyPackHelper::readNumericValue(answer, StaticStrings::ErrorNum,
                                                     TRI_ERROR_TRANSACTION_INTERNAL),
                  VelocyPackHelper::getStringValue(answer, StaticStrings::ErrorMessage, ""));
  }
  LOG_TOPIC(DEBUG, Logger::TRANSACTIONS) << " failed to begin transaction on " << res.endpoint;

  return result.reset(TRI_ERROR_TRANSACTION_INTERNAL);  // unspecified error
}
  
Result commitAbortTransaction(transaction::Methods& trx, transaction::Status status) {
  Result res;
  arangodb::TransactionState& state = *trx.state();
  TRI_ASSERT(state.isRunning());
  
  if (state.knownServers().empty()) {
    return res;
  }
  
  // only commit managed transactions, and AQL leader transactions (on DBServers)
  if ((!state.hasHint(transaction::Hints::Hint::GLOBAL_MANAGED) &&
       !state.hasHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL)) ||
      (state.isCoordinator() && state.hasHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL))) {
    return res;
  }
  TRI_ASSERT(!state.isDBServer() || !transaction::isFollowerTransactionId(state.id()));
  
  auto cc = ClusterComm::instance();
  if (cc == nullptr) {
    // nullptr happens only during controlled shutdown
    return TRI_ERROR_SHUTTING_DOWN;
  }
  // std::vector<ServerID> DBservers = ci->getCurrentDBServers();
  const std::string url = "/_db/" + StringUtils::urlEncode(state.vocbase().name()) +
  "/_api/transaction/" + std::to_string(state.id() + 1);
  
  RequestType rtype;
  if (status == transaction::Status::COMMITTED) {
    rtype = RequestType::PUT;
  } else if (status == transaction::Status::ABORTED) {
    rtype = RequestType::DELETE_REQ;
  } else {
    TRI_ASSERT(false);
  }
  
  std::shared_ptr<std::string> body;
  std::vector<ClusterCommRequest> requests;
  for (std::string const& server : state.knownServers()) {
    LOG_TOPIC(DEBUG, Logger::TRANSACTIONS) << transaction::statusString(status)
      << " on " << server;
    requests.emplace_back("server:" + server, rtype, url, body);
  }
  
  // Perform the requests
  size_t nrDone = 0;
  cc->performRequests(requests, ::CL_DEFAULT_TIMEOUT, nrDone, Logger::COMMUNICATION, false);
  
  if (state.isCoordinator()) {
    TRI_ASSERT(transaction::isCoordinatorTransactionId(state.id()));
    
    for (size_t i = 0; i < requests.size(); ++i) {
      Result res = ::checkTransactionResult(state, status, requests[i]);
      if (res.fail()) {
        return res;
      }
    }
    
    return res;
  } else {
    TRI_ASSERT(state.isDBServer());
    TRI_ASSERT(transaction::isLeaderTransactionId(state.id()));
    
    // Drop all followers that were not successful:
    for (size_t i = 0; i < requests.size(); ++i) {
      Result res = ::checkTransactionResult(state, status, requests[i]);
      if (res.fail()) {  // remove follower from all collections
        ServerID const& follower = requests[i].result.serverID;
        state.allCollections([&](TransactionCollection& tc) {
          auto cc = tc.collection();
          if (cc) {
            if (cc->followers()->remove(follower)) {
              // TODO: what happens if a server is re-added during a transaction ?
              LOG_TOPIC(WARN, Logger::REPLICATION)
              << "synchronous replication: dropping follower " << follower
              << " for shard " << tc.collectionName();
            } else {
              LOG_TOPIC(ERR, Logger::REPLICATION)
              << "synchronous replication: could not drop follower "
              << follower << " for shard " << tc.collectionName();
              res.reset(TRI_ERROR_CLUSTER_COULD_NOT_DROP_FOLLOWER);
              return false; // cancel transaction
            }
          }
          return true;
        });
      }
    }
    
    return res; // succeed even if some followers did not commit
  }
}
}  // namespace

namespace arangodb {
  
/// @brief begin a transaction on all leaders
arangodb::Result ClusterTrxMethods::beginTransactionOnLeaders(TransactionState& state,
                                                           std::vector<ServerID> const& leaders) {
  TRI_ASSERT(state.isCoordinator());
  TRI_ASSERT(!state.hasHint(transaction::Hints::Hint::SINGLE_OPERATION));
  Result res;
  if (leaders.empty()) {
    return res;
  }

  std::vector<ClusterCommRequest> requests;
  for (ServerID const& leader : leaders) {
    if (state.knowsServer(leader)) {
      continue;  // already send a begin transaction there
    }
    state.addKnownServer(leader);
    
    LOG_DEVEL << "Begin transaction " << state.id() << " on " << leader;
    requests.emplace_back(::beginTransactionRequest(nullptr, state, leader));
  }

  if (requests.empty()) {
    return res;
  }

  auto cc = ClusterComm::instance();
  if (cc == nullptr) {
    // nullptr happens only during controlled shutdown
    return res.reset(TRI_ERROR_SHUTTING_DOWN);
  }

  // Perform the requests
  size_t nrDone = 0;
  cc->performRequests(requests, ::CL_DEFAULT_TIMEOUT, nrDone, Logger::COMMUNICATION, false);
  
  for (size_t i = 0; i < requests.size(); ++i) {
    res = ::checkTransactionResult(state, transaction::Status::RUNNING, requests[i]);
    if (res.fail()) {  // remove follower from all collections
      return res;
    }
  }
  return res;  // all good
}

/// @brief begin a transaction on all followers
Result ClusterTrxMethods::beginTransactionOnFollowers(transaction::Methods& trx,
                                                   arangodb::FollowerInfo& info,
                                                   std::vector<ServerID> const& followers) {
  TransactionState& state = *trx.state();
  TRI_ASSERT(state.isDBServer());
  TRI_ASSERT(!state.hasHint(transaction::Hints::Hint::SINGLE_OPERATION));
  TRI_ASSERT(transaction::isLeaderTransactionId(state.id()));

  // prepare the requests:
  std::vector<ClusterCommRequest> requests;
  for (ServerID const& follower : followers) {
    if (state.knowsServer(follower)) {
      continue;  // already send a begin transaction there
    }
    state.addKnownServer(follower);
    requests.emplace_back(::beginTransactionRequest(&trx, state, follower));
  }

  if (requests.empty()) {
    return TRI_ERROR_NO_ERROR;
  }

  auto cc = ClusterComm::instance();
  if (cc == nullptr) {
    // nullptr happens only during controlled shutdown
    return TRI_ERROR_SHUTTING_DOWN;
  }

  // Perform the requests
  size_t nrDone = 0;
  size_t nrGood = cc->performRequests(requests, ::CL_DEFAULT_TIMEOUT, nrDone,
                                      Logger::COMMUNICATION, false);

  if (nrGood < followers.size()) {
    // Otherwise we drop all followers that were not successful:
    for (size_t i = 0; i < followers.size(); ++i) {
      Result res = ::checkTransactionResult(state, transaction::Status::RUNNING, requests[i]);
      if (res.fail()) {
        LOG_DEVEL << "dropping follower because it did not start trx " << state.id()
                  << ", error: '" << res.errorMessage() << "'";
        info.remove(followers[i]);
      }
    }
  }
  // FIXME dropping followers is not
  return TRI_ERROR_NO_ERROR; //nrGood > 0 ? TRI_ERROR_NO_ERROR : TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE;
}

/// @brief commit a transaction on a subordinate
Result ClusterTrxMethods::commitTransaction(transaction::Methods& trx) {
  return commitAbortTransaction(trx, transaction::Status::COMMITTED);
}

/// @brief commit a transaction on a subordinate
Result ClusterTrxMethods::abortTransaction(transaction::Methods& trx) {
  return commitAbortTransaction(trx, transaction::Status::ABORTED);
}

/// @brief set the transaction ID header
void ClusterTrxMethods::addTransactionHeader(transaction::Methods const& trx,
                                          ServerID const& server,
                                          std::unordered_map<std::string, std::string>& headers) {
  TransactionState& state = *trx.state();
  TRI_ASSERT(state.isRunningInCluster());
  if (!(state.hasHint(transaction::Hints::Hint::GLOBAL_MANAGED) ||
        state.hasHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL))) {
    return; // no need
  }
  TRI_voc_tid_t tidPlus = state.id() + 1;
  TRI_ASSERT(!transaction::isLegacyTransactionId(tidPlus));
  TRI_ASSERT(!state.hasHint(transaction::Hints::Hint::SINGLE_OPERATION));
  
  const bool addBegin = !state.knowsServer(server);
  if (addBegin) {
    TRI_ASSERT(state.hasHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL));
    if (state.isCoordinator()) {
      return; // do not add header to server without a snippet
    } else if (transaction::isLeaderTransactionId(state.id())) {
      TRI_ASSERT(state.isDBServer());
      transaction::BuilderLeaser builder(trx.transactionContextPtr());
      ::buildTransactionBody(state, server, *builder.get());
      headers.emplace(StaticStrings::TransactionBody, builder->toJson());
      headers.emplace(arangodb::StaticStrings::TransactionId, std::to_string(tidPlus).append(" begin"));
    }
    // FIXME: only add server on a successful response ?
    state.addKnownServer(server);  // remember server
  } else {
    headers.emplace(arangodb::StaticStrings::TransactionId, std::to_string(tidPlus));
  }
}
  
/// @brief add transaction ID header for setting up AQL snippets
void ClusterTrxMethods::addAQLTransactionHeader(transaction::Methods const& trx,
                                             ServerID const& server,
                                             std::unordered_map<std::string, std::string>& headers) {
  TransactionState& state = *trx.state();
  TRI_ASSERT(state.isRunningInCluster());
  
  TRI_voc_tid_t tidPlus = state.id() + 1;
  TRI_ASSERT(!transaction::isLegacyTransactionId(tidPlus));
  TRI_ASSERT(!state.hasHint(transaction::Hints::Hint::SINGLE_OPERATION));
  
  std::string value = std::to_string(tidPlus);
  const bool addBegin = !state.knowsServer(server);
  if (addBegin) {
    TRI_ASSERT(state.hasHint(transaction::Hints::Hint::FROM_TOPLEVEL_AQL));
    if (state.isCoordinator()) {
      value.append(" aql"); // This is a single AQL query
    } else if (transaction::isLeaderTransactionId(state.id())) {
      TRI_ASSERT(state.isDBServer());
      value.append(" begin");
      transaction::BuilderLeaser builder(trx.transactionContextPtr());
      ::buildTransactionBody(state, server, *builder.get());
      headers.emplace(StaticStrings::TransactionBody, builder->toJson());
    }
    // FIXME: only add server on a successful response ?
    state.addKnownServer(server);  // remember server
  }
  headers.emplace(arangodb::StaticStrings::TransactionId, std::move(value));
}

}  // namespace arangodb
