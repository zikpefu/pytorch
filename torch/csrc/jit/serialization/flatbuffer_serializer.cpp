#include <torch/csrc/jit/serialization/flatbuffer_serializer.h>

#include <ATen/ATen.h>
#include <c10/core/CPUAllocator.h>
#include <flatbuffers/flatbuffers.h>
#include <torch/csrc/jit/mobile/code.h>
#include <torch/csrc/jit/mobile/flatbuffer_loader.h>
#include <torch/csrc/jit/passes/inliner.h>
#include <torch/csrc/jit/runtime/instruction.h>
#include <torch/csrc/jit/serialization/export.h>
#include <torch/csrc/jit/serialization/export_bytecode.h>
#include <torch/csrc/jit/serialization/import.h>
#include <string>

namespace torch {
namespace jit {

using flatbuffers::FlatBufferBuilder;
using mobile::serialization::CreateArg;
using mobile::serialization::CreateDebugInfo;
using mobile::serialization::CreateDict;
using mobile::serialization::CreateFunctionDirect;
using mobile::serialization::CreateIValue;
using mobile::serialization::CreateList;
using mobile::serialization::CreateModule;
using mobile::serialization::CreateObject;
using mobile::serialization::CreateOperator;
using mobile::serialization::CreateTensorMetadataDirect;
using mobile::serialization::CreateTupleDirect;

namespace {

// We will store IValue NONE in index 0 in flatbuffer.
constexpr int kNoneIndex = 0;

static TypePtr realType(TypePtr type) {
  if (auto dyn = type->castRaw<c10::DynamicType>()) {
    return dyn->fallback();
  } else {
    return type;
  }
}

auto print_type(const c10::Type& t) -> c10::optional<std::string> {
  auto namedType = t.cast<c10::NamedType>();
  if (namedType && namedType->name()) {
    return namedType->name().value().qualifiedName();
  }
  if (auto dyn = t.castRaw<c10::DynamicType>()) {
    return dyn->fallback()->annotation_str();
  }
  return c10::nullopt;
}

class FlatbufferSerializer {
 public:
  FlatbufferSerializer() = default;

  flatbuffers::DetachedBuffer serializeModule(
      const mobile::Module& module,
      bool include_tensor_data_in_flatbuffer,
      const ExtraFilesMap& extra_files = ExtraFilesMap(),
      const ExtraFilesMap& jit_sources = ExtraFilesMap(),
      const std::vector<IValue>& jit_constants = {});

 private:
  template <typename It>
  std::vector<uint32_t> storeIValuesAndGetIndexes(
      flatbuffers::FlatBufferBuilder& fbb,
      It begin,
      It end) {
    std::vector<uint32_t> indexes;
    for (; begin != end; ++begin) {
      indexes.push_back(storeIValueAndGetIndex(fbb, *begin));
    }
    return indexes;
  }

  flatbuffers::Offset<mobile::serialization::Tuple> tupleToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      const IValue& tuple);

  flatbuffers::Offset<mobile::serialization::List> listToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      const IValue& list);

  flatbuffers::Offset<mobile::serialization::Dict> dictToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      const IValue& list);

  flatbuffers::Offset<mobile::serialization::Object> objectToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      const IValue& ivalue);

  flatbuffers::Offset<mobile::serialization::TensorMetadata> tensorToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      const IValue& ivalue);

  flatbuffers::Offset<mobile::serialization::Function> functionToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      const std::string& qn,
      const mobile::Function& func);

  flatbuffers::Offset<mobile::serialization::IValue> iValueToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      const IValue& ivalue);

  flatbuffers::Offset<jit::mobile::serialization::Schema> CreateFBSchema(
      flatbuffers::FlatBufferBuilder& fbb,
      const std::vector<Argument>& args,
      const std::vector<Argument>& returns,
      c10::TypePrinter type_printer);

  flatbuffers::Offset<mobile::serialization::ObjectType> classTypeToFB(
      flatbuffers::FlatBufferBuilder& fbb,
      ClassTypePtr class_ptr);

  uint32_t storeIValueAndGetIndex(
      flatbuffers::FlatBufferBuilder& fbb,
      const IValue& ivalue);
  uint32_t storeFunctionAndGetIndex(
      flatbuffers::FlatBufferBuilder& fbb,
      const std::string& qn,
      const mobile::Function& function);

  uint32_t storeClassTypeAndGetIndex(
      flatbuffers::FlatBufferBuilder& fbb,
      ClassTypePtr class_type);

  flatbuffers::Offset<flatbuffers::Vector<
      flatbuffers::Offset<mobile::serialization::ExtraFile>>>
  storeExtraFilesAndGetOffset(
      FlatBufferBuilder& fbb,
      const ExtraFilesMap& extra_files);

  uint32_t insertIValue(
      flatbuffers::Offset<mobile::serialization::IValue> ivalue) {
    uint32_t size = ivalue_offsets_.size();
    ivalue_offsets_.push_back(ivalue);
    return size;
  }

  std::vector<at::Tensor> tensor_data_;

  std::unordered_map<const void*, uint32_t> memoized_storage_map_;

  std::vector<flatbuffers::Offset<mobile::serialization::IValue>>
      ivalue_offsets_;
  std::vector<flatbuffers::Offset<mobile::serialization::ObjectType>>
      obj_types_offset_;

  // qualified name to serialized class, type or function
  std::unordered_map<std::string, uint32_t> qn_to_serialized_values_;

  // cache of some ivalues
  struct IValueHash {
    size_t operator()(const IValue& val) const {
      return IValue::hash(val);
    }
  };

  std::unordered_map<IValue, uint32_t, IValueHash> cached_ivalues_;

  const mobile::CompilationUnit* mcu_ = nullptr;
};

flatbuffers::Offset<jit::mobile::serialization::Schema> FlatbufferSerializer::
    CreateFBSchema(
        flatbuffers::FlatBufferBuilder& fbb,
        const std::vector<Argument>& args,
        const std::vector<Argument>& returns,
        c10::TypePrinter type_printer) {
  std::vector<flatbuffers::Offset<jit::mobile::serialization::Arg>> arg_vec;
  arg_vec.reserve(args.size());
  std::vector<flatbuffers::Offset<jit::mobile::serialization::Arg>> return_vec;
  return_vec.reserve(returns.size());
  for (const auto& arg : args) {
    int index = storeIValueAndGetIndex(fbb, arg.default_value());
    arg_vec.emplace_back(CreateArg(
        fbb,
        fbb.CreateSharedString(arg.name()),
        fbb.CreateSharedString(
            realType(arg.type())->annotation_str(type_printer)),
        index));
  }

  for (const auto& ret : returns) {
    int index = storeIValueAndGetIndex(fbb, ret.default_value());
    return_vec.emplace_back(CreateArg(
        fbb,
        fbb.CreateSharedString(ret.name()),
        fbb.CreateSharedString(
            realType(ret.type())->annotation_str(type_printer)),
        index));
  }
  return CreateSchema(
      fbb, fbb.CreateVector(arg_vec), fbb.CreateVector(return_vec));
}

flatbuffers::Offset<mobile::serialization::Function> FlatbufferSerializer::
    functionToFB(
        FlatBufferBuilder& fbb,
        const std::string& qn,
        const mobile::Function& func) {
  const auto& code = func.get_code();

  // instructions
  std::vector<mobile::serialization::Instruction> instruction_vector;
  for (const auto& inst : code.instructions_) {
    instruction_vector.emplace_back(inst.op, inst.N, inst.X);
  }

  // operators
  std::vector<flatbuffers::Offset<mobile::serialization::Operator>>
      operator_vector;
  operator_vector.reserve(code.op_names_.size());
  for (int i = 0; i < code.op_names_.size(); ++i) {
    const auto& opname = code.op_names_[i];
    const int op_size = code.operator_input_sizes_[i];
    operator_vector.push_back(CreateOperator(
        fbb,
        fbb.CreateSharedString(opname.name),
        fbb.CreateSharedString(opname.overload_name),
        op_size));
  }

  const auto& constants = code.constants_;

  std::vector<uint32_t> constant_indexes;
  constant_indexes.reserve(constants.size());
  for (const auto& constant : constants) {
    constant_indexes.push_back(storeIValueAndGetIndex(fbb, constant));
  }

  // types
  static const std::string torch_prefix("__torch__");
  static const std::string class_prefix("__torch__.torch.classes");
  std::vector<flatbuffers::Offset<flatbuffers::String>> type_offsets;

  for (const TypePtr& t : code.types_) {
    auto type_str = realType(t)->annotation_str();
    if (type_str.find(torch_prefix) == 0) {
      TORCH_CHECK(
          type_str.find(class_prefix) == 0,
          "__torch__ types other than custom c++ classes (__torch__.torch.classes)"
          "are not supported in lite interpreter. ",
          "Workaround: instead of using arbitrary class type (class Foo()), ",
          "define a pytorch class (class Foo(torch.nn.Module)).");
    }

    type_offsets.push_back(fbb.CreateSharedString(type_str));
  }

  // since the register location is embedded into the bytecode, pass the
  // register size
  auto register_size = static_cast<int>(code.register_size_);

  // schema
  auto type_printer = [&](const c10::Type& t) -> c10::optional<std::string> {
    auto namedType = t.cast<c10::NamedType>();
    if (namedType && namedType->name()) {
      return namedType->name().value().qualifiedName();
    }
    if (auto dyn = t.castRaw<c10::DynamicType>()) {
      return dyn->fallback()->annotation_str();
    }
    return c10::nullopt;
  };

  flatbuffers::Offset<mobile::serialization::Schema> schema_offset = 0;
  uint32_t class_index = 0;
  if (func.hasSchema()) {
    const auto& schema = func.getSchema();
    TORCH_CHECK(
        schema.overload_name().empty(), // @TODO: is this check correct?
        "Overloads are not supported in mobile modules.");
    TORCH_CHECK(
        !schema.is_vararg(),
        "Python *args are not supported in mobile modules.");
    TORCH_CHECK(
        !schema.is_varret(),
        "A variable number of return values is not supported in mobile modules.");
    schema_offset =
        CreateFBSchema(fbb, schema.arguments(), schema.returns(), type_printer);
    auto classtype = schema.arguments()[0].type()->cast<ClassType>();
    class_index = storeClassTypeAndGetIndex(fbb, classtype);
  }

  auto debug_info_offset =
      CreateDebugInfo(fbb, fbb.CreateVector(code.debug_handles_));

  auto function_offset = CreateFunctionDirect(
      fbb,
      qn.c_str(),
      &instruction_vector,
      &operator_vector,
      &constant_indexes,
      &type_offsets,
      register_size,
      schema_offset,
      debug_info_offset,
      class_index);
  return function_offset;
}

flatbuffers::Offset<
    flatbuffers::Vector<flatbuffers::Offset<mobile::serialization::ExtraFile>>>
FlatbufferSerializer::storeExtraFilesAndGetOffset(
    FlatBufferBuilder& fbb,
    const ExtraFilesMap& extra_files) {
  std::vector<flatbuffers::Offset<mobile::serialization::ExtraFile>>
      extra_file_offsets;

  for (const auto& extra_file : extra_files) {
    flatbuffers::Offset<mobile::serialization::ExtraFile> extra_file_offset =
        mobile::serialization::CreateExtraFile(
            fbb,
            fbb.CreateSharedString(extra_file.first),
            fbb.CreateString(extra_file.second));
    extra_file_offsets.emplace_back(extra_file_offset);
  }
  return fbb.CreateVector(extra_file_offsets);
}

flatbuffers::DetachedBuffer FlatbufferSerializer::serializeModule(
    const mobile::Module& module,
    bool include_tensor_data_in_flatbuffer,
    const ExtraFilesMap& extra_files,
    const ExtraFilesMap& jit_sources,
    const std::vector<IValue>& jit_constants) {
  FlatBufferBuilder fbb;

  mcu_ = &module.compilation_unit();

  // first element is None.
  insertIValue(CreateIValue(fbb, mobile::serialization::IValueUnion::NONE, 0));

  auto methods = module.get_methods();
  std::vector<uint32_t> functions_index;
  functions_index.reserve(methods.size());
  for (const auto& method : methods) {
    auto func_offset = storeFunctionAndGetIndex(
        fbb, method.function().qualname().qualifiedName(), method.function());
    functions_index.push_back(func_offset);
  }

  auto functions_offset = fbb.CreateVector(functions_index);
  uint32_t ivalue_index = storeIValueAndGetIndex(fbb, module._ivalue());

  flatbuffers::Offset<flatbuffers::Vector<
      flatbuffers::Offset<mobile::serialization::StorageData>>>
      storage_data_offset = 0;
  if (include_tensor_data_in_flatbuffer) {
    std::vector<flatbuffers::Offset<mobile::serialization::StorageData>>
        storage_data;
    for (auto td : tensor_data_) {
      if (td.storage().device_type() != DeviceType::CPU) {
        td = at::empty({0}, td.options())
                 .set_(
                     td.storage(),
                     /* storage_offset = */ 0,
                     /* size = */
                     {static_cast<int64_t>(
                         td.storage().nbytes() / td.element_size())},
                     /* stride = */ {1})
                 .cpu();
      }
      fbb.ForceVectorAlignment(
          td.storage().nbytes(), sizeof(uint8_t), FLATBUFFERS_MAX_ALIGNMENT);
      auto storage_offset = mobile::serialization::CreateStorageData(
          fbb,
          fbb.CreateVector(
              reinterpret_cast<const uint8_t*>(td.storage().data()),
              td.storage().nbytes()));
      storage_data.push_back(storage_offset);
    }
    storage_data_offset = fbb.CreateVector(storage_data);
  }

  auto extra_files_offset = storeExtraFilesAndGetOffset(fbb, extra_files);

  auto jit_source_offset = storeExtraFilesAndGetOffset(fbb, jit_sources);
  std::vector<uint32_t> jit_constants_indexes;
  jit_constants_indexes.reserve(jit_constants.size());
  for (const auto& ival : jit_constants) {
    jit_constants_indexes.emplace_back(storeIValueAndGetIndex(fbb, ival));
  }

  const uint32_t bytecode_version =
      static_cast<uint32_t>(module.bytecode_version());

  auto mod = CreateModule(
      fbb,
      /*bytecode_version=*/bytecode_version,
      extra_files_offset, /* extra_files */
      functions_offset,
      ivalue_index,
      fbb.CreateVector(ivalue_offsets_),
      tensor_data_.size(),
      storage_data_offset,
      fbb.CreateVector(obj_types_offset_),
      jit_source_offset,
      fbb.CreateVector(jit_constants_indexes));
  FinishModuleBuffer(fbb, mod);
  return fbb.Release();
}

flatbuffers::Offset<mobile::serialization::Tuple> FlatbufferSerializer::
    tupleToFB(flatbuffers::FlatBufferBuilder& fbb, const IValue& tuple) {
  const auto& elements = tuple.toTuple()->elements();
  std::vector<uint32_t> items =
      storeIValuesAndGetIndexes(fbb, elements.begin(), elements.end());
  return CreateTupleDirect(fbb, &items);
}

flatbuffers::Offset<mobile::serialization::List> FlatbufferSerializer::listToFB(
    flatbuffers::FlatBufferBuilder& fbb,
    const IValue& list) {
  const auto& elements = list.toList();
  std::vector<uint32_t> items =
      storeIValuesAndGetIndexes(fbb, elements.begin(), elements.end());
  return CreateList(
      fbb,
      fbb.CreateVector(items),
      fbb.CreateSharedString(
          realType(list.type<c10::Type>())->annotation_str(print_type)));
}

flatbuffers::Offset<mobile::serialization::Dict> FlatbufferSerializer::dictToFB(
    flatbuffers::FlatBufferBuilder& fbb,
    const IValue& ivalue) {
  const auto& dict = ivalue.toGenericDict();
  std::vector<uint32_t> keys;
  std::vector<uint32_t> values;
  keys.reserve(dict.size());
  values.reserve(dict.size());
  for (const auto& entry : dict) {
    int key_index = storeIValueAndGetIndex(fbb, entry.key());
    keys.push_back(key_index);
    int value_index = storeIValueAndGetIndex(fbb, entry.value());
    values.push_back(value_index);
  }

  return CreateDict(
      fbb,
      fbb.CreateVector(keys),
      fbb.CreateVector(values),
      fbb.CreateSharedString(
          realType(ivalue.type<c10::Type>())->annotation_str(print_type)));
}

flatbuffers::Offset<mobile::serialization::ObjectType> FlatbufferSerializer::
    classTypeToFB(FlatBufferBuilder& fbb, ClassTypePtr class_ptr) {
  mobile::serialization::TypeType typetype =
      mobile::serialization::TypeType::UNSET;

  flatbuffers::Offset<
      flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>>
      names_offset = 0;
  c10::QualifiedName setstate_name(*class_ptr->name(), "__setstate__");
  const mobile::Function* setstate = mcu_->find_function(setstate_name);
  if (setstate != nullptr) {
    typetype = mobile::serialization::TypeType::CLASS_WITH_SETSTATE;
  } else if (class_ptr->findMethod("__setstate__")) {
    typetype = mobile::serialization::TypeType::CUSTOM_CLASS;
  } else {
    size_t num_attr = class_ptr->numAttributes();
    std::vector<flatbuffers::Offset<flatbuffers::String>> names;
    std::vector<uint32_t> type_index;
    for (size_t i = 0; i < num_attr; ++i) {
      names.push_back(fbb.CreateSharedString(class_ptr->getAttributeName(i)));
    }
    names_offset = fbb.CreateVector(names);
    typetype = mobile::serialization::TypeType::CLASS_WITH_FIELD;
  }

  auto name_offset = fbb.CreateString(class_ptr->name()->qualifiedName());
  return CreateObjectType(fbb, name_offset, typetype, names_offset);
}

uint32_t FlatbufferSerializer::storeFunctionAndGetIndex(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::string& qn,
    const mobile::Function& function) {
  auto iter = qn_to_serialized_values_.find(qn);
  if (iter != qn_to_serialized_values_.end()) {
    return iter->second;
  }

  auto offset = CreateIValue(
      fbb,
      mobile::serialization::IValueUnion::Function,
      functionToFB(fbb, qn, function).Union());

  uint32_t index = insertIValue(offset);
  qn_to_serialized_values_[qn] = index;
  return index;
}

uint32_t FlatbufferSerializer::storeClassTypeAndGetIndex(
    FlatBufferBuilder& fbb,
    ClassTypePtr class_ptr) {
  const auto& type_str = class_ptr->name()->qualifiedName();
  auto iter = qn_to_serialized_values_.find(type_str);
  if (iter != qn_to_serialized_values_.end()) {
    return iter->second;
  }

  auto offset = classTypeToFB(fbb, class_ptr);
  uint32_t res = obj_types_offset_.size();
  obj_types_offset_.push_back(offset);
  qn_to_serialized_values_[type_str] = res;
  return res;
}

flatbuffers::Offset<mobile::serialization::Object> FlatbufferSerializer::
    objectToFB(flatbuffers::FlatBufferBuilder& fbb, const IValue& ivalue) {
  auto obj = ivalue.toObject();
  auto type = obj->type();
  // rename type?
  // check getstate

  // save state as ivalue
  flatbuffers::Offset<flatbuffers::Vector<uint32_t>> attrs = 0;
  uint32_t state_index = 0;
  uint32_t setstate_func_index = 0;
  const auto qn = type->name()->qualifiedName() + ".__setstate__";
  auto getstate = type->findMethod("__getstate__");
  auto setstate = type->findMethod("__setstate__");
  if (getstate && setstate) {
    auto state = (*getstate)({obj});
    state_index = storeIValueAndGetIndex(fbb, state);
    auto func_index = qn_to_serialized_values_.find(qn);
    if (func_index != qn_to_serialized_values_.end()) {
      setstate_func_index = func_index->second;
    }
  } else {
    size_t num_attr = type->numAttributes();
    std::vector<uint32_t> tuple_index;
    for (size_t i = 0; i < num_attr; ++i) {
      tuple_index.push_back(storeIValueAndGetIndex(fbb, obj->getSlot(i)));
    }
    attrs = fbb.CreateVector(tuple_index);
  }

  uint32_t type_index = storeClassTypeAndGetIndex(fbb, type);
  return CreateObject(fbb, type_index, state_index, attrs, setstate_func_index);
}

flatbuffers::Offset<mobile::serialization::TensorMetadata> FlatbufferSerializer::
    FlatbufferSerializer::tensorToFB(
        flatbuffers::FlatBufferBuilder& fbb,
        const IValue& ivalue) {
  auto& tensor = ivalue.toTensor();
  bool quantized = tensor.is_quantized();
  const at::Storage& storage = tensor.storage();

  flatbuffers::Offset<mobile::serialization::QuantizedSchema> qschema_offset =
      0;
  if (quantized) {
    double scale = 0;
    int32_t zero_point = 0;
    flatbuffers::Offset<mobile::serialization::TensorMetadata> scales = 0;
    flatbuffers::Offset<mobile::serialization::TensorMetadata> zero_points = 0;
    int32_t axis = 0;

    switch (tensor.qscheme()) {
      case at::kPerTensorAffine:
        scale = tensor.q_scale();
        zero_point = tensor.q_zero_point();
        break;
      case at::kPerChannelAffineFloatQParams:
      case at::kPerChannelAffine: {
        scales = tensorToFB(fbb, tensor.q_per_channel_scales());
        zero_points = tensorToFB(fbb, tensor.q_per_channel_zero_points());
        axis = tensor.q_per_channel_axis();
      } break;
      default:
        TORCH_CHECK(
            false,
            "Unsupported tensor quantization type in serialization ",
            toString(tensor.qscheme()));
        break;
    }

    qschema_offset = mobile::serialization::CreateQuantizedSchema(
        fbb,
        static_cast<int8_t>(tensor.qscheme()),
        scale,
        zero_point,
        scales,
        zero_points,
        axis);
  }

  void* addr = storage.unsafeGetStorageImpl();
  uint32_t storage_index = 0;
  auto it = memoized_storage_map_.find(addr);
  if (it != memoized_storage_map_.end()) {
    storage_index = it->second;
  } else {
    storage_index = tensor_data_.size();
    memoized_storage_map_[addr] = storage_index;
    tensor_data_.push_back(tensor);
  }

  std::vector<int> sizes{tensor.sizes().begin(), tensor.sizes().end()};
  std::vector<int> strides{tensor.strides().begin(), tensor.strides().end()};

  return CreateTensorMetadataDirect(
      fbb,
      /* storage_location_index */ storage_index,
      /* scalar_type */ static_cast<int8_t>(tensor.scalar_type()),
      /* int32_t storage_offset */ tensor.storage_offset(),
      /* sizes */ &sizes,
      /* strides */ &strides,
      /* bool requires_grad */ tensor.requires_grad(),
      /* qschema */ qschema_offset);
}

uint32_t FlatbufferSerializer::storeIValueAndGetIndex(
    flatbuffers::FlatBufferBuilder& fbb,
    const IValue& ivalue) {
  if (ivalue.isNone()) {
    return kNoneIndex;
  }

  try {
    auto iter = cached_ivalues_.find(ivalue);
    if (iter != cached_ivalues_.end()) {
      return iter->second;
    }
  } catch (const std::runtime_error&) {
    // Threw if ivalue is not hashable
  } catch (const c10::Error&) {
    // Threw if ivalue is don't have proper operator==
  }

  auto offset = iValueToFB(fbb, ivalue);
  uint32_t index = insertIValue(offset);
  try {
    cached_ivalues_[ivalue] = index;
  } catch (const std::runtime_error&) {
  } catch (const c10::Error&) {
  }

  return index;
}

flatbuffers::Offset<mobile::serialization::IValue> FlatbufferSerializer::
    iValueToFB(flatbuffers::FlatBufferBuilder& fbb, const IValue& ivalue) {
  using mobile::serialization::IValueUnion;

  IValueUnion ivalue_type = IValueUnion::NONE;
  flatbuffers::Offset<void> offset = 0;

  if (ivalue.isTensor()) {
    ivalue_type = IValueUnion::TensorMetadata;
    offset = tensorToFB(fbb, ivalue).Union();
  } else if (ivalue.isTuple()) {
    ivalue_type = IValueUnion::Tuple;
    offset = tupleToFB(fbb, ivalue).Union();
  } else if (ivalue.isDouble()) {
    ivalue_type = IValueUnion::Double;
    offset = fbb.CreateStruct(mobile::serialization::Double(ivalue.toDouble()))
                 .Union();
  } else if (ivalue.isComplexDouble()) {
    auto comp = ivalue.toComplexDouble();
    ivalue_type = IValueUnion::ComplexDouble;
    offset = fbb.CreateStruct(mobile::serialization::ComplexDouble(
                                  comp.real(), comp.imag()))
                 .Union();
  } else if (ivalue.isInt()) {
    ivalue_type = IValueUnion::Int;
    offset =
        fbb.CreateStruct(mobile::serialization::Int(ivalue.toInt())).Union();
  } else if (ivalue.isBool()) {
    ivalue_type = IValueUnion::Bool;
    offset =
        fbb.CreateStruct(mobile::serialization::Bool(ivalue.toBool())).Union();
  } else if (ivalue.isString()) {
    ivalue_type = IValueUnion::String;
    offset = mobile::serialization::CreateString(
                 fbb, fbb.CreateSharedString(ivalue.toString()->string()))
                 .Union();
  } else if (ivalue.isGenericDict()) {
    ivalue_type = IValueUnion::Dict;
    offset = dictToFB(fbb, ivalue).Union();
  } else if (ivalue.isNone()) {
    ivalue_type = IValueUnion::NONE;
    offset = 0;
  } else if (ivalue.isIntList()) {
    ivalue_type = IValueUnion::IntList;
    offset = mobile::serialization::CreateIntList(
                 fbb, fbb.CreateVector(ivalue.toIntVector()))
                 .Union();
  } else if (ivalue.isDoubleList()) {
    ivalue_type = IValueUnion::DoubleList;
    offset = mobile::serialization::CreateDoubleList(
                 fbb, fbb.CreateVector(ivalue.toDoubleVector()))
                 .Union();
  } else if (ivalue.isBoolList()) {
    ivalue_type = IValueUnion::BoolList;
    auto boollist = ivalue.toBoolList();
    std::vector<uint8_t> bool_vec(boollist.begin(), boollist.end());
    offset =
        mobile::serialization::CreateBoolListDirect(fbb, &bool_vec).Union();
  } else if (ivalue.isList()) {
    ivalue_type = IValueUnion::List;
    offset = listToFB(fbb, ivalue).Union();
  } else if (ivalue.isObject()) {
    ivalue_type = IValueUnion::Object;
    offset = objectToFB(fbb, ivalue).Union();
  } else if (ivalue.isDevice()) {
    ivalue_type = IValueUnion::Device;
    offset = mobile::serialization::CreateDevice(
                 fbb, fbb.CreateSharedString(ivalue.toDevice().str()))
                 .Union();
  } else if (ivalue.isEnum()) {
    const auto& enum_holder = ivalue.toEnumHolder();
    const auto& qualified_class_name =
        enum_holder->type()->qualifiedClassName();
    uint32_t ival_pos = storeIValueAndGetIndex(fbb, enum_holder->value());
    ivalue_type = IValueUnion::EnumValue;
    offset = mobile::serialization::CreateEnumValue(
                 fbb,
                 fbb.CreateSharedString(qualified_class_name.qualifiedName()),
                 ival_pos)
                 .Union();
  } else {
    AT_ERROR("Invalid IValue type for serialization: ", ivalue.tagKind());
  }
  return CreateIValue(fbb, ivalue_type, offset);
}

} // namespace

void save_mobile_module(
    const mobile::Module& module,
    const std::string& filename,
    const ExtraFilesMap& extra_files,
    const ExtraFilesMap& jit_sources,
    const std::vector<IValue>& jit_constants) {
  auto buffer = save_mobile_module_to_bytes(
      module, extra_files, jit_sources, jit_constants);
  std::fstream ofile(filename, std::ios::binary | std::ios::out);
  ofile.write(reinterpret_cast<char*>(buffer.data()), buffer.size());
  ofile.close();
}

flatbuffers::DetachedBuffer save_mobile_module_to_bytes(
    const mobile::Module& module,
    const ExtraFilesMap& extra_files,
    const ExtraFilesMap& jit_sources,
    const std::vector<IValue>& jit_constants) {
  FlatbufferSerializer fb_serializer;
  return fb_serializer.serializeModule(
      module,
      /*include_tensor_data_in_flatbuffer*/ true,
      extra_files,
      jit_sources,
      jit_constants);
}

Module parse_and_initialize_jit_module(
    std::shared_ptr<char> data,
    size_t size,
    c10::optional<at::Device> device,
    ExtraFilesMap& extra_files) {
  auto* flatbuffer_module = mobile::serialization::GetMutableModule(data.get());
  FlatbufferLoader loader;
  mobile::Module mobilem = loader.parseModule(flatbuffer_module);
  parseExtraFiles(flatbuffer_module, extra_files);
  ExtraFilesMap files;
  std::vector<IValue> constants;
  loader.extractJitSourceAndConstants(&files, &constants);
  Module m = jitModuleFromSourceAndConstants(
      mobilem._ivalue(),
      files,
      constants,
      flatbuffer_module->bytecode_version());
  m.set_delete_memory(data);
  return m;
}

Module load_jit_module_from_file(
    const std::string& filename,
    ExtraFilesMap& extra_files,
    c10::optional<at::Device> device) {
  auto data = get_file_content(filename.c_str());
  return parse_and_initialize_jit_module(
      std::move(std::get<0>(data)), std::get<1>(data), device, extra_files);
}

Module load_jit_module_from_stream(
    std::istream& in,
    ExtraFilesMap& extra_files,
    c10::optional<at::Device> device) {
  auto data = get_stream_content(in);
  return parse_and_initialize_jit_module(
      std::move(std::get<0>(data)), std::get<1>(data), device, extra_files);
}

void save_jit_module(
    const Module& module,
    const std::string& filename,
    const ExtraFilesMap& extra_files) {
  auto buffer = save_jit_module_to_bytes(module, extra_files);
  std::fstream ofile(filename, std::ios::binary | std::ios::out);
  ofile.write(reinterpret_cast<char*>(buffer.data()), buffer.size()); // NOLINT
  ofile.close();
}

flatbuffers::DetachedBuffer save_jit_module_to_bytes(
    const Module& module,
    const ExtraFilesMap& extra_files) {
  ExtraFilesMap jitfiles;
  std::vector<IValue> constants;
  jitModuleToPythonCodeAndConstants(module, &jitfiles, &constants);
  CompilationOptions options;
  mobile::Module mobilem = jitModuleToMobile(module, options);
  return save_mobile_module_to_bytes(mobilem, extra_files, jitfiles, constants);
}

} // namespace jit
} // namespace torch
