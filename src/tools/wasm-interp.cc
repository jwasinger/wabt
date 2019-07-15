/*
 * Copyright 2016 WebAssembly Community Group participants
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

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <iostream>

#include "src/binary-reader.h"
#include "src/cast.h"
#include "src/error-formatter.h"
#include "src/feature.h"
#include "src/interp/binary-reader-interp.h"
#include "src/interp/interp.h"
#include "src/literal.h"
#include "src/option-parser.h"
#include "src/resolve-names.h"
#include "src/stream.h"
#include "src/validator.h"
#include "src/wast-lexer.h"
#include "src/wast-parser.h"

using namespace wabt;
using namespace wabt::interp;

using chrono_clock = std::chrono::high_resolution_clock;

static int s_verbose;
static const char* s_infile;
static Thread::Options s_thread_options;
static Stream* s_trace_stream;
static bool s_run_all_exports;
static bool s_host_print;
static Features s_features;

static std::unique_ptr<FileStream> s_log_stream;
static std::unique_ptr<FileStream> s_stdout_stream;

enum class RunVerbosity {
  Quiet = 0,
  Verbose = 1,
};

static const char s_description[] =
    R"(  read a file in the wasm binary format, and run in it a stack-based
  interpreter.

examples:
  # parse binary file test.wasm, and type-check it
  $ wasm-interp test.wasm

  # parse test.wasm and run all its exported functions
  $ wasm-interp test.wasm --run-all-exports

  # parse test.wasm, run the exported functions and trace the output
  $ wasm-interp test.wasm --run-all-exports --trace

  # parse test.wasm and run all its exported functions, setting the
  # value stack size to 100 elements
  $ wasm-interp test.wasm -V 100 --run-all-exports
)";

static void ParseOptions(int argc, char** argv) {
  OptionParser parser("wasm-interp", s_description);

  parser.AddOption('v', "verbose", "Use multiple times for more info", []() {
    s_verbose++;
    s_log_stream = FileStream::CreateStdout();
  });
  parser.AddHelpOption();
  s_features.AddOptions(&parser);
  parser.AddOption('V', "value-stack-size", "SIZE",
                   "Size in elements of the value stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.value_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('C', "call-stack-size", "SIZE",
                   "Size in elements of the call stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.call_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('t', "trace", "Trace execution",
                   []() { s_trace_stream = s_stdout_stream.get(); });
  parser.AddOption(
      "run-all-exports",
      "Run all the exported functions, in order. Useful for testing",
      []() { s_run_all_exports = true; });
  parser.AddOption("host-print",
                   "Include an importable function named \"host.print\" for "
                   "printing to stdout",
                   []() { s_host_print = true; });

  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) { s_infile = argument; });
  parser.Parse(argc, argv);
}

static void RunAllExports(interp::Module* module,
                          Executor* executor,
                          RunVerbosity verbose) {
  TypedValues args;
  TypedValues results;
  for (const interp::Export& export_ : module->exports) {
    if (export_.kind != ExternalKind::Func) {
      continue;
    }
    ExecResult exec_result = executor->RunExport(&export_, args);
    if (verbose == RunVerbosity::Verbose) {
      WriteCall(s_stdout_stream.get(), string_view(), export_.name, args,
                exec_result.values, exec_result.result);
    }
  }
}

static wabt::Result ReadModule(const char* module_filename,
                               Environment* env,
                               Errors* errors,
                               DefinedModule** out_module) {
  wabt::Result result;
  std::vector<uint8_t> file_data;

  *out_module = nullptr;

  result = ReadFile(module_filename, &file_data);
  if (Succeeded(result)) {
    const bool kReadDebugNames = true;
    const bool kStopOnFirstError = true;
    const bool kFailOnCustomSectionError = true;
    ReadBinaryOptions options(s_features, s_log_stream.get(), kReadDebugNames,
                              kStopOnFirstError, kFailOnCustomSectionError);
    result = ReadBinaryInterp(env, file_data.data(), file_data.size(), options,
                              errors, out_module);

    if (Succeeded(result)) {
      if (s_verbose) {
        env->DisassembleModule(s_stdout_stream.get(), *out_module);
      }
    }
  }
  return result;
}

static interp::Result PrintCallback(const HostFunc* func,
                                    const interp::FuncSignature* sig,
                                    const TypedValues& args,
                                    TypedValues& results) {
  printf("called host ");
  WriteCall(s_stdout_stream.get(), func->module_name, func->field_name, args,
            results, interp::Result::Ok);
  return interp::Result::Ok;
}

// function not implemented here, just need a stub so the import is valid
// the implementation is in interp.cc under `case Opcode::EwasmCall`
static interp::Result EwasmHostFunc(const HostFunc* func,
                                    const interp::FuncSignature* sig,
                                    const TypedValues& args,
                                    TypedValues& results) {
  return interp::Result::Ok;
}

static interp::Result EwasmAddMod(const HostFunc* func,
                                    const interp::FuncSignature* sig,
                                    const TypedValues& args,
                                    TypedValues& results) {
  return interp::Result::Ok;
}

static interp::Result EwasmSubMod(const HostFunc* func,
                                    const interp::FuncSignature* sig,
                                    const TypedValues& args,
                                    TypedValues& results) {
  return interp::Result::Ok;
}

static interp::Result EwasMulModMont(const HostFunc* func,
                                    const interp::FuncSignature* sig,
                                    const TypedValues& args,
                                    TypedValues& results) {
  return interp::Result::Ok;
}


static interp::Result EthereumFinish(const HostFunc* func,
                                    const interp::FuncSignature* sig,
                                    const TypedValues& args,
                                    TypedValues& results) {
  //printf("EthereumFinish");
  return interp::Result::Ok;
}

static void InitEnvironment(Environment* env) {
  if (s_host_print) {
    HostModule* host_module = env->AppendHostModule("host");
    host_module->on_unknown_func_export =
        [](Environment* env, HostModule* host_module, string_view name,
           Index sig_index) -> Index {
      if (name != "print") {
        return kInvalidIndex;
      }

      std::pair<HostFunc*, Index> pair =
          host_module->AppendFuncExport(name, sig_index, PrintCallback);
      return pair.second;
    };
  }

  // this is just here so the import is valid
  HostModule* host_module_ewasm = env->AppendHostModule("ewasm");
  //host_module_ewasm->AppendFuncExport("ewasmHostFunc", {{Type::I32, Type::I32}, {Type::I32}}, EwasmHostFunc);
  //host_module_debug->AppendFuncExport("ewasmHostFunc", {{Type::I32, Type::I32, Type::I32}, {}}, EwasmHostFunc);
  //host_module_debug->AppendFuncExport("ewasmHostFunc", {{}, {}}, EwasmHostFunc);

  //host_module_ewasm->AppendFuncExport("addmod256", {{Type::I32, Type::I32, Type::I32, Type::I32}, {}}, EwasmAddMod);
  //host_module_ewasm->AppendFuncExport("submod256", {{Type::I32, Type::I32, Type::I32, Type::I32}, {}}, EwasmSubMod);
  //host_module_ewasm->AppendFuncExport("mulmodmont256", {{Type::I32, Type::I32, Type::I32, Type::I32, Type::I32}, {}}, EwasMulModMont);

  host_module_ewasm->AppendFuncExport("addmodbn", {{Type::I32, Type::I32, Type::I32}, {}}, EwasmAddMod);
  host_module_ewasm->AppendFuncExport("submodbn", {{Type::I32, Type::I32, Type::I32}, {}}, EwasmSubMod);
  host_module_ewasm->AppendFuncExport("mulmodmontbn", {{Type::I32, Type::I32, Type::I32}, {}}, EwasMulModMont);

  /*
  host_module_ewasm->AppendFuncExport("addmodbn", {{Type::I32, Type::I32}, {Type::I32}}, EwasmAddMod);
  host_module_ewasm->AppendFuncExport("submodbn", {{Type::I32, Type::I32}, {Type::I32}}, EwasmSubMod);
  host_module_ewasm->AppendFuncExport("mulmodmontbn", {{Type::I32, Type::I32}, {Type::I32}}, EwasMulModMont);
  */

  //host_module_ewasm->AppendFuncExport("debugPrint", {{Type::I32, Type::I32, Type::I32}, {}}, EwasmDebugPrint);

  host_module_ewasm->AppendFuncExport(
    "debugPrint",
    {{Type::I32, Type::I32, Type::I32}, {}},
    [&env](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      printf("EwasmDebugPrint. \n");
      env->PrintBignumStack(args[0].value.i32, args[1].value.i32, args[2].value.i32);
      return interp::Result::Ok;
    }
  );


  host_module_ewasm->AppendFuncExport(
    "setBignumStack",
    {{Type::I32}, {}},
    [&env](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      //interface.mulmodmont256(args[0].value.i32, args[1].value.i32, args[2].value.i32, args[3].value.i32, args[4].value.i32);
      env->SetBignumStack(args[0].value.i32);
      return interp::Result::Ok;
    }
  );



  HostModule* host_module_ethereum = env->AppendHostModule("ethereum");
  host_module_ethereum->AppendFuncExport("finish", {{Type::I32, Type::I32}, {}}, EthereumFinish);
  
  
  

  /*
  host_module_env->AppendFuncExport("eth2_blockDataSize", {{}, {Type::I32}}, Eth2BlockDataSize);
  host_module_env->AppendFuncExport("eth2_blockDataCopy", {{Type::I32, Type::I32, Type::I32}, {}}, Eth2BlockDataCopy);
  host_module_env->AppendFuncExport("eth2_loadPreStateRoot", {{Type::I32}, {}}, Eth2LoadPreStateRoot);
  host_module_env->AppendFuncExport("eth2_savePostStateRoot", {{Type::I32}, {}}, Eth2SavePostStateRoot);
  */

  HostModule* host_module_env = env->AppendHostModule("env");

  host_module_env->AppendFuncExport(
    "eth2_loadPreStateRoot",
    {{Type::I32}, {}},
    [&env](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      //printf("eth2_loadPreStateRoot mem_pos: %llu\n", args[0].value.i32);
      return interp::Result::Ok;
    }
  );

  host_module_env->AppendFuncExport(
    "eth2_savePostStateRoot",
    {{Type::I32}, {}},
    [&env](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      //printf("eth2_savePostStateRoot. %llu\n", args[0].value.i32);
      // TODO: get 32 bytes of mem at position
      return interp::Result::Ok;
    }
  );


  host_module_env->AppendFuncExport(
    "eth2_blockDataSize",
    {{}, {Type::I32}},
    [&env](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      //printf("eth2_blockDataSize\n");

      results[0].set_i32(532);

      return interp::Result::Ok;
    }
  );

// block data size: 532

// block data:  f90211a042d5f0401d27bdae05452ce52bcee42a2c659c112d15b5f30b126b9ec24c68c0a045454ccb94e62efdbcd08423b7eef4cac43614025010c4312a745f67401588a5a02afb19069a247c89761e8eef700649c95743f1651e837c26aab6880228bedbf3a0305dd55390d2baea9157b4b1841bf1356b1187310bacc5eb848c34ce1391875aa0b53a79d67ee772cfdc8df03402e1400e7ff4df4e97423a6296c2b71a7649af1ca0dfaffef77c95e519bd9f2ef464c69545b5085106d767ed1dfcb2fef12d3f94f5a0a898ad01e272c350dd4a7954bd3eda10837ee79de6d7a99245a3b5d638ad51aea0d5476878fd37d082f47084404592dc5fe1f88911e25709862f078712806f0c00a0345ae4503837685c7656a1d7753ba02bafec78f7dd99e18b7f34af9a2ae0d5f5a00b330c853039769f2f6c193ede0d597e84c16694c30bf45ae542c744ee08705aa0f5c48d706862325d76408f20df39f41e7c4dbd37dfbf1862fb5fed95324d7cb4a068eea910788058bc79939fe8077713bc8a18546d1f7ea5883bed970a82cbc3cfa066b9fd9323d3e0192ca69393413812bb2ebe025ebb120d31984f9e9a7a26dbd8a09852cd62db324e6bb2c94d04a5e0b24174af7dfaecebff58801be80f4d1e5400a0414befb1392d59c80ec0437feeab156080c237257ef2b352706f1946dec6c1baa060a5007191b8029e1b03785b2208e86e454eff23748c326a82767de3e16bc05e80

  host_module_env->AppendFuncExport(
    "eth2_blockDataCopy",
    {{Type::I32, Type::I32, Type::I32}, {}},
    [&env](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      //printf("eth2_blockDataCopy\n");

      // eth2_blockDataCopy(outOffset, srcOffset, length) {
      //wabt::interp::Memory* mem = &env->memories_[0];
      wabt::interp::Memory* mem = env->GetMemory(0);

      //&(mem->data[a_offset])

      // void memorySet(size_t offset, uint8_t value) override { m_wasmMemory->data[offset] = static_cast<char>(value); }

      uint32_t out_offset = args[0].value.i32;
      uint32_t src_offset = args[1].value.i32;
      uint32_t copy_len = args[2].value.i32;

      //char myArray[] = {0x00, 0x01, 0x02};

      //unsigned char blockData[] = {0xf9, 0x02, 0x11, 0xa0, 0x42, 0xd5, 0xf0, 0x40, 0x1d, 0x27, 0xbd, 0xae, 0x05, 0x45, 0x2c, 0xe5, 0x2b, 0xce, 0xe4, 0x2a, 0x2c, 0x65, 0x9c, 0x11, 0x2d, 0x15, 0xb5, 0xf3, 0x0b, 0x12, 0x6b, 0x9e, 0xc2, 0x4c, 0x68, 0xc0, 0xa0, 0x45, 0x45, 0x4c, 0xcb, 0x94, 0xe6, 0x2e, 0xfd, 0xbc, 0xd0, 0x84, 0x23, 0xb7, 0xee, 0xf4, 0xca, 0xc4, 0x36, 0x14, 0x02, 0x50, 0x10, 0xc4, 0x31, 0x2a, 0x74, 0x5f, 0x67, 0x40, 0x15, 0x88, 0xa5, 0xa0, 0x2a, 0xfb, 0x19, 0x06, 0x9a, 0x24, 0x7c, 0x89, 0x76, 0x1e, 0x8e, 0xef, 0x70, 0x06, 0x49, 0xc9, 0x57, 0x43, 0xf1, 0x65, 0x1e, 0x83, 0x7c, 0x26, 0xaa, 0xb6, 0x88, 0x02, 0x28, 0xbe, 0xdb, 0xf3, 0xa0, 0x30, 0x5d, 0xd5, 0x53, 0x90, 0xd2, 0xba, 0xea, 0x91, 0x57, 0xb4, 0xb1, 0x84, 0x1b, 0xf1, 0x35, 0x6b, 0x11, 0x87, 0x31, 0x0b, 0xac, 0xc5, 0xeb, 0x84, 0x8c, 0x34, 0xce, 0x13, 0x91, 0x87, 0x5a, 0xa0, 0xb5, 0x3a, 0x79, 0xd6, 0x7e, 0xe7, 0x72, 0xcf, 0xdc, 0x8d, 0xf0, 0x34, 0x02, 0xe1, 0x40, 0x0e, 0x7f, 0xf4, 0xdf, 0x4e, 0x97, 0x42, 0x3a, 0x62, 0x96, 0xc2, 0xb7, 0x1a, 0x76, 0x49, 0xaf, 0x1c, 0xa0, 0xdf, 0xaf, 0xfe, 0xf7, 0x7c, 0x95, 0xe5, 0x19, 0xbd, 0x9f, 0x2e, 0xf4, 0x64, 0xc6, 0x95, 0x45, 0xb5, 0x08, 0x51, 0x06, 0xd7, 0x67, 0xed, 0x1d, 0xfc, 0xb2, 0xfe, 0xf1, 0x2d, 0x3f, 0x94, 0xf5, 0xa0, 0xa8, 0x98, 0xad, 0x01, 0xe2, 0x72, 0xc3, 0x50, 0xdd, 0x4a, 0x79, 0x54, 0xbd, 0x3e, 0xda, 0x10, 0x83, 0x7e, 0xe7, 0x9d, 0xe6, 0xd7, 0xa9, 0x92, 0x45, 0xa3, 0xb5, 0xd6, 0x38, 0xad, 0x51, 0xae, 0xa0, 0xd5, 0x47, 0x68, 0x78, 0xfd, 0x37, 0xd0, 0x82, 0xf4, 0x70, 0x84, 0x40, 0x45, 0x92, 0xdc, 0x5f, 0xe1, 0xf8, 0x89, 0x11, 0xe2, 0x57, 0x09, 0x86, 0x2f, 0x07, 0x87, 0x12, 0x80, 0x6f, 0x0c, 0x00, 0xa0, 0x34, 0x5a, 0xe4, 0x50, 0x38, 0x37, 0x68, 0x5c, 0x76, 0x56, 0xa1, 0xd7, 0x75, 0x3b, 0xa0, 0x2b, 0xaf, 0xec, 0x78, 0xf7, 0xdd, 0x99, 0xe1, 0x8b, 0x7f, 0x34, 0xaf, 0x9a, 0x2a, 0xe0, 0xd5, 0xf5, 0xa0, 0x0b, 0x33, 0x0c, 0x85, 0x30, 0x39, 0x76, 0x9f, 0x2f, 0x6c, 0x19, 0x3e, 0xde, 0x0d, 0x59, 0x7e, 0x84, 0xc1, 0x66, 0x94, 0xc3, 0x0b, 0xf4, 0x5a, 0xe5, 0x42, 0xc7, 0x44, 0xee, 0x08, 0x70, 0x5a, 0xa0, 0xf5, 0xc4, 0x8d, 0x70, 0x68, 0x62, 0x32, 0x5d, 0x76, 0x40, 0x8f, 0x20, 0xdf, 0x39, 0xf4, 0x1e, 0x7c, 0x4d, 0xbd, 0x37, 0xdf, 0xbf, 0x18, 0x62, 0xfb, 0x5f, 0xed, 0x95, 0x32, 0x4d, 0x7c, 0xb4, 0xa0, 0x68, 0xee, 0xa9, 0x10, 0x78, 0x80, 0x58, 0xbc, 0x79, 0x93, 0x9f, 0xe8, 0x07, 0x77, 0x13, 0xbc, 0x8a, 0x18, 0x54, 0x6d, 0x1f, 0x7e, 0xa5, 0x88, 0x3b, 0xed, 0x97, 0x0a, 0x82, 0xcb, 0xc3, 0xcf, 0xa0, 0x66, 0xb9, 0xfd, 0x93, 0x23, 0xd3, 0xe0, 0x19, 0x2c, 0xa6, 0x93, 0x93, 0x41, 0x38, 0x12, 0xbb, 0x2e, 0xbe, 0x02, 0x5e, 0xbb, 0x12, 0x0d, 0x31, 0x98, 0x4f, 0x9e, 0x9a, 0x7a, 0x26, 0xdb, 0xd8, 0xa0, 0x98, 0x52, 0xcd, 0x62, 0xdb, 0x32, 0x4e, 0x6b, 0xb2, 0xc9, 0x4d, 0x04, 0xa5, 0xe0, 0xb2, 0x41, 0x74, 0xaf, 0x7d, 0xfa, 0xec, 0xeb, 0xff, 0x58, 0x80, 0x1b, 0xe8, 0x0f, 0x4d, 0x1e, 0x54, 0x00, 0xa0, 0x41, 0x4b, 0xef, 0xb1, 0x39, 0x2d, 0x59, 0xc8, 0x0e, 0xc0, 0x43, 0x7f, 0xee, 0xab, 0x15, 0x60, 0x80, 0xc2, 0x37, 0x25, 0x7e, 0xf2, 0xb3, 0x52, 0x70, 0x6f, 0x19, 0x46, 0xde, 0xc6, 0xc1, 0xba, 0xa0, 0x60, 0xa5, 0x00, 0x71, 0x91, 0xb8, 0x02, 0x9e, 0x1b, 0x03, 0x78, 0x5b, 0x22, 0x08, 0xe8, 0x6e, 0x45, 0x4e, 0xff, 0x23, 0x74, 0x8c, 0x32, 0x6a, 0x82, 0x76, 0x7d, 0xe3, 0xe1, 0x6b, 0xc0, 0x5e, 0x80};

      unsigned char blockData[] = {0xf9, 0x02, 0x11, 0xa0, 0x42, 0xd5, 0xf0, 0x40, 0x1d, 0x27, 0xbd, 0xae, 0x05, 0x45, 0x2c, 0xe5, 0x2b, 0xce, 0xe4, 0x2a, 0x2c, 0x65, 0x9c, 0x11, 0x2d, 0x15, 0xb5, 0xf3, 0x0b, 0x12, 0x6b, 0x9e, 0xc2, 0x4c, 0x68, 0xc0, 0xa0, 0x45, 0x45, 0x4c, 0xcb, 0x94, 0xe6, 0x2e, 0xfd, 0xbc, 0xd0, 0x84, 0x23, 0xb7, 0xee, 0xf4, 0xca, 0xc4, 0x36, 0x14, 0x02, 0x50, 0x10, 0xc4, 0x31, 0x2a, 0x74, 0x5f, 0x67, 0x40, 0x15, 0x88, 0xa5, 0xa0, 0x2a, 0xfb, 0x19, 0x06, 0x9a, 0x24, 0x7c, 0x89, 0x76, 0x1e, 0x8e, 0xef, 0x70, 0x06, 0x49, 0xc9, 0x57, 0x43, 0xf1, 0x65, 0x1e, 0x83, 0x7c, 0x26, 0xaa, 0xb6, 0x88, 0x02, 0x28, 0xbe, 0xdb, 0xf3, 0xa0, 0x30, 0x5d, 0xd5, 0x53, 0x90, 0xd2, 0xba, 0xea, 0x91, 0x57, 0xb4, 0xb1, 0x84, 0x1b, 0xf1, 0x35, 0x6b, 0x11, 0x87, 0x31, 0x0b, 0xac, 0xc5, 0xeb, 0x84, 0x8c, 0x34, 0xce, 0x13, 0x91, 0x87, 0x5a, 0xa0, 0xb5, 0x3a, 0x79, 0xd6, 0x7e, 0xe7, 0x72, 0xcf, 0xdc, 0x8d, 0xf0, 0x34, 0x02, 0xe1, 0x40, 0x0e, 0x7f, 0xf4, 0xdf, 0x4e, 0x97, 0x42, 0x3a, 0x62, 0x96, 0xc2, 0xb7, 0x1a, 0x76, 0x49, 0xaf, 0x1c, 0xa0, 0xdf, 0xaf, 0xfe, 0xf7, 0x7c, 0x95, 0xe5, 0x19, 0xbd, 0x9f, 0x2e, 0xf4, 0x64, 0xc6, 0x95, 0x45, 0xb5, 0x08, 0x51, 0x06, 0xd7, 0x67, 0xed, 0x1d, 0xfc, 0xb2, 0xfe, 0xf1, 0x2d, 0x3f, 0x94, 0xf5, 0xa0, 0xa8, 0x98, 0xad, 0x01, 0xe2, 0x72, 0xc3, 0x50, 0xdd, 0x4a, 0x79, 0x54, 0xbd, 0x3e, 0xda, 0x10, 0x83, 0x7e, 0xe7, 0x9d, 0xe6, 0xd7, 0xa9, 0x92, 0x45, 0xa3, 0xb5, 0xd6, 0x38, 0xad, 0x51, 0xae, 0xa0, 0xd5, 0x47, 0x68, 0x78, 0xfd, 0x37, 0xd0, 0x82, 0xf4, 0x70, 0x84, 0x40, 0x45, 0x92, 0xdc, 0x5f, 0xe1, 0xf8, 0x89, 0x11, 0xe2, 0x57, 0x09, 0x86, 0x2f, 0x07, 0x87, 0x12, 0x80, 0x6f, 0x0c};

      //unsigned char blockData[] = {0xf9, 0x02, 0x11, 0xa0, 0x42, 0xd5, 0xf0, 0x40, 0x1d, 0x27, 0xbd, 0xae, 0x05, 0x45, 0x2c, 0xe5, 0x2b, 0xce, 0xe4, 0x2a, 0x2c, 0x65, 0x9c, 0x11, 0x2d, 0x15, 0xb5, 0xf3, 0x0b, 0x12, 0x6b, 0x9e, 0xc2, 0x4c, 0x68, 0xc0, 0xa0, 0x45, 0x45, 0x4c, 0xcb, 0x94, 0xe6, 0x2e, 0xfd, 0xbc, 0xd0, 0x84, 0x23, 0xb7, 0xee, 0xf4, 0xca, 0xc4, 0x36, 0x14, 0x02, 0x50, 0x10, 0xc4, 0x31, 0x2a, 0x74, 0x5f, 0x67, 0x40, 0x15, 0x88, 0xa5, 0xa0, 0x2a, 0xfb, 0x19, 0x06, 0x9a, 0x24, 0x7c, 0x89, 0x76, 0x1e, 0x8e, 0xef, 0x70, 0x06, 0x49, 0xc9, 0x57, 0x43, 0xf1, 0x65, 0x1e, 0x83, 0x7c, 0x26, 0xaa, 0xb6, 0x88, 0x02, 0x28, 0xbe, 0xdb, 0xf3, 0xa0, 0x30, 0x5d, 0xd5, 0x53, 0x90, 0xd2, 0xba, 0xea, 0x91, 0x57, 0xb4, 0xb1, 0x84, 0x1b, 0xf1, 0x35, 0x6b, 0x11, 0x87, 0x31, 0x0b, 0xac, 0xc5, 0xeb, 0x84, 0x8c, 0x34, 0xce, 0x13, 0x91};

      //&(mem->data[a_offset]);
      //mem->data[out_offset] = static_cast<char>(value);
      unsigned char dataToCopy[copy_len];
      // std::copy(array+1, array+4, b);
      std::copy(blockData+src_offset, blockData+copy_len, dataToCopy);

      //mem->data[out_offset] = static_cast<char>(*dataToCopy);
      mem->data[out_offset] = static_cast<unsigned char>(*dataToCopy);

      //env->SetBignumStack(args[0].value.i32);
      return interp::Result::Ok;
    }
  );

}



static wabt::Result ReadAndRunModule(const char* module_filename) {
	/*
  constexpr auto to_us = [](chrono_clock::duration d) {
		return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
	};
	*/

  wabt::Result result;
  Environment env;
  InitEnvironment(&env);

  Errors errors;
  DefinedModule* module = nullptr;

  const auto parseStartTime = chrono_clock::now();

  result = ReadModule(module_filename, &env, &errors, &module);
  FormatErrorsToFile(errors, Location::Type::Binary);

  const auto now = chrono_clock::now();
	const auto parseDuration = now - parseStartTime;
  const auto execStartTime = now;

  if (Succeeded(result)) {
    Executor executor(&env, s_trace_stream, s_thread_options);
    //RunAllExports(module, &executor, RunVerbosity::Verbose);

    TypedValues args;
    interp::Export* main_ = module->GetExport("main");
    ExecResult exec_result = executor.RunExport(main_, args);

    const auto execFinishTime = chrono_clock::now();
    const auto execDuration = execFinishTime - execStartTime;
    //std::cout << "parse time: " << to_us(parseDuration) << "us\n";
    //std::cout << "exec time: " << to_us(execDuration) << "us\n";
    
    /*
    ExecResult exec_result = executor.RunStartFunction(module);
    if (exec_result.result == interp::Result::Ok) {
      if (s_run_all_exports) {
        RunAllExports(module, &executor, RunVerbosity::Verbose);
        const auto execFinishTime = chrono_clock::now();
        const auto execDuration = execFinishTime - execStartTime;
        std::cout << "parse time: " << to_us(parseDuration) << "us\n";
        std::cout << "exec time: " << to_us(execDuration) << "us\n";
      }
    } else {
      WriteResult(s_stdout_stream.get(), "error running start function",
                  exec_result.result);
    }
    */
  }
  return result;
}

int ProgramMain(int argc, char** argv) {
  InitStdio();
  s_stdout_stream = FileStream::CreateStdout();

  ParseOptions(argc, argv);

  wabt::Result result = ReadAndRunModule(s_infile);
  return result != wabt::Result::Ok;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
