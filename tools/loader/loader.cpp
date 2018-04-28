/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "glow/Base/Image.h"
#include "glow/Base/Tensor.h"
#include "glow/ExecutionEngine/ExecutionEngine.h"
#include "glow/IR/IR.h"
#include "glow/Importer/Caffe2.h"
#include "glow/Importer/ONNX.h"
#include "glow/Quantization/Serialization.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

using namespace glow;

enum class ImageNormalizationMode {
  k0to1,     // Values are in the range: 0 and 1.
  k0to256,   // Values are in the range: 0 and 256.
  k128to127, // Values are in the range: -128 .. 127
};

ImageNormalizationMode strToImageNormalizationMode(const std::string &str) {
  if (str == "0to1")
    return ImageNormalizationMode::k0to1;
  if (str == "0to256")
    return ImageNormalizationMode::k0to256;
  if (str == "128to127")
    return ImageNormalizationMode::k128to127;

  GLOW_ASSERT(false && "Unknown image format");
}

/// Convert the normalization to numeric floating poing ranges.
std::pair<float, float> normModeToRange(ImageNormalizationMode mode) {
  switch (mode) {
  case ImageNormalizationMode::k0to1:
    return {0., 1.0};
  case ImageNormalizationMode::k0to256:
    return {0., 256.0};
  case ImageNormalizationMode::k128to127:
    return {-128., 127.};
  }

  GLOW_ASSERT(false && "Unknown image format");
}

/// Loads and normalizes all PNGs into a tensor in the NCHW 3x224x224 format.
void loadImagesAndPreprocess(const llvm::cl::list<std::string> &filenames,
                             Tensor *result, ImageNormalizationMode normMode) {
  assert(filenames.size() > 0 &&
         "There must be at least one filename in filenames");
  auto range = normModeToRange(normMode);
  unsigned numImages = filenames.size();

  // Get first image's dimensions and check if grayscale or color.
  size_t imgHeight, imgWidth;
  bool isGray;
  std::tie(imgHeight, imgWidth, isGray) = getPngInfo(filenames[0].c_str());
  const size_t numChannels = isGray ? 1 : 3;

  // N x C x H x W
  result->reset(ElemKind::FloatTy,
                {numImages, numChannels, imgHeight, imgWidth});
  auto RH = result->getHandle<>();
  // We iterate over all the png files, reading them all into our result tensor
  // for processing
  for (unsigned n = 0; n < filenames.size(); n++) {
    Tensor localCopy;
    bool loadSuccess = !readPngImage(&localCopy, filenames[n].c_str(), range);
    GLOW_ASSERT(loadSuccess && "Error reading input image.");
    auto imageH = localCopy.getHandle<>();

    auto dims = localCopy.dims();
    assert((dims[0] == imgHeight && dims[1] == imgWidth) &&
           "All images must have the same Height and Width");
    assert(dims[2] == numChannels &&
           "All images must have the same number of channels");

    // Convert to BGR, as this is what imagenet models are expecting.
    for (unsigned z = 0; z < numChannels; z++) {
      for (unsigned y = 0; y < dims[1]; y++) {
        for (unsigned x = 0; x < dims[0]; x++) {
          RH.at({n, numChannels - 1 - z, x, y}) = (imageH.at({x, y, z}));
        }
      }
    }
  }
}

namespace {

llvm::cl::list<std::string>
    inputImageFilenames(llvm::cl::Positional,
                        llvm::cl::desc("<input image files>"),
                        llvm::cl::OneOrMore);

llvm::cl::OptionCategory modelInputCat("How to input the models",
                                       "These control the model paths");
llvm::cl::opt<std::string> caffe2NetDescFilenameOpt(
    "caffe2_network",
    llvm::cl::desc("Specify the Caffe2 network structure file"),
    llvm::cl::value_desc("caffe2NetDescFilename"), llvm::cl::cat(modelInputCat),
    llvm::cl::Optional);
llvm::cl::alias
    caffe2NetDescFilenameAOpt("n", llvm::cl::desc("Alias for -caffe2_network"),
                              llvm::cl::aliasopt(caffe2NetDescFilenameOpt),
                              llvm::cl::cat(modelInputCat));
llvm::cl::opt<std::string> caffe2NetWeightFilenameOpt(
    "caffe2_weight", llvm::cl::desc("Specify the Caffe2 network weight file"),
    llvm::cl::value_desc("caffe2NetWeightFilename"),
    llvm::cl::cat(modelInputCat), llvm::cl::Optional);
llvm::cl::alias
    caffe2NetWeightFilenameAOpt("w", llvm::cl::desc("Alias for -caffe2_weight"),
                                llvm::cl::aliasopt(caffe2NetWeightFilenameOpt),
                                llvm::cl::cat(modelInputCat));
llvm::cl::opt<std::string> caffe2NetDirectoryOpt(
    "caffe2_directory",
    llvm::cl::desc("Specify the directory with the Caffe2 network structure "
                   "<predict_net.pb> and weight <init_net.pb> files"),
    llvm::cl::value_desc("caffe2NetDirectory"), llvm::cl::cat(modelInputCat),
    llvm::cl::Optional);
llvm::cl::alias
    caffe2NetDirectoryA("d", llvm::cl::desc("Alias for -caffe2_directory"),
                        llvm::cl::aliasopt(caffe2NetDirectoryOpt),
                        llvm::cl::cat(modelInputCat));
llvm::cl::opt<std::string>
    onnxModelFilenameOpt("onnx", llvm::cl::desc("Specify the ONNX model file"),
                         llvm::cl::value_desc("onnxModelFilename"),
                         llvm::cl::cat(modelInputCat), llvm::cl::Optional);
llvm::cl::alias onnxModelFilenameAOpt("o", llvm::cl::desc("Alias for -onnx"),
                                      llvm::cl::aliasopt(onnxModelFilenameOpt),
                                      llvm::cl::cat(modelInputCat));

llvm::cl::OptionCategory
    modelExportCat("How to export the Glow Intermediate Representation/Graphs",
                   "These options are for debugging the "
                   "graphs by writing the IR/Graphs to "
                   "given files/stdout");

llvm::cl::opt<std::string> dumpGraphDAGFileOpt(
    "dumpGraphDAG",
    llvm::cl::desc("Specify the file to export the Graph in DOT format"),
    llvm::cl::value_desc("file.dot"), llvm::cl::cat(modelExportCat));

llvm::cl::opt<bool> dumpGraphOpt("dumpGraph",
                                 llvm::cl::desc("Prints Graph to stdout"),
                                 llvm::cl::cat(modelExportCat));

llvm::cl::opt<std::string> dumpIRDAGFileOpt(
    "dumpIRDAG",
    llvm::cl::desc("Specify the file to export the IR in DOT format"),
    llvm::cl::value_desc("file.dot"), llvm::cl::cat(modelExportCat));

llvm::cl::opt<bool> dumpIROpt("dumpIR", llvm::cl::desc("Prints IR to stdout"),
                              llvm::cl::cat(modelExportCat));

llvm::cl::OptionCategory loaderCat("Image Loader Options");

llvm::cl::opt<ImageNormalizationMode> imageMode(
    "image_mode", llvm::cl::desc("Specify the image mode:"), llvm::cl::Required,
    llvm::cl::cat(loaderCat),
    llvm::cl::values(clEnumValN(ImageNormalizationMode::k0to1, "0to1",
                                "Values are in the range: 0 and 1"),
                     clEnumValN(ImageNormalizationMode::k0to256, "0to256",
                                "Values are in the range: 0 and 256"),
                     clEnumValN(ImageNormalizationMode::k128to127, "128to127",
                                "Values are in the range: -128 .. 127")));
llvm::cl::alias imageModeA("i", llvm::cl::desc("Alias for -image_mode"),
                           llvm::cl::aliasopt(imageMode),
                           llvm::cl::cat(loaderCat));

llvm::cl::opt<bool>
    verbose("verbose",
            llvm::cl::desc("Specify whether to run with verbose output"),
            llvm::cl::Optional, llvm::cl::cat(loaderCat));

llvm::cl::opt<bool>
    timeOpt("time",
            llvm::cl::desc("Print timer output to stderr detailing how long it "
                           "takes for the program to execute"),
            llvm::cl::Optional, llvm::cl::cat(loaderCat));

llvm::cl::opt<unsigned> iterationsOpt(
    "iterations", llvm::cl::desc("Number of iterations to perform"),
    llvm::cl::Optional, llvm::cl::init(1), llvm::cl::cat(loaderCat));

llvm::cl::opt<std::string> dumpProfileFileOpt(
    "dump_profile",
    llvm::cl::desc("Perform quantization profiling for a given graph "
                   "and dump result to the file."),
    llvm::cl::value_desc("profile.yaml"), llvm::cl::Optional,
    llvm::cl::cat(loaderCat));

llvm::cl::opt<std::string> loadProfileFileOpt(
    "load_profile",
    llvm::cl::desc("Load quantization profile file and quantize the graph"),
    llvm::cl::value_desc("profile.yaml"), llvm::cl::Optional,
    llvm::cl::cat(loaderCat));

llvm::cl::opt<BackendKind> ExecutionBackend(
    llvm::cl::desc("Backend to use:"),
    llvm::cl::values(clEnumValN(BackendKind::Interpreter, "interpreter",
                                "Use interpreter"),
                     clEnumValN(BackendKind::CPU, "cpu", "Use CPU"),
                     clEnumValN(BackendKind::OpenCL, "opencl", "Use OpenCL")),
    llvm::cl::init(BackendKind::Interpreter), llvm::cl::cat(loaderCat));

/// Emit a bundle into the specified output directory.
llvm::cl::opt<std::string>
    emitBundle("emit-bundle",
               llvm::cl::desc("Output directory for the bundle serialization"),
               llvm::cl::cat(loaderCat));
} // namespace

static bool commandLineIsInvalid() {
  if (!dumpProfileFileOpt.empty() && !loadProfileFileOpt.empty()) {
    llvm::errs() << "loader: the -" << dumpProfileFileOpt.ArgStr << " and -"
                 << loadProfileFileOpt.ArgStr
                 << " options may not be specified together.\n";
    return true;
  }
  return false;
}

int main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(
      argc, argv,
      " The Glow compiler\n\n"
      "Glow is a compiler for neural network accelerators.\n");

  if (commandLineIsInvalid()) {
    return 1;
  }

  Tensor data;
  Tensor expectedSoftmax(ElemKind::IndexTy, {1, 1});

  loadImagesAndPreprocess(inputImageFilenames, &data, imageMode);

  if (!caffe2NetDirectoryOpt.empty()) {
    caffe2NetDescFilenameOpt.setValue(caffe2NetDirectoryOpt +
                                      "/predict_net.pb");
    caffe2NetWeightFilenameOpt.setValue(caffe2NetDirectoryOpt + "/init_net.pb");
  }
  std::string modelPath = !caffe2NetDescFilenameOpt.empty()
                              ? caffe2NetDescFilenameOpt
                              : onnxModelFilenameOpt;

  ExecutionEngine EE(ExecutionBackend);
  Function *F = EE.getModule().createFunction(modelPath);
  SaveNode *SM;
  Variable *i0;
  Variable *i1;
  if (!caffe2NetDescFilenameOpt.empty()) {
    caffe2ModelLoader LD(caffe2NetDescFilenameOpt, caffe2NetWeightFilenameOpt,
                         {"data", "gpu_0/data", "softmax_expected"},
                         {&data, &data, &expectedSoftmax}, *F);
    SM = LD.getRoot();
    i0 = llvm::cast<Variable>(LD.getOrCreateNodeByName("gpu_0/data"));
    i1 = llvm::cast<Variable>(LD.getOrCreateNodeByName("data"));
  } else {
    assert(inputImageFilenames.size() == 1 &&
           "Batch image inference is not supported by ONNX models.");
    ONNXModelLoader LD(onnxModelFilenameOpt,
                       {"data_0", "gpu_0/data_0", "softmax_expected"},
                       {&data, &data, &expectedSoftmax}, *F);
    SM = LD.getRoot();
    i0 = llvm::cast<Variable>(LD.getOrCreateNodeByName("gpu_0/data_0"));
    i1 = llvm::cast<Variable>(LD.getOrCreateNodeByName("data_0"));
  }

  assert(i0->getVisibilityKind() == VisibilityKind::Public);
  assert(i1->getVisibilityKind() == VisibilityKind::Public);

  // Handle the request to profile the graph in preperation for quantization.
  if (!dumpProfileFileOpt.empty()) {
    // Perform the high-level optimizations before instrumenting the graph. This
    // optimization phase will remove stuff like repetitive transpose operations
    // perform CSE, etc.
    ::optimize(F, glow::CompilationMode::Infer);

    // Instrument the graph to capture profiles for nodes' outputs.
    ::profileQuantization(F);
  }

  // Load the quantization profile and transform the graph.
  if (!loadProfileFileOpt.empty()) {
    // The profiled graph was optimized before it was instrumentated. In this
    // part of the code we repeat the same transformation in order to create
    // the same graph structure.
    ::optimize(F, glow::CompilationMode::Infer);

    auto quantizationInfos = deserializeFromYaml(loadProfileFileOpt);

    // Quantize the graph based on the captured profile.
    quantization::generateQuantizedGraph(EE, F, quantizationInfos);
  }

  if (!emitBundle.empty()) {
    // Emit IR for the graph, compile it and save as a bundle.
    EE.save(CompilationMode::Infer, F, emitBundle);
  } else {
    // Emit IR for the graph and compile it.
    EE.compile(CompilationMode::Infer, F);
  }

  if (dumpGraphOpt) {
    F->dump();
  }
  if (!dumpGraphDAGFileOpt.empty()) {
    F->dumpDAG(dumpGraphDAGFileOpt.c_str());
  }
  if (dumpIROpt) {
    EE.getIR().dump();
  }
  if (!dumpIRDAGFileOpt.empty()) {
    EE.getIR().dumpDAG(dumpIRDAGFileOpt.c_str());
  }

  // No inference is performed in the bundle generation mode.
  if (!emitBundle.empty()) {
    return 0;
  }

  llvm::Timer timer("Infer", "Infer");
  if (timeOpt) {
    timer.startTimer();
  }
  for (unsigned i = 0; i < iterationsOpt; i++) {
    EE.run({i0, i1}, {&data, &data});
  }
  if (timeOpt) {
    timer.stopTimer();
    llvm::outs() << llvm::formatv("Wall time per iteration (s): {0:f4}\n",
                                  timer.getTotalTime().getWallTime() /
                                      iterationsOpt);
  }

  if (!dumpProfileFileOpt.empty()) {
    std::vector<NodeQuantizationInfo> QI =
        quantization::generateNodeQuantizationInfos(F);
    serializeToYaml(dumpProfileFileOpt, QI);
  }

  Tensor &res = SM->getVariable()->getPayload();
  auto H = res.getHandle<>();
  llvm::outs() << "Model: " << modelPath << "\n";
  for (unsigned i = 0; i < inputImageFilenames.size(); i++) {
    Tensor slice = H.extractSlice(i);
    auto SH = slice.getHandle<>();
    llvm::outs() << " File: " << inputImageFilenames[i]
                 << " Result:" << SH.minMaxArg().second << "\n";
  }
  return 0;
}
