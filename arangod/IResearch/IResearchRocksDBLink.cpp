////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "Basics/Common.h" // required for RocksDBColumnFamily.h
#include "IResearch/IResearchFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"
#include "RocksDBEngine/RocksDBColumnFamily.h"

#include "IResearchRocksDBLink.h"

NS_LOCAL

////////////////////////////////////////////////////////////////////////////////
/// @brief return a reference to a static VPackSlice of an empty RocksDB index
///        definition
////////////////////////////////////////////////////////////////////////////////
VPackSlice const& emptyParentSlice() {
  static const struct EmptySlice {
    VPackBuilder _builder;
    VPackSlice _slice;
    EmptySlice() {
      VPackBuilder fieldsBuilder;

      fieldsBuilder.openArray();
      fieldsBuilder.close(); // empty array
      _builder.openObject();
      _builder.add("fields", fieldsBuilder.slice()); // empty array
      arangodb::iresearch::IResearchLink::setType(_builder); // the index type required by Index
      _builder.close(); // object with just one field required by the Index constructor
      _slice = _builder.slice();
    }
  } emptySlice;

  return emptySlice._slice;
}

NS_END

NS_BEGIN(arangodb)
NS_BEGIN(iresearch)

IResearchRocksDBLink::IResearchRocksDBLink(
    TRI_idx_iid_t iid,
    arangodb::LogicalCollection* collection
): RocksDBIndex(iid, collection, emptyParentSlice(), RocksDBColumnFamily::invalid(), false),
   IResearchLink(iid, collection) {
  _unique = false; // cannot be unique since multiple fields are indexed
  _sparse = true;  // always sparse
}

IResearchRocksDBLink::~IResearchRocksDBLink() {
  // NOOP
}

/*static*/ IResearchRocksDBLink::ptr IResearchRocksDBLink::make(
  TRI_idx_iid_t iid,
  arangodb::LogicalCollection* collection,
  arangodb::velocypack::Slice const& definition
) noexcept {
  try {
    PTR_NAMED(IResearchRocksDBLink, ptr, iid, collection);

    #ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      auto* link = dynamic_cast<arangodb::iresearch::IResearchRocksDBLink*>(ptr.get());
    #else
      auto* link = static_cast<arangodb::iresearch::IResearchRocksDBLink*>(ptr.get());
    #endif

    return link && link->init(definition) ? ptr : nullptr;
  } catch (std::exception const& e) {
    LOG_TOPIC(WARN, Logger::DEVEL) << "caught exception while creating IResearch view RocksDB link '" << iid << "'" << e.what();
  } catch (...) {
    LOG_TOPIC(WARN, Logger::DEVEL) << "caught exception while creating IResearch view RocksDB link '" << iid << "'";
  }

  return nullptr;
}

void IResearchRocksDBLink::toVelocyPack(
    arangodb::velocypack::Builder& builder,
    bool withFigures,
    bool forPersistence
) const {
  TRI_ASSERT(!builder.isOpenObject());
  builder.openObject();
  bool success = json(builder, forPersistence);
  TRI_ASSERT(success);

  if (withFigures) {
    VPackBuilder figuresBuilder;

    figuresBuilder.openObject();
    toVelocyPackFigures(figuresBuilder);
    figuresBuilder.close();
    builder.add("figures", figuresBuilder.slice());
  }

  builder.close();
}

Result IResearchRocksDBLink::drop(TRI_voc_cid_t viewId, TRI_voc_cid_t collectionId) {
  LOG_TOPIC(DEBUG, arangodb::iresearch::IResearchFeature::IRESEARCH) << "Removing all documents belonging to view " << viewId << " sourced from collection " << collectionId;
}

NS_END // iresearch
NS_END // arangodb

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
