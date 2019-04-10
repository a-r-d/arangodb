////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "ExecutionBlockImpl.h"

#include "Basics/Common.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionState.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/InputAqlItemRow.h"

#include "Aql/CalculationExecutor.h"
#include "Aql/ConstrainedSortExecutor.h"
#include "Aql/CountCollectExecutor.h"
#include "Aql/DistinctCollectExecutor.h"
#include "Aql/EnumerateCollectionExecutor.h"
#include "Aql/EnumerateListExecutor.h"
#include "Aql/FilterExecutor.h"
#include "Aql/HashedCollectExecutor.h"
#include "Aql/IResearchViewExecutor.h"
#include "Aql/IdExecutor.h"
#include "Aql/IndexExecutor.h"
#include "Aql/LimitExecutor.h"
#include "Aql/ModificationExecutor.h"
#include "Aql/ModificationExecutorTraits.h"
#include "Aql/NoResultsExecutor.h"
#include "Aql/ReturnExecutor.h"
#include "Aql/ShortestPathExecutor.h"
#include "Aql/SingleRemoteModificationExecutor.h"
#include "Aql/SortExecutor.h"
#include "Aql/SortRegister.h"
#include "Aql/SortedCollectExecutor.h"
#include "Aql/SortingGatherExecutor.h"
#include "Aql/SubqueryExecutor.h"
#include "Aql/TraversalExecutor.h"

#include <type_traits>

using namespace arangodb;
using namespace arangodb::aql;

namespace {

std::string const doneString = "DONE";
std::string const hasMoreString = "HASMORE";
std::string const waitingString = "WAITING";

static std::string const& stateToString(ExecutionState state) {
  switch (state) {
    case ExecutionState::DONE:
      return doneString;
    case ExecutionState::HASMORE:
      return hasMoreString;
    case ExecutionState::WAITING:
      return waitingString;
  }
}

}  // namespace

template <class Executor>
ExecutionBlockImpl<Executor>::ExecutionBlockImpl(ExecutionEngine* engine,
                                                 ExecutionNode const* node,
                                                 typename Executor::Infos&& infos)
    : ExecutionBlock(engine, node),
      _blockFetcher(_dependencies, engine->itemBlockManager(),
                    infos.getInputRegisters(), infos.numberOfInputRegisters()),
      _rowFetcher(_blockFetcher),
      _infos(std::move(infos)),
      _executor(_rowFetcher, _infos),
      _outputItemRow(),
      _query(*engine->getQuery()),
      _engine(engine),
      _trx(engine->getQuery()->trx()),
      _pos(0) {
  TRI_ASSERT(_trx != nullptr);

  // already insert ourselves into the statistics results
  if (_profile >= PROFILE_LEVEL_BLOCKS) {
    _engine->_stats.nodes.emplace(node->id(), ExecutionStats::Node());
  }
}

template <class Executor>
ExecutionBlockImpl<Executor>::~ExecutionBlockImpl() {
  for (auto& it : _buffer) {
    delete it;
  }
}

template <class Executor>
std::pair<ExecutionState, std::unique_ptr<AqlItemBlock>> ExecutionBlockImpl<Executor>::getSome(size_t atMost) {
  traceGetSomeBeginInner(atMost);
  auto result = getSomeWithoutTrace(atMost);
  return traceGetSomeEnd(result.first, std::move(result.second));
}

template <class Executor>
std::pair<ExecutionState, std::unique_ptr<AqlItemBlock>>
ExecutionBlockImpl<Executor>::getSomeWithoutTrace(size_t atMost) {
  // silence tests -- we need to introduce new failure tests for fetchers
  TRI_IF_FAILURE("ExecutionBlock::getOrSkipSome1") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  TRI_IF_FAILURE("ExecutionBlock::getOrSkipSome2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  TRI_IF_FAILURE("ExecutionBlock::getOrSkipSome3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  if (getQuery().killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  if (!_outputItemRow) {
    ExecutionState state;
    std::shared_ptr<AqlItemBlockShell> newBlock;
    std::tie(state, newBlock) =
        requestWrappedBlock(atMost, _infos.numberOfOutputRegisters());
    if (state == ExecutionState::WAITING) {
      TRI_ASSERT(newBlock == nullptr);
      return {state, nullptr};
    }
    if (newBlock == nullptr) {
      TRI_ASSERT(state == ExecutionState::DONE);
      // _rowFetcher must be DONE now already
      return {state, nullptr};
    }
    TRI_ASSERT(newBlock != nullptr);
    _outputItemRow = createOutputRow(newBlock);
  }

  // TODO It's not very obvious that `state` will be initialized, because
  // it's not obvious that the loop will run at least once (e.g. after a
  // WAITING). It should, but I'd like that to be clearer. Initializing here
  // won't help much because it's unclear whether the value will be correct.
  ExecutionState state = ExecutionState::HASMORE;
  ExecutorStats executorStats{};

  TRI_ASSERT(atMost > 0);

  TRI_ASSERT(!_outputItemRow->isFull());
  while (!_outputItemRow->isFull()) {
    std::tie(state, executorStats) = _executor.produceRow(*_outputItemRow);
    // Count global but executor-specific statistics, like number of filtered
    // rows.
    _engine->_stats += executorStats;
    if (_outputItemRow->produced()) {
      _outputItemRow->advanceRow();
    }

    if (state == ExecutionState::WAITING) {
      return {state, nullptr};
    }

    if (state == ExecutionState::DONE) {
      // TODO Does this work as expected when there was no row produced, or
      // we were DONE already, so we didn't build a single row?
      // We must return nullptr then, because empty AqlItemBlocks are not
      // allowed!
      auto outputBlock = _outputItemRow->stealBlock();
      // This is not strictly necessary here, as we shouldn't be called again
      // after DONE.
      _outputItemRow.reset();
      return {state, std::move(outputBlock)};
    }
  }

  TRI_ASSERT(state == ExecutionState::HASMORE);
  // When we're passing blocks through we have no control over the size of the
  // output block.
  if /* constexpr */ (!Executor::Properties::allowsBlockPassthrough) {
    TRI_ASSERT(_outputItemRow->numRowsWritten() == atMost);
  }

  auto outputBlock = _outputItemRow->stealBlock();
  // we guarantee that we do return a valid pointer in the HASMORE case.
  TRI_ASSERT(outputBlock != nullptr);
  // TODO OutputAqlItemRow could get "reset" and "isValid" methods and be reused
  _outputItemRow.reset();
  return {state, std::move(outputBlock)};
}

template <class Executor>
std::unique_ptr<OutputAqlItemRow> ExecutionBlockImpl<Executor>::createOutputRow(
    std::shared_ptr<AqlItemBlockShell>& newBlock) const {
  if /* constexpr */ (Executor::Properties::allowsBlockPassthrough) {
    return std::make_unique<OutputAqlItemRow>(newBlock, infos().getOutputRegisters(),
                                              infos().registersToKeep(),
                                              infos().registersToClear(),
                                              OutputAqlItemRow::CopyRowBehaviour::DoNotCopyInputRows);
  } else {
    return std::make_unique<OutputAqlItemRow>(newBlock, infos().getOutputRegisters(),
                                              infos().registersToKeep(),
                                              infos().registersToClear());
  }
}

template <class Executor>
std::pair<ExecutionState, size_t> ExecutionBlockImpl<Executor>::skipSome(size_t atMost) {
  // TODO IMPLEMENT ME, this is a stub!

  traceSkipSomeBeginInner(atMost);

  auto res = getSomeWithoutTrace(atMost);

  size_t skipped = 0;
  if (res.second != nullptr) {
    skipped = res.second->size();
    AqlItemBlock* resultPtr = res.second.get();
    returnBlock(resultPtr);
    res.second.release();
  }

  return traceSkipSomeEnd(res.first, skipped);
}

template <class Executor>
std::pair<ExecutionState, std::unique_ptr<AqlItemBlock>> ExecutionBlockImpl<Executor>::traceGetSomeEnd(
    ExecutionState state, std::unique_ptr<AqlItemBlock> result) {
  traceGetSomeEndInner(result.get(), state);
  return {state, std::move(result)};
}

template <class Executor>
ExecutionState ExecutionBlockImpl<Executor>::getHasMoreState() {
  if (_done) {
    return ExecutionState::DONE;
  }
  if (_buffer.empty() && _upstreamState == ExecutionState::DONE) {
    _done = true;
    return ExecutionState::DONE;
  }
  return ExecutionState::HASMORE;
}

template <class Executor>
std::pair<ExecutionState, size_t> ExecutionBlockImpl<Executor>::traceSkipSomeEnd(
    ExecutionState state, size_t skipped) {
  traceSkipSomeEndInner(skipped, state);
  return {state, skipped};
}

template <class Executor>
std::pair<ExecutionState, Result> ExecutionBlockImpl<Executor>::initializeCursor(InputAqlItemRow const& input) {
  // destroy and re-create the BlockFetcher
  _blockFetcher.~BlockFetcher();
  new (&_blockFetcher)
      BlockFetcher(_dependencies, _engine->itemBlockManager(),
                   _infos.getInputRegisters(), _infos.numberOfInputRegisters());

  // destroy and re-create the Fetcher
  _rowFetcher.~Fetcher();
  new (&_rowFetcher) Fetcher(_blockFetcher);

  // destroy and re-create the Executor
  _executor.~Executor();
  new (&_executor) Executor(_rowFetcher, _infos);
  // // use this with c++17 instead of specialisation below
  // if constexpr (std::is_same_v<Executor, IdExecutor>) {
  //   if (items != nullptr) {
  //     _executor._inputRegisterValues.reset(
  //         items->slice(pos, *(_executor._infos.registersToKeep())));
  //   }
  // }

  if (_dependencyPos == _dependencies.end()) {
    // We need to start again.
    _dependencyPos = _dependencies.begin();
  }
  for (; _dependencyPos != _dependencies.end(); ++_dependencyPos) {
    auto res = (*_dependencyPos)->initializeCursor(input);
    if (res.first == ExecutionState::WAITING || !res.second.ok()) {
      // If we need to wait or get an error we return as is.
      return res;
    }
  }

  for (auto& it : _buffer) {
    returnBlock(it);
  }
  _buffer.clear();

  _done = false;
  _upstreamState = ExecutionState::HASMORE;
  _pos = 0;
  _collector.clear();

  TRI_ASSERT(getHasMoreState() == ExecutionState::HASMORE);
  TRI_ASSERT(_dependencyPos == _dependencies.end());
  return {ExecutionState::DONE, TRI_ERROR_NO_ERROR};
}

template <class Executor>
std::pair<ExecutionState, Result> ExecutionBlockImpl<Executor>::internalShutdown(int errorCode) {
  if (_dependencyPos == _dependencies.end()) {
    _shutdownResult.reset(TRI_ERROR_NO_ERROR);
    _dependencyPos = _dependencies.begin();
  }

  for (; _dependencyPos != _dependencies.end(); ++_dependencyPos) {
    Result res;
    ExecutionState state;
    try {
      std::tie(state, res) = (*_dependencyPos)->shutdown(errorCode);
      if (state == ExecutionState::WAITING) {
        return {state, TRI_ERROR_NO_ERROR};
      }
    } catch (...) {
      _shutdownResult.reset(TRI_ERROR_INTERNAL);
    }

    if (res.fail()) {
      _shutdownResult = res;
    }
  }

  if (!_buffer.empty()) {
    for (auto& it : _buffer) {
      delete it;
    }
    _buffer.clear();
  }

  return {ExecutionState::DONE, _shutdownResult};
}

template <class Executor>
std::pair<ExecutionState, Result> ExecutionBlockImpl<Executor>::shutdown(int errorCode) {
  return internalShutdown(errorCode);
}

// Work around GCC bug: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=56480
// Without the namespaces it fails with
// error: specialization of 'template<class Executor> std::pair<arangodb::aql::ExecutionState, arangodb::Result> arangodb::aql::ExecutionBlockImpl<Executor>::initializeCursor(arangodb::aql::AqlItemBlock*, size_t)' in different namespace
namespace arangodb {
namespace aql {
// TODO -- remove this specialization when cpp 17 becomes available
template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<IdExecutor<ConstFetcher>>::initializeCursor(
    InputAqlItemRow const& input) {
  // destroy and re-create the BlockFetcher
  _blockFetcher.~BlockFetcher();
  new (&_blockFetcher)
      BlockFetcher(_dependencies, _engine->itemBlockManager(),
                   infos().getInputRegisters(), infos().numberOfInputRegisters());

  // destroy and re-create the Fetcher
  _rowFetcher.~Fetcher();
  new (&_rowFetcher) Fetcher(_blockFetcher);

  std::unique_ptr<AqlItemBlock> block =
      input.cloneToBlock(_engine->itemBlockManager(), *(infos().registersToKeep()),
                         infos().numberOfOutputRegisters());

  auto shell = std::make_shared<AqlItemBlockShell>(_engine->itemBlockManager(),
                                                   std::move(block));
  _rowFetcher.injectBlock(shell);

  // destroy and re-create the Executor
  _executor.~IdExecutor<ConstFetcher>();
  new (&_executor) IdExecutor<ConstFetcher>(_rowFetcher, _infos);

  // end of default initializeCursor
  if (_dependencyPos == _dependencies.end()) {
    // We need to start again.
    _dependencyPos = _dependencies.begin();
  }
  for (; _dependencyPos != _dependencies.end(); ++_dependencyPos) {
    auto res = (*_dependencyPos)->initializeCursor(input);
    if (res.first == ExecutionState::WAITING || !res.second.ok()) {
      // If we need to wait or get an error we return as is.
      return res;
    }
  }

  for (auto& it : _buffer) {
    returnBlock(it);
  }
  _buffer.clear();

  _done = false;
  _upstreamState = ExecutionState::HASMORE;
  _pos = 0;
  _collector.clear();

  TRI_ASSERT(getHasMoreState() == ExecutionState::HASMORE);
  TRI_ASSERT(_dependencyPos == _dependencies.end());
  return {ExecutionState::DONE, TRI_ERROR_NO_ERROR};
}

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<TraversalExecutor>::shutdown(int errorCode) {
  ExecutionState state;
  Result result;

  std::tie(state, result) = internalShutdown(errorCode);

  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  return this->executor().shutdown(errorCode);
}

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<ShortestPathExecutor>::shutdown(int errorCode) {
  ExecutionState state;
  Result result;

  std::tie(state, result) = internalShutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  return this->executor().shutdown(errorCode);
}

template <>
std::pair<ExecutionState, Result> ExecutionBlockImpl<SubqueryExecutor>::shutdown(int errorCode) {
  ExecutionState state;
  Result subqueryResult;
  // shutdown is repeatable
  std::tie(state, subqueryResult) = this->executor().shutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {ExecutionState::WAITING, subqueryResult};
  }
  Result result;

  std::tie(state, result) = internalShutdown(errorCode);
  if (state == ExecutionState::WAITING) {
    return {state, result};
  }
  if (result.fail()) {
    return {state, result};
  }
  return {state, subqueryResult};
}

}  // namespace aql
}  // namespace arangodb

template <class Executor>
AqlItemBlock* ExecutionBlockImpl<Executor>::requestBlock(size_t nrItems, RegisterId nrRegs) {  // TODO check correct here?
  return _engine->itemBlockManager().requestBlock(nrItems, nrRegs);
}

template <class Executor>
void ExecutionBlockImpl<Executor>::returnBlock(AqlItemBlock*& block) noexcept {  // TODO check?
  _engine->itemBlockManager().returnBlock(block);
}

template <class Executor>
std::pair<ExecutionState, std::shared_ptr<AqlItemBlockShell>>
ExecutionBlockImpl<Executor>::requestWrappedBlock(size_t nrItems, RegisterId nrRegs) {
  std::shared_ptr<AqlItemBlockShell> blockShell;
  if /* constexpr */ (Executor::Properties::allowsBlockPassthrough) {
    // If blocks can be passed through, we do not create new blocks.
    // Instead, we take the input blocks from the fetcher and reuse them.

    ExecutionState state;
    std::tie(state, blockShell) = _rowFetcher.fetchBlockForPassthrough(nrItems);

    if (state == ExecutionState::WAITING) {
      TRI_ASSERT(blockShell == nullptr);
      return {state, nullptr};
    }
    if (blockShell == nullptr) {
      TRI_ASSERT(state == ExecutionState::DONE);
      return {state, nullptr};
    }

    // Now we must have a block.
    TRI_ASSERT(blockShell != nullptr);
    // Assert that the block has enough registers. This must be guaranteed by
    // the register planning.
    TRI_ASSERT(blockShell->block().getNrRegs() == nrRegs);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // Check that all output registers are empty.
    if (!std::is_same<Executor, ReturnExecutor<true>>::value) {
      for (auto const& reg : *infos().getOutputRegisters()) {
        for (size_t row = 0; row < blockShell->block().size(); row++) {
          AqlValue const& val = blockShell->block().getValueReference(row, reg);
          TRI_ASSERT(val.isEmpty());
        }
      }
    }
#endif
  } else if /* constexpr */ (Executor::Properties::inputSizeRestrictsOutputSize) {
    // The SortExecutor should refetch a block to save memory in case if only few elements to sort
    ExecutionState state;
    size_t expectedRows = 0;
    std::tie(state, expectedRows) = _rowFetcher.preFetchNumberOfRows(nrItems);
    if (state == ExecutionState::WAITING) {
      TRI_ASSERT(expectedRows == 0);
      return {state, nullptr};
    }
    expectedRows += _executor.numberOfRowsInFlight();
    nrItems = (std::min)(expectedRows, nrItems);
    if (nrItems == 0) {
      TRI_ASSERT(state == ExecutionState::DONE);
      return {state, nullptr};
    }
    AqlItemBlock* block = requestBlock(nrItems, nrRegs);
    blockShell =
        std::make_shared<AqlItemBlockShell>(_engine->itemBlockManager(),
                                            std::unique_ptr<AqlItemBlock>{block});
  } else {
    AqlItemBlock* block = requestBlock(nrItems, nrRegs);

    blockShell =
        std::make_shared<AqlItemBlockShell>(_engine->itemBlockManager(),
                                            std::unique_ptr<AqlItemBlock>{block});
  }

  // std::unique_ptr<OutputAqlItemBlockShell> outputBlockShell =
  //     std::make_unique<OutputAqlItemBlockShell>(blockShell, _infos.getOutputRegisters(),
  //                                               _infos.registersToKeep());

  return {ExecutionState::HASMORE, std::move(blockShell)};
}

// Trace the start of a getSome call
template <class Executor>
void ExecutionBlockImpl<Executor>::traceGetSomeBeginInner(size_t atMost) {
  if (_profile >= PROFILE_LEVEL_BLOCKS) {
    if (_getSomeBegin <= 0.0) {
      _getSomeBegin = TRI_microtime();
    }
    if (_profile >= PROFILE_LEVEL_TRACE_1) {
      LOG_TOPIC("ca7db", INFO, Logger::QUERIES)
          << "getSome type=" << _exeNode->getTypeString() << " atMost = " << atMost
          << " this=" << (uintptr_t)this << " id=" << _exeNode->id();
    }
  }
}

// Trace the end of a getSome call, potentially with result
template <class Executor>
void ExecutionBlockImpl<Executor>::traceGetSomeEndInner(AqlItemBlock const* result,
                                                        ExecutionState state) {
  TRI_ASSERT(result != nullptr || state != ExecutionState::HASMORE);
  if (_profile >= PROFILE_LEVEL_BLOCKS) {
    ExecutionStats::Node stats;
    stats.calls = 1;
    stats.items = result != nullptr ? result->size() : 0;
    if (state != ExecutionState::WAITING) {
      stats.runtime = TRI_microtime() - _getSomeBegin;
      _getSomeBegin = 0.0;
    }

    auto it = _engine->_stats.nodes.find(_exeNode->id());
    if (it != _engine->_stats.nodes.end()) {
      it->second += stats;
    } else {
      _engine->_stats.nodes.emplace(_exeNode->id(), stats);
    }

    if (_profile >= PROFILE_LEVEL_TRACE_1) {
      LOG_TOPIC("07a60", INFO, Logger::QUERIES)
          << "getSome done type=" << _exeNode->getTypeString()
          << " this=" << (uintptr_t)this << " id=" << _exeNode->id()
          << " state=" << ::stateToString(state);

      if (_profile >= PROFILE_LEVEL_TRACE_2) {
        if (result == nullptr) {
          LOG_TOPIC("daa64", INFO, Logger::QUERIES)
              << "getSome type=" << _exeNode->getTypeString() << " result: nullptr";
        } else {
          VPackBuilder builder;
          {
            VPackObjectBuilder guard(&builder);
            result->toVelocyPack(_trx, builder);
          }
          LOG_TOPIC("fcd9c", INFO, Logger::QUERIES)
              << "getSome type=" << _exeNode->getTypeString()
              << " result: " << builder.toJson();
        }
      }
    }
  }
}

template <class Executor>
void ExecutionBlockImpl<Executor>::traceSkipSomeBeginInner(size_t atMost) {  // ALL TRACE TODO: -> IMPL PRIV
  if (_profile >= PROFILE_LEVEL_BLOCKS) {
    if (_getSomeBegin <= 0.0) {
      _getSomeBegin = TRI_microtime();
    }
    if (_profile >= PROFILE_LEVEL_TRACE_1) {
      LOG_TOPIC("dba8a", INFO, Logger::QUERIES)
          << "skipSome type=" << _exeNode->getTypeString() << " atMost = " << atMost
          << " this=" << (uintptr_t)this << " id=" << _exeNode->id();
    }
  }
}

template <class Executor>
void ExecutionBlockImpl<Executor>::traceSkipSomeEndInner(size_t skipped, ExecutionState state) {
  if (_profile >= PROFILE_LEVEL_BLOCKS) {
    ExecutionStats::Node stats;
    stats.calls = 1;
    stats.items = skipped;
    if (state != ExecutionState::WAITING) {
      stats.runtime = TRI_microtime() - _getSomeBegin;
      _getSomeBegin = 0.0;
    }

    auto it = _engine->_stats.nodes.find(_exeNode->id());
    if (it != _engine->_stats.nodes.end()) {
      it->second += stats;
    } else {
      _engine->_stats.nodes.emplace(_exeNode->id(), stats);
    }

    if (_profile >= PROFILE_LEVEL_TRACE_1) {
      LOG_TOPIC("d1950", INFO, Logger::QUERIES)
          << "skipSome done type=" << _exeNode->getTypeString()
          << " this=" << (uintptr_t)this << " id=" << _exeNode->id()
          << " state=" << ::stateToString(state);
    }
  }
}

template class ::arangodb::aql::ExecutionBlockImpl<CalculationExecutor<CalculationType::Condition>>;
template class ::arangodb::aql::ExecutionBlockImpl<CalculationExecutor<CalculationType::Reference>>;
template class ::arangodb::aql::ExecutionBlockImpl<CalculationExecutor<CalculationType::V8Condition>>;
template class ::arangodb::aql::ExecutionBlockImpl<ConstrainedSortExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<CountCollectExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<DistinctCollectExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<EnumerateCollectionExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<EnumerateListExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<FilterExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<HashedCollectExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<false>>;
template class ::arangodb::aql::ExecutionBlockImpl<IResearchViewExecutor<true>>;
template class ::arangodb::aql::ExecutionBlockImpl<IdExecutor<ConstFetcher>>;
template class ::arangodb::aql::ExecutionBlockImpl<IdExecutor<SingleRowFetcher<true>>>;
template class ::arangodb::aql::ExecutionBlockImpl<IndexExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<LimitExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Insert, SingleBlockFetcher<false /*allowsBlockPassthrough */>>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Insert, AllRowsFetcher>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Remove, SingleBlockFetcher<false /*allowsBlockPassthrough */>>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Remove, AllRowsFetcher>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Replace, SingleBlockFetcher<false /*allowsBlockPassthrough */>>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Replace, AllRowsFetcher>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Update, SingleBlockFetcher<false /*allowsBlockPassthrough */>>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Update, AllRowsFetcher>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Upsert, SingleBlockFetcher<false /*allowsBlockPassthrough */>>>;
template class ::arangodb::aql::ExecutionBlockImpl<ModificationExecutor<Upsert, AllRowsFetcher>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<IndexTag>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Insert>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Remove>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Update>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Replace>>;
template class ::arangodb::aql::ExecutionBlockImpl<SingleRemoteModificationExecutor<Upsert>>;
template class ::arangodb::aql::ExecutionBlockImpl<NoResultsExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<ReturnExecutor<false>>;
template class ::arangodb::aql::ExecutionBlockImpl<ReturnExecutor<true>>;
template class ::arangodb::aql::ExecutionBlockImpl<ShortestPathExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SortedCollectExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SortExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SubqueryExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<TraversalExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<SortingGatherExecutor>;
