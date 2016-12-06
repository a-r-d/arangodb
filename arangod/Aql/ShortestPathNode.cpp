/// @brief Implementation of Shortest Path Execution Node
///
/// @file arangod/Aql/ShortestPathNode.cpp
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// @author Michael Hackstein
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "ShortestPathNode.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Query.h"
#include "Cluster/ClusterComm.h"
#include "Indexes/Index.h"
#include "Utils/CollectionNameResolver.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb::basics;
using namespace arangodb::aql;

static void parseNodeInput(AstNode const* node, std::string& id,
                           Variable const*& variable) {
  switch (node->type) {
    case NODE_TYPE_REFERENCE:
      variable = static_cast<Variable*>(node->getData());
      id = "";
      break;
    case NODE_TYPE_VALUE:
      if (node->value.type != VALUE_TYPE_STRING) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                       "invalid start vertex. Must either be "
                                       "an _id string or an object with _id.");
      }
      variable = nullptr;
      id = node->getString();
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                     "invalid start vertex. Must either be an "
                                     "_id string or an object with _id.");
  }
}

static TRI_edge_direction_e parseDirection (uint64_t dirNum) {
  switch (dirNum) {
    case 0:
      return TRI_EDGE_ANY;
    case 1:
      return TRI_EDGE_IN;
    case 2:
      return TRI_EDGE_OUT;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_PARSE,
          "direction can only be INBOUND, OUTBOUND or ANY");
  }
}

ShortestPathNode::ShortestPathNode(
    ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase, uint64_t direction,
    AstNode const* start, AstNode const* target, AstNode const* graph,
    std::unique_ptr<traverser::ShortestPathOptions>& options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _inStartVariable(nullptr),
      _inTargetVariable(nullptr),
      _graphObj(nullptr),
      _options(options.release()) {

  TRI_ASSERT(_vocbase != nullptr);
  TRI_ASSERT(start != nullptr);
  TRI_ASSERT(target != nullptr);
  TRI_ASSERT(graph != nullptr);

  TRI_edge_direction_e baseDirection = parseDirection(direction);

  std::unordered_map<std::string, TRI_edge_direction_e> seenCollections;
  auto addEdgeColl = [&](std::string const& n, TRI_edge_direction_e dir) -> void {
    if (dir == TRI_EDGE_ANY) {
      _directions.emplace_back(TRI_EDGE_OUT);
      _edgeColls.emplace_back(
          std::make_unique<aql::Collection>(n, _vocbase, AccessMode::Type::READ));

      _directions.emplace_back(TRI_EDGE_IN);
      _edgeColls.emplace_back(
          std::make_unique<aql::Collection>(n, _vocbase, AccessMode::Type::READ));
    } else {
      _directions.emplace_back(dir);
      _edgeColls.emplace_back(
          std::make_unique<aql::Collection>(n, _vocbase, AccessMode::Type::READ));
    }
  };

  auto ci = ClusterInfo::instance();

  if (graph->type == NODE_TYPE_COLLECTION_LIST) {
    size_t edgeCollectionCount = graph->numMembers();
    auto resolver = std::make_unique<CollectionNameResolver>(vocbase);
    _graphInfo.openArray();
    _edgeColls.reserve(edgeCollectionCount);
    _directions.reserve(edgeCollectionCount);

    // List of edge collection names
    for (size_t i = 0; i < edgeCollectionCount; ++i) {
      TRI_edge_direction_e dir = TRI_EDGE_ANY;
      auto col = graph->getMember(i);

      if (col->type == NODE_TYPE_DIRECTION) {
        TRI_ASSERT(col->numMembers() == 2);
        auto dirNode = col->getMember(0);
        // We have a collection with special direction.
        TRI_ASSERT(dirNode->isIntValue());
        dir = parseDirection(dirNode->getIntValue());
        col = col->getMember(1);
      } else {
        dir = baseDirection;
      }
 
      std::string eColName = col->getString();

      // now do some uniqueness checks for the specified collections
      auto it = seenCollections.find(eColName);
      if (it != seenCollections.end()) {
        if ((*it).second != dir) {
          std::string msg("conflicting directions specified for collection '" +
                          std::string(eColName));
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
                                         msg);
        }
        // do not re-add the same collection!
        continue;
      }
      seenCollections.emplace(eColName, dir);
 
      auto eColType = resolver->getCollectionTypeCluster(eColName);
      if (eColType != TRI_COL_TYPE_EDGE) {
        std::string msg("collection type invalid for collection '" +
                        std::string(eColName) +
                        ": expecting collection type 'edge'");
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
                                       msg);
      }

      _graphInfo.add(VPackValue(eColName));
      if (ServerState::instance()->isRunningInCluster()) {
        auto c = ci->getCollection(_vocbase->name(), eColName);
        if (!c->isSmart()) {
          addEdgeColl(eColName, dir);
        } else {
          std::vector<std::string> names;
          names = c->realNamesForRead();
          for (auto const& name : names) {
            addEdgeColl(name, dir);
          }
        }
      } else {
        addEdgeColl(eColName, dir);
      }
    
      if (dir == TRI_EDGE_ANY) {
        // collection with direction ANY must be added again
        _graphInfo.add(VPackValue(eColName));
      }
    }
    _graphInfo.close();
  } else {
    if (_edgeColls.empty()) {
      if (graph->isStringValue()) {
        std::string graphName = graph->getString();
        _graphInfo.add(VPackValue(graphName));
        _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

        if (_graphObj == nullptr) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NOT_FOUND);
        }

        auto eColls = _graphObj->edgeCollections();
        size_t length = eColls.size();
        if (length == 0) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_EMPTY);
        }
        _edgeColls.reserve(length);
        _directions.reserve(length);

        for (const auto& n : eColls) {
          if (ServerState::instance()->isRunningInCluster()) {
            auto c = ci->getCollection(_vocbase->name(), n);
            if (!c->isSmart()) {
              addEdgeColl(n, baseDirection);
            } else {
              std::vector<std::string> names;
              names = c->realNamesForRead();
              for (auto const& name : names) {
                addEdgeColl(name, baseDirection);
              }
            }
          } else {
            addEdgeColl(n, baseDirection);
          }
        }
      }
    }
  }

  parseNodeInput(start, _startVertexId, _inStartVariable);
  parseNodeInput(target, _targetVertexId, _inTargetVariable);
}

ShortestPathNode::ShortestPathNode(
    ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
    std::vector<std::unique_ptr<aql::Collection>> const& edgeColls,
    std::vector<TRI_edge_direction_e> const& directions,
    Variable const* inStartVariable, std::string const& startVertexId,
    Variable const* inTargetVariable, std::string const& targetVertexId,
    std::unique_ptr<traverser::ShortestPathOptions>& options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _inStartVariable(inStartVariable),
      _startVertexId(startVertexId),
      _inTargetVariable(inTargetVariable),
      _targetVertexId(targetVertexId),
      _directions(directions),
      _graphObj(nullptr),
      _options(options.release()) {
  _graphInfo.openArray();
  for (auto const& it : edgeColls) {
    // Collections cannot be copied. So we need to create new ones to prevent leaks
    _edgeColls.emplace_back(std::make_unique<aql::Collection>(
        it->getName(), _vocbase, AccessMode::Type::READ));
    _graphInfo.add(VPackValue(it->getName()));
  }

  _graphInfo.close();
}

ShortestPathNode::~ShortestPathNode() {
}

arangodb::traverser::ShortestPathOptions* ShortestPathNode::options()
    const {
  return _options.get();
}

void ShortestPathNode::enhanceEngineInfo(arangodb::velocypack::Builder&) const {
}

void ShortestPathNode::addEngine(traverser::TraverserEngineID const&, ServerID const&) {
}

ShortestPathNode::ShortestPathNode(ExecutionPlan* plan,
                                   arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _vocbase(plan->getAst()->query()->vocbase()),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _inStartVariable(nullptr),
      _inTargetVariable(nullptr),
      _graphObj(nullptr) {
  _options = std::make_unique<arangodb::traverser::ShortestPathOptions>(
      plan->getAst()->query()->trx());
  // Directions
  VPackSlice dirList = base.get("directions");
  for (auto const& it : VPackArrayIterator(dirList)) {
    uint64_t dir = arangodb::basics::VelocyPackHelper::stringUInt64(it);
    TRI_edge_direction_e d;
    switch (dir) {
      case 0:
        d = TRI_EDGE_ANY;
        break;
      case 1:
        d = TRI_EDGE_IN;
        break;
      case 2:
        d = TRI_EDGE_OUT;
        break;
      default:
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "Invalid direction value");
        break;
    }
    _directions.emplace_back(d);
  }

  // Start Vertex
  if (base.hasKey("startInVariable")) {
    _inStartVariable = varFromVPack(plan->getAst(), base, "startInVariable");
  } else {
    VPackSlice v = base.get("startVertexId");
    if (!v.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex must be a string");
    }
    _startVertexId = v.copyString();

    if (_startVertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex mustn't be empty");
    }
  }

  // Target Vertex
  if (base.hasKey("targetInVariable")) {
    _inTargetVariable = varFromVPack(plan->getAst(), base, "targetInVariable");
  } else {
    VPackSlice v = base.get("targetVertexId");
    if (!v.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "target vertex must be a string");
    }
    _targetVertexId = v.copyString();
    if (_targetVertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "target vertex mustn't be empty");
    }
  }

  std::string graphName;
  if (base.hasKey("graph") && (base.get("graph").isString())) {
    graphName = base.get("graph").copyString();
    if (base.hasKey("graphDefinition")) {
      _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

      if (_graphObj == nullptr) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NOT_FOUND);
      }

      auto const& eColls = _graphObj->edgeCollections();
      for (auto const& it : eColls) {
        _edgeColls.emplace_back(
            std::make_unique<aql::Collection>(it, _vocbase, AccessMode::Type::READ));
        
        // if there are twice as many directions as collections, this means we
        // have a shortest path with direction ANY. we must add each collection
        // twice then
        if (_directions.size() == 2 * eColls.size()) {
          // add collection again
          _edgeColls.emplace_back(
              std::make_unique<aql::Collection>(it, _vocbase, AccessMode::Type::READ));
        }
      }
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "missing graphDefinition.");
    }
  } else {
    _graphInfo.add(base.get("graph"));
    if (!_graphInfo.slice().isArray()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "graph has to be an array.");
    }
    // List of edge collection names
    for (auto const& it : VPackArrayIterator(_graphInfo.slice())) {
      if (!it.isString()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                       "graph has to be an array of strings.");
      }
      std::string e = arangodb::basics::VelocyPackHelper::getStringValue(it, "");
      _edgeColls.emplace_back(
          std::make_unique<aql::Collection>(e, _vocbase, AccessMode::Type::READ));
    }
    if (_edgeColls.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_BAD_JSON_PLAN,
          "graph has to be a non empty array of strings.");
    }
  }

  // Out variables
  if (base.hasKey("vertexOutVariable")) {
    _vertexOutVariable = varFromVPack(plan->getAst(), base, "vertexOutVariable");
  }
  if (base.hasKey("edgeOutVariable")) {
    _edgeOutVariable = varFromVPack(plan->getAst(), base, "edgeOutVariable");
  }

  // Flags
  if (base.hasKey("shortestPathFlags")) {
    // _options = ShortestPathOptions(base);
  }
}

void ShortestPathNode::toVelocyPackHelper(VPackBuilder& nodes,
                                          bool verbose) const {
  ExecutionNode::toVelocyPackHelperGeneric(nodes,
                                           verbose);  // call base class method
  nodes.add("database", VPackValue(_vocbase->name()));
  nodes.add("graph", _graphInfo.slice());
  nodes.add(VPackValue("directions"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& d : _directions) {
      nodes.add(VPackValue(d));
    }
  }

  // In variables
  if (usesStartInVariable()) {
    nodes.add(VPackValue("startInVariable"));
    startInVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("startVertexId", VPackValue(_startVertexId));
  }

  if (usesTargetInVariable()) {
    nodes.add(VPackValue("targetInVariable"));
    targetInVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("targetVertexId", VPackValue(_targetVertexId));
  }

  if (_graphObj != nullptr) {
    nodes.add(VPackValue("graphDefinition"));
    _graphObj->toVelocyPack(nodes, verbose);
  }

  // Out variables
  if (usesVertexOutVariable()) {
    nodes.add(VPackValue("vertexOutVariable"));
    vertexOutVariable()->toVelocyPack(nodes);
  }
  if (usesEdgeOutVariable()) {
    nodes.add(VPackValue("edgeOutVariable"));
    edgeOutVariable()->toVelocyPack(nodes);
  }

  nodes.add(VPackValue("shortestPathFlags"));
  // _options.toVelocyPack(nodes);

  // And close it:
  nodes.close();
}

ExecutionNode* ShortestPathNode::clone(ExecutionPlan* plan,
                                       bool withDependencies,
                                       bool withProperties) const {
  auto tmp =
      std::make_unique<arangodb::traverser::ShortestPathOptions>(*_options.get());
  auto c = new ShortestPathNode(plan, _id, _vocbase, _edgeColls, _directions,
                                _inStartVariable, _startVertexId,
                                _inTargetVariable, _targetVertexId, tmp);

  if (usesVertexOutVariable()) {
    auto vertexOutVariable = _vertexOutVariable;
    if (withProperties) {
      vertexOutVariable =
          plan->getAst()->variables()->createVariable(vertexOutVariable);
    }
    TRI_ASSERT(vertexOutVariable != nullptr);
    c->setVertexOutput(vertexOutVariable);
  }

  if (usesEdgeOutVariable()) {
    auto edgeOutVariable = _edgeOutVariable;
    if (withProperties) {
      edgeOutVariable =
          plan->getAst()->variables()->createVariable(edgeOutVariable);
    }
    TRI_ASSERT(edgeOutVariable != nullptr);
    c->setEdgeOutput(edgeOutVariable);
  }

  cloneHelper(c, plan, withDependencies, withProperties);

  return static_cast<ExecutionNode*>(c);
}

double ShortestPathNode::estimateCost(size_t& nrItems) const {
  // Standard estimation for Shortest path is O(|E| + |V|*LOG(|V|))
  // At this point we know |E| but do not know |V|.
  size_t incoming = 0;
  double depCost = _dependencies.at(0)->getCost(incoming);
  auto trx = _plan->getAst()->query()->trx();
  size_t edgesCount = 0;
  double nodesEstimate = 0;
  auto collections = _plan->getAst()->query()->collections();

  for (auto const& it : _edgeColls) {

    auto collection = collections->get(it->getName());

    if (collection == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unexpected pointer for collection");
    }

    TRI_ASSERT(collection != nullptr);

    size_t edges = collection->count();

    auto indexes = trx->indexesForCollection(collection->name);
    for (auto const& index : indexes) {
      if (index->type() == arangodb::Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX) {
        // We can only use Edge Index
        if (index->hasSelectivityEstimate()) {
          nodesEstimate += edges * index->selectivityEstimate();
        } else {
          // Hard-coded fallback should not happen
          nodesEstimate += edges * 0.01;
        }
        break;
      }
    }

    edgesCount += edges;
  }
  nrItems = edgesCount + static_cast<size_t>(std::log2(nodesEstimate) * nodesEstimate);
  return depCost + nrItems;
}
