// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/shader_library_gles.h"

#include <sstream>

#include "impeller/base/config.h"
#include "impeller/base/validation.h"
#include "impeller/blobcat/blob_library.h"
#include "impeller/renderer/backend/gles/shader_function_gles.h"

namespace impeller {

static ShaderStage ToShaderStage(Blob::ShaderType type) {
  switch (type) {
    case Blob::ShaderType::kVertex:
      return ShaderStage::kVertex;
    case Blob::ShaderType::kFragment:
      return ShaderStage::kFragment;
  }
  FML_UNREACHABLE();
}

static std::string GLESShaderNameToShaderKeyName(const std::string& name,
                                                 ShaderStage stage) {
  std::stringstream stream;
  stream << name;
  switch (stage) {
    case ShaderStage::kUnknown:
      stream << "_unknown_";
      break;
    case ShaderStage::kVertex:
      stream << "_vertex_";
      break;
    case ShaderStage::kFragment:
      stream << "_fragment_";
      break;
    case ShaderStage::kTessellationControl:
      stream << "_tessellation_control_";
      break;
    case ShaderStage::kTessellationEvaluation:
      stream << "_tessellation_evaluation_";
      break;
    case ShaderStage::kCompute:
      stream << "_compute_";
      break;
  }
  stream << "main";
  return stream.str();
}

ShaderLibraryGLES::ShaderLibraryGLES(
    std::vector<std::shared_ptr<fml::Mapping>> shader_libraries) {
  ShaderFunctionMap functions;
  UniqueID library_id;
  auto iterator = [&functions, &library_id](auto type,           //
                                            const auto& name,    //
                                            const auto& mapping  //
                                            ) -> bool {
    const auto stage = ToShaderStage(type);
    const auto key_name = GLESShaderNameToShaderKeyName(name, stage);

    functions[ShaderKey{key_name, stage}] = std::shared_ptr<ShaderFunctionGLES>(
        new ShaderFunctionGLES(library_id,  //
                               stage,       //
                               key_name,    //
                               mapping      //
                               ));

    return true;
  };
  for (auto library : shader_libraries) {
    auto blob_library = BlobLibrary{std::move(library)};
    if (!blob_library.IsValid()) {
      VALIDATION_LOG << "Could not construct blob library for shaders.";
      return;
    }
    blob_library.IterateAllBlobs(iterator);
  }

  functions_ = functions;
  is_valid_ = true;
}

// |ShaderLibrary|
ShaderLibraryGLES::~ShaderLibraryGLES() = default;

// |ShaderLibrary|
bool ShaderLibraryGLES::IsValid() const {
  return is_valid_;
}

// |ShaderLibrary|
std::shared_ptr<const ShaderFunction> ShaderLibraryGLES::GetFunction(
    std::string_view name,
    ShaderStage stage) {
  const auto key = ShaderKey{name, stage};
  if (auto found = functions_.find(key); found != functions_.end()) {
    return found->second;
  }
  return nullptr;
}

}  // namespace impeller
