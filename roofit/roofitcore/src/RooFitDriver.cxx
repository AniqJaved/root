/*
 * Project: RooFit
 * Authors:
 *   Jonas Rembser, CERN 2021
 *   Emmanouil Michalainas, CERN 2021
 *
 * Copyright (c) 2021, CERN
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted according to the terms
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)
 */

/**
\file RooFitDriver.cxx
\class RooFitDriver
\ingroup Roofitcore

This class can evaluate a RooAbsReal object in other ways than recursive graph
traversal. Currently, it is being used for evaluating a RooAbsReal object and
supplying the value to the minimizer, during a fit. The class scans the
dependencies and schedules the computations in a secure and efficient way. The
computations take place in the RooBatchCompute library and can be carried off
by either the CPU or a CUDA-supporting GPU. The RooFitDriver class takes care
of data transfers. An instance of this class is created every time
RooAbsPdf::fitTo() is called and gets destroyed when the fitting ends.
**/

#include "RooFitDriver.h"

#include <RooAbsCategory.h>
#include <RooAbsData.h>
#include <RooAbsReal.h>
#include <RooRealVar.h>
#include <RooBatchCompute.h>
#include <RooHelpers.h>
#include <RooMsgService.h>
#include "RooFit/BatchModeDataHelpers.h"
#include "RooFit/BatchModeHelpers.h"
#include <RooSimultaneous.h>

#include <TList.h>

#include <iomanip>
#include <numeric>
#include <thread>

#ifdef R__HAS_CUDA
namespace CudaInterface = RooFit::Detail::CudaInterface;
#endif

namespace ROOT {
namespace Experimental {

namespace {

void logArchitectureInfo(RooFit::BatchModeOption batchMode)
{
   // We have to exit early if the message stream is not active. Otherwise it's
   // possible that this function skips logging because it thinks it has
   // already logged, but actually it didn't.
   if (!RooMsgService::instance().isActive(static_cast<RooAbsArg *>(nullptr), RooFit::Fitting, RooFit::INFO)) {
      return;
   }

   // Don't repeat logging architecture info if the batchMode option didn't change
   {
      // Second element of pair tracks whether this function has already been called
      static std::pair<RooFit::BatchModeOption, bool> lastBatchMode;
      if (lastBatchMode.second && lastBatchMode.first == batchMode)
         return;
      lastBatchMode = {batchMode, true};
   }

   auto log = [](std::string_view message) {
      oocxcoutI(static_cast<RooAbsArg *>(nullptr), Fitting) << message << std::endl;
   };

   if (batchMode == RooFit::BatchModeOption::Cuda && !RooBatchCompute::hasCuda()) {
      throw std::runtime_error(std::string("In: ") + __func__ + "(), " + __FILE__ + ":" + __LINE__ +
                               ": Cuda implementation of the computing library is not available\n");
   }
   if (RooBatchCompute::cpuArchitecture() == RooBatchCompute::Architecture::GENERIC) {
      log("using generic CPU library compiled with no vectorizations");
   } else {
      log(std::string("using CPU computation library compiled with -m") + RooBatchCompute::cpuArchitectureName());
   }
   if (batchMode == RooFit::BatchModeOption::Cuda) {
      log("using CUDA computation library");
   }
}

} // namespace

/// A struct used by the RooFitDriver to store information on the RooAbsArgs in
/// the computation graph.
struct NodeInfo {

   bool isScalar() const { return outputSize == 1; }
#ifdef R__HAS_CUDA
   bool computeInGPU() const { return (absArg->isReducerNode() || !isScalar()) && absArg->canComputeBatchWithCuda(); }
#endif

   RooAbsArg *absArg = nullptr;
   std::unique_ptr<Detail::AbsBuffer> buffer;
   std::size_t iNode = 0;
#ifdef R__HAS_CUDA
   int remClients = 0;
   int remServers = 0;
   std::unique_ptr<RooFit::Detail::CudaInterface::CudaEvent> event;
   std::unique_ptr<RooFit::Detail::CudaInterface::CudaStream> stream;
   bool copyAfterEvaluation = false;
   bool hasLogged = false;
#endif
   bool fromDataset = false;
   bool isVariable = false;
   bool isDirty = true;
   bool isCategory = false;
   std::size_t outputSize = 1;
   std::size_t lastSetValCount = std::numeric_limits<std::size_t>::max();
   double scalarBuffer = 0.0;
   std::vector<NodeInfo *> serverInfos;
   std::vector<NodeInfo *> clientInfos;
};

/// Construct a new RooFitDriver. The constructor analyzes and saves metadata about the graph,
/// useful for the evaluation of it that will be done later. In case the CUDA mode is selected,
/// there's also some CUDA-related initialization.
///
/// \param[in] absReal The RooAbsReal object that sits on top of the
///            computation graph that we want to evaluate.
/// \param[in] batchMode The computation mode, accepted values are
///            `RooBatchCompute::Cpu` and `RooBatchCompute::Cuda`.
RooFitDriver::RooFitDriver(const RooAbsReal &absReal, RooFit::BatchModeOption batchMode)
   : _topNode{const_cast<RooAbsReal &>(absReal)}, _batchMode{batchMode}
{
#ifndef R__HAS_CUDA
   if (_batchMode == RooFit::BatchModeOption::Cuda) {
      throw std::runtime_error(
         "Can't create RooFitDriver in CUDA mode because ROOT was compiled without CUDA support!");
   }
#endif
   // Some checks and logging of used architectures
   logArchitectureInfo(_batchMode);

   RooArgSet serverSet;
   RooHelpers::getSortedComputationGraph(topNode(), serverSet);

   _dataMapCPU.resize(serverSet.size());
#ifdef R__HAS_CUDA
   _dataMapCUDA.resize(serverSet.size());
#endif

   std::map<RooFit::Detail::DataKey, NodeInfo *> nodeInfos;

   // Fill the ordered nodes list and initialize the node info structs.
   _nodes.resize(serverSet.size());
   std::size_t iNode = 0;
   for (RooAbsArg *arg : serverSet) {

      auto &nodeInfo = _nodes[iNode];
      nodeInfo.absArg = arg;
      nodeInfo.iNode = iNode;
      nodeInfos[arg] = &nodeInfo;

      if (dynamic_cast<RooRealVar const *>(arg)) {
         nodeInfo.isVariable = true;
      } else {
         arg->setDataToken(iNode);
      }
      if (dynamic_cast<RooAbsCategory const *>(arg)) {
         nodeInfo.isCategory = true;
      }

      ++iNode;
   }

   for (NodeInfo &info : _nodes) {
      info.serverInfos.reserve(info.absArg->servers().size());
      for (RooAbsArg *server : info.absArg->servers()) {
         if (server->isValueServer(*info.absArg)) {
            auto *serverInfo = nodeInfos.at(server);
            info.serverInfos.emplace_back(serverInfo);
            serverInfo->clientInfos.emplace_back(&info);
         }
      }
   }

   syncDataTokens();

#ifdef R__HAS_CUDA
   if (_batchMode == RooFit::BatchModeOption::Cuda) {
      // create events and streams for every node
      for (auto &info : _nodes) {
         info.event = std::make_unique<CudaInterface::CudaEvent>(false);
         info.stream = std::make_unique<CudaInterface::CudaStream>();
         RooBatchCompute::Config cfg;
         cfg.setCudaStream(info.stream.get());
         _dataMapCUDA.setConfig(info.absArg, cfg);
      }
   }
#endif
}

/// If there are servers with the same name that got de-duplicated in the
/// `_nodes` list, we need to set their data tokens too. We find such nodes by
/// visiting the servers of every known node.
void RooFitDriver::syncDataTokens()
{
   for (NodeInfo &info : _nodes) {
      std::size_t iValueServer = 0;
      for (RooAbsArg *server : info.absArg->servers()) {
         if (server->isValueServer(*info.absArg)) {
            auto *knownServer = info.serverInfos[iValueServer]->absArg;
            if (knownServer->hasDataToken()) {
               server->setDataToken(knownServer->dataToken());
            }
            ++iValueServer;
         }
      }
   }
}

void RooFitDriver::setData(RooAbsData const &data, std::string const &rangeName, RooSimultaneous const *simPdf,
                           bool skipZeroWeights, bool takeGlobalObservablesFromData)
{
   std::stack<std::vector<double>>{}.swap(_vectorBuffers);
   setData(RooFit::BatchModeDataHelpers::getDataSpans(data, rangeName, simPdf, skipZeroWeights,
                                                      takeGlobalObservablesFromData, _vectorBuffers));
}

void RooFitDriver::setData(DataSpansMap const &dataSpans)
{
   auto outputSizeMap = RooFit::BatchModeDataHelpers::determineOutputSizes(topNode(), dataSpans);

   // Iterate over the given data spans and add them to the data map. Check if
   // they are used in the computation graph. If yes, add the span to the data
   // map and set the node info accordingly.
#ifdef R__HAS_CUDA
   std::size_t totalSize = 0;
#endif
   std::size_t iNode = 0;
   for (auto &info : _nodes) {
      info.buffer.reset();
      auto found = dataSpans.find(info.absArg->namePtr());
      if (found != dataSpans.end()) {
         info.absArg->setDataToken(iNode);
         _dataMapCPU.set(info.absArg, found->second);
         info.fromDataset = true;
         info.isDirty = false;
#ifdef R__HAS_CUDA
         totalSize += found->second.size();
#endif
      } else {
         info.fromDataset = false;
         info.isDirty = true;
      }
      ++iNode;
   }

   syncDataTokens();

   for (auto &info : _nodes) {
      info.outputSize = outputSizeMap.at(info.absArg);

      // In principle we don't need dirty flag propagation because the driver
      // takes care of deciding which node needs to be re-evaluated. However,
      // disabling it also for scalar mode results in very long fitting times
      // for specific models (test 14 in stressRooFit), which still needs to be
      // understood. TODO.
      if (!info.isScalar()) {
         setOperMode(info.absArg, RooAbsArg::ADirty);
      }
   }

#ifdef R__HAS_CUDA
   // Extra steps for initializing in cuda mode
   if (_batchMode != RooFit::BatchModeOption::Cuda)
      return;

   // copy observable data to the GPU
   // TODO: use separate buffers here
   _cudaMemDataset = std::make_unique<CudaInterface::DeviceArray<double>>(totalSize);
   size_t idx = 0;
   for (auto &info : _nodes) {
      if (!info.fromDataset)
         continue;
      std::size_t size = info.outputSize;
      if (size == 1) {
         // Scalar observables from the data don't need to be copied to the GPU
         _dataMapCUDA.set(info.absArg, _dataMapCPU.at(info.absArg));
      } else {
         _dataMapCUDA.set(info.absArg, {_cudaMemDataset->data() + idx, size});
         CudaInterface::copyHostToDevice(_dataMapCPU.at(info.absArg).data(), _cudaMemDataset->data() + idx, size);
         idx += size;
      }
   }

   markGPUNodes();
#endif // R__HAS_CUDA
}

RooFitDriver::~RooFitDriver()
{
   for (auto &info : _nodes) {
      info.absArg->resetDataToken();
   }
}

std::vector<double> RooFitDriver::getValues()
{
   getVal();
   // We copy the data to the output vector
   auto dataSpan = _dataMapCPU.at(&topNode());
   std::vector<double> out;
   out.reserve(dataSpan.size());
   for (auto const &x : dataSpan) {
      out.push_back(x);
   }
   return out;
}

void RooFitDriver::computeCPUNode(const RooAbsArg *node, NodeInfo &info)
{
   using namespace Detail;

   auto nodeAbsReal = static_cast<RooAbsReal const *>(node);

   const std::size_t nOut = info.outputSize;

   double *buffer = nullptr;
   if (nOut == 1) {
      buffer = &info.scalarBuffer;
#ifdef R__HAS_CUDA
      if (_batchMode == RooFit::BatchModeOption::Cuda) {
         _dataMapCUDA.set(node, {buffer, nOut});
      }
#endif
   } else {
#ifdef R__HAS_CUDA
      if (!info.hasLogged && _batchMode == RooFit::BatchModeOption::Cuda) {
         RooAbsArg const &arg = *info.absArg;
         oocoutI(&arg, FastEvaluations) << "The argument " << arg.ClassName() << "::" << arg.GetName()
                                        << " could not be evaluated on the GPU because the class doesn't support it. "
                                           "Consider requesting or implementing it to benefit from a speed up."
                                        << std::endl;
         info.hasLogged = true;
      }
#endif
      if (!info.buffer) {
#ifdef R__HAS_CUDA
         info.buffer = info.copyAfterEvaluation ? _bufferManager.makePinnedBuffer(nOut, info.stream.get())
                                                : _bufferManager.makeCpuBuffer(nOut);
#else
         info.buffer = _bufferManager.makeCpuBuffer(nOut);
#endif
      }
      buffer = info.buffer->cpuWritePtr();
   }
   _dataMapCPU.set(node, {buffer, nOut});
   nodeAbsReal->computeBatch(buffer, nOut, _dataMapCPU);
#ifdef R__HAS_CUDA
   if (info.copyAfterEvaluation) {
      _dataMapCUDA.set(node, {info.buffer->gpuReadPtr(), nOut});
      if (info.event) {
         CudaInterface::cudaEventRecord(*info.event, *info.stream);
      }
   }
#endif
}

/// Process a variable in the computation graph. This is a separate non-inlined
/// function such that we can see in performance profiles how long this takes.
void RooFitDriver::processVariable(NodeInfo &nodeInfo)
{
   RooAbsArg *node = nodeInfo.absArg;
   auto *var = static_cast<RooRealVar const *>(node);
   if (nodeInfo.lastSetValCount != var->valueResetCounter()) {
      nodeInfo.lastSetValCount = var->valueResetCounter();
      for (NodeInfo *clientInfo : nodeInfo.clientInfos) {
         clientInfo->isDirty = true;
      }
      computeCPUNode(node, nodeInfo);
      nodeInfo.isDirty = false;
   }
}

/// Flags all the clients of a given node dirty. This is a separate non-inlined
/// function such that we can see in performance profiles how long this takes.
void RooFitDriver::setClientsDirty(NodeInfo &nodeInfo)
{
   for (NodeInfo *clientInfo : nodeInfo.clientInfos) {
      clientInfo->isDirty = true;
   }
}

/// Returns the value of the top node in the computation graph
double RooFitDriver::getVal()
{
   ++_getValInvocations;

#ifdef R__HAS_CUDA
   if (_batchMode == RooFit::BatchModeOption::Cuda) {
      return getValHeterogeneous();
   }
#endif

   for (auto &nodeInfo : _nodes) {
      if (!nodeInfo.fromDataset) {
         if (nodeInfo.isVariable) {
            processVariable(nodeInfo);
         } else {
            if (nodeInfo.isDirty) {
               setClientsDirty(nodeInfo);
               computeCPUNode(nodeInfo.absArg, nodeInfo);
               nodeInfo.isDirty = false;
            }
         }
      }
   }

   // return the final value
   return _dataMapCPU.at(&topNode())[0];
}

#ifdef R__HAS_CUDA

/// Returns the value of the top node in the computation graph
double RooFitDriver::getValHeterogeneous()
{
   for (auto &info : _nodes) {
      info.remClients = info.clientInfos.size();
      info.remServers = info.serverInfos.size();
      info.buffer.reset();
   }

   // find initial GPU nodes and assign them to GPU
   for (auto &info : _nodes) {
      if (info.remServers == 0 && info.computeInGPU()) {
         assignToGPU(info);
      }
   }

   NodeInfo const &topNodeInfo = _nodes.back();
   while (topNodeInfo.remServers != -2) {
      // find finished GPU nodes
      for (auto &info : _nodes) {
         if (info.remServers == -1 && !info.stream->isActive()) {
            info.remServers = -2;
            // Decrement number of remaining servers for clients and start GPU computations
            for (auto *infoClient : info.clientInfos) {
               --infoClient->remServers;
               if (infoClient->computeInGPU() && infoClient->remServers == 0) {
                  assignToGPU(*infoClient);
               }
            }
            for (auto *serverInfo : info.serverInfos) {
               /// Check the servers of a node that has been computed and release it's resources
               /// if they are no longer needed.
               if (--serverInfo->remClients == 0) {
                  serverInfo->buffer.reset();
               }
            }
         }
      }

      // find next CPU node
      auto it = _nodes.begin();
      for (; it != _nodes.end(); it++) {
         if (it->remServers == 0 && !it->computeInGPU())
            break;
      }

      // if no CPU node available sleep for a while to save CPU usage
      if (it == _nodes.end()) {
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
         continue;
      }

      // compute next CPU node
      NodeInfo &info = *it;
      RooAbsArg const *node = info.absArg;
      info.remServers = -2; // so that it doesn't get picked again

      if (!info.fromDataset) {
         computeCPUNode(node, info);
      }

      // Assign the clients that are computed on the GPU
      for (auto *infoClient : info.clientInfos) {
         if (--infoClient->remServers == 0 && infoClient->computeInGPU()) {
            assignToGPU(*infoClient);
         }
      }
      for (auto *serverInfo : info.serverInfos) {
         /// Check the servers of a node that has been computed and release it's resources
         /// if they are no longer needed.
         if (--serverInfo->remClients == 0) {
            serverInfo->buffer.reset();
         }
      }
   }

   // return the final value
   return _dataMapCPU.at(&topNode())[0];
}

/// Assign a node to be computed in the GPU. Scan it's clients and also assign them
/// in case they only depend on GPU nodes.
void RooFitDriver::assignToGPU(NodeInfo &info)
{
   auto node = static_cast<RooAbsReal const *>(info.absArg);

   info.remServers = -1;
   // wait for every server to finish
   for (auto *infoServer : info.serverInfos) {
      if (infoServer->event)
         info.stream->waitForEvent(*infoServer->event);
   }

   const std::size_t nOut = info.outputSize;

   double *buffer = nullptr;
   if (nOut == 1) {
      buffer = &info.scalarBuffer;
      _dataMapCPU.set(node, {buffer, nOut});
   } else {
      info.buffer = info.copyAfterEvaluation ? _bufferManager.makePinnedBuffer(nOut, info.stream.get())
                                             : _bufferManager.makeGpuBuffer(nOut);
      buffer = info.buffer->gpuWritePtr();
   }
   _dataMapCUDA.set(node, {buffer, nOut});
   node->computeBatch(buffer, nOut, _dataMapCUDA);
   CudaInterface::cudaEventRecord(*info.event, *info.stream);
   if (info.copyAfterEvaluation) {
      _dataMapCPU.set(node, {info.buffer->cpuReadPtr(), nOut});
   }
}

/// Decides which nodes are assigned to the GPU in a CUDA fit.
void RooFitDriver::markGPUNodes()
{
   for (auto &info : _nodes) {
      info.copyAfterEvaluation = false;
      // scalar nodes don't need copying
      if (!info.isScalar()) {
         for (auto *clientInfo : info.clientInfos) {
            if (info.computeInGPU() != clientInfo->computeInGPU()) {
               info.copyAfterEvaluation = true;
               break;
            }
         }
      }
   }
}

#endif // R__HAS_CUDA

/// Temporarily change the operation mode of a RooAbsArg until the
/// RooFitDriver gets deleted.
void RooFitDriver::setOperMode(RooAbsArg *arg, RooAbsArg::OperMode opMode)
{
   if (opMode != arg->operMode()) {
      _changeOperModeRAIIs.emplace(arg, opMode);
   }
}

RooAbsReal &RooFitDriver::topNode() const
{
   return _topNode;
}

void RooFitDriver::print(std::ostream &os) const
{
   std::cout << "--- RooFit BatchMode evaluation ---\n";

   std::vector<int> widths{9, 37, 20, 9, 10, 20};

   auto printElement = [&](int iCol, auto const &t) {
      const char separator = ' ';
      os << separator << std::left << std::setw(widths[iCol]) << std::setfill(separator) << t;
      os << "|";
   };

   auto printHorizontalRow = [&]() {
      int n = 0;
      for (int w : widths) {
         n += w + 2;
      }
      for (int i = 0; i < n; i++) {
         os << '-';
      }
      os << "|\n";
   };

   printHorizontalRow();

   os << "|";
   printElement(0, "Index");
   printElement(1, "Name");
   printElement(2, "Class");
   printElement(3, "Size");
   printElement(4, "From Data");
   printElement(5, "1st value");
   std::cout << "\n";

   printHorizontalRow();

   for (std::size_t iNode = 0; iNode < _nodes.size(); ++iNode) {
      auto &nodeInfo = _nodes[iNode];
      RooAbsArg *node = nodeInfo.absArg;

      auto span = _dataMapCPU.at(node);

      os << "|";
      printElement(0, iNode);
      printElement(1, node->GetName());
      printElement(2, node->ClassName());
      printElement(3, nodeInfo.outputSize);
      printElement(4, nodeInfo.fromDataset);
      printElement(5, span[0]);

      std::cout << "\n";
   }

   printHorizontalRow();
}

/// Gets all the parameters of the RooAbsReal. This is in principle not
/// necessary, because we can always ask the RooAbsReal itself, but the
/// RooFitDriver has the cached information to get the answer quicker.
/// Therefore, this is not meant to be used in general, just where it matters.
/// \warning If we find another solution to get the parameters efficiently,
/// this function might be removed without notice.
RooArgSet RooFitDriver::getParameters() const
{
   RooArgSet parameters;
   for (auto &nodeInfo : _nodes) {
      if (!nodeInfo.fromDataset && nodeInfo.isVariable) {
         parameters.add(*nodeInfo.absArg);
      }
   }
   // Just like in RooAbsArg::getParameters(), we sort the parameters alphabetically.
   parameters.sort();
   return parameters;
}

RooAbsRealWrapper::RooAbsRealWrapper(std::unique_ptr<RooFitDriver> driver, std::string const &rangeName,
                                     RooSimultaneous const *simPdf, bool takeGlobalObservablesFromData)
   : RooAbsReal{"RooFitDriverWrapper", "RooFitDriverWrapper"},
     _driver{std::move(driver)},
     _topNode("topNode", "top node", this, _driver->topNode()),
     _rangeName{rangeName},
     _simPdf{simPdf},
     _takeGlobalObservablesFromData{takeGlobalObservablesFromData}
{
}

RooAbsRealWrapper::RooAbsRealWrapper(const RooAbsRealWrapper &other, const char *name)
   : RooAbsReal{other, name},
     _driver{other._driver},
     _topNode("topNode", this, other._topNode),
     _data{other._data},
     _rangeName{other._rangeName},
     _simPdf{other._simPdf},
     _takeGlobalObservablesFromData{other._takeGlobalObservablesFromData}
{
}

bool RooAbsRealWrapper::getParameters(const RooArgSet *observables, RooArgSet &outputSet,
                                      bool /*stripDisconnected*/) const
{
   outputSet.add(_driver->getParameters());
   if (observables) {
      outputSet.remove(*observables);
   }
   // If we take the global observables as data, we have to return these as
   // parameters instead of the parameters in the model. Otherwise, the
   // constant parameters in the fit result that are global observables will
   // not have the right values.
   if (_takeGlobalObservablesFromData && _data->getGlobalObservables()) {
      outputSet.replace(*_data->getGlobalObservables());
   }
   return false;
}

bool RooAbsRealWrapper::setData(RooAbsData &data, bool /*cloneData*/)
{
   _data = &data;
   _driver->setData(*_data, _rangeName, _simPdf, /*skipZeroWeights=*/true, _takeGlobalObservablesFromData);
   return true;
}

} // namespace Experimental
} // namespace ROOT
