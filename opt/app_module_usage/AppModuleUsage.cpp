/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AppModuleUsage.h"

#include <algorithm>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <sstream>
#include <string>

#include "ConfigFiles.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "ReflectionAnalysis.h"
#include "Show.h"
#include "Walkers.h"

namespace {
constexpr const char* APP_MODULE_USAGE_OUTPUT_FILENAME =
    "redex-app-module-usage.csv";
constexpr const char* APP_MODULE_COUNT_OUTPUT_FILENAME =
    "redex-app-module-count.csv";
constexpr const char* USES_AM_ANNO_VIOLATIONS_FILENAME =
    "redex-app-module-annotation-violations.csv";
// @UsesAppModule DexType descriptor
// returns potential type for an AbstractObject
boost::optional<DexType*> type_used(const reflection::AbstractObject& o) {
  DexClass* clazz = nullptr;
  if (o.dex_type) {
    clazz = type_class(o.dex_type);
  }
  switch (o.obj_kind) {
  case reflection::OBJECT:
    TRACE(APP_MOD_USE, 8,
          "Reflection with result kind of OBJECT found as type ");
    if (o.dex_type) {
      TRACE(APP_MOD_USE, 8, "%s\n", SHOW(o.dex_type));
      return o.dex_type;
    } else {
      TRACE(APP_MOD_USE, 8, "undetermined\n");
    }
    break;
  case reflection::INT:
    [[fallthrough]];
  case reflection::STRING:
    break;
  case reflection::CLASS:
    TRACE(APP_MOD_USE, 8,
          "Reflection with result kind of CLASS found as class ");
    if (o.dex_type) {
      TRACE(APP_MOD_USE, 8, "%s\n", SHOW(o.dex_type));
      return o.dex_type;
    } else {
      TRACE(APP_MOD_USE, 8, "undetermined\n");
    }
    break;
  case reflection::FIELD:
    TRACE(APP_MOD_USE,
          8,
          "Reflection with result kind of FIELD (%s) from class ",
          o.dex_string->c_str());
    if (clazz && !clazz->is_external() && o.dex_string) {
      auto field = clazz->find_field_from_simple_deobfuscated_name(
          o.dex_string->c_str());
      if (field) {
        TRACE(APP_MOD_USE, 8, "%s\n", field->get_type()->c_str());
        return field->get_type();
      } else {
        TRACE(APP_MOD_USE, 8, "undetermined; could not find field\n");
      }
    } else {
      TRACE(APP_MOD_USE,
            8,
            "undetermined; source class could not be created or is external\n");
    }
    break;
  case reflection::METHOD:
    TRACE(APP_MOD_USE,
          8,
          "Reflection with result kind of METHOD (%s) from class ",
          o.dex_string->c_str());
    if (clazz && !clazz->is_external() && o.dex_string) {
      auto reflective_method = clazz->find_method_from_simple_deobfuscated_name(
          o.dex_string->c_str());
      if (reflective_method && reflective_method->get_class()) {
        TRACE(APP_MOD_USE, 8, "%s\n", reflective_method->get_class()->c_str());
        return reflective_method->get_class();
      } else {
        TRACE(APP_MOD_USE, 8, "undetermined; could not find method\n");
      }
    } else {
      TRACE(APP_MOD_USE,
            8,
            "undetermined; source class could not be created or is external\n");
    }
    break;
  }
  return boost::none;
}
} // namespace

void AppModuleUsagePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  const auto& full_scope = build_class_scope(stores);

  reflection::MetadataCache refl_metadata_cache;
  for (unsigned int idx = 0; idx < stores.size(); idx++) {
    Scope scope = build_class_scope(stores.at(idx).get_dexen());
    walk::parallel::classes(scope, [&](DexClass* cls) {
      m_type_store_map.emplace(cls->get_type(), idx);
    });
  }
  walk::parallel::methods(full_scope, [&](DexMethod* method) {
    m_stores_method_uses_map.emplace(method,
                                     std::unordered_set<unsigned int>{});
    m_stores_method_uses_reflectively_map.emplace(
        method, std::unordered_set<unsigned int>{});
  });

  analyze_direct_app_module_usage(full_scope);
  TRACE(APP_MOD_USE, 4, "*** Direct analysis done\n");
  analyze_reflective_app_module_usage(full_scope);
  TRACE(APP_MOD_USE, 4, "*** Reflective analysis done\n");
  generate_report(stores, conf, mgr);
  TRACE(APP_MOD_USE, 4, "*** Report done\n");

  if (m_output_entrypoints_to_modules) {
    TRACE(APP_MOD_USE, 4, "*** Outputting module use at %s\n",
          APP_MODULE_USAGE_OUTPUT_FILENAME);
    output_usages(stores, conf);
  }
  if (m_output_module_use_count) {
    TRACE(APP_MOD_USE, 4, "*** Outputting module use count at %s\n",
          APP_MODULE_COUNT_OUTPUT_FILENAME);
    output_use_count(stores, conf);
  }

  unsigned int num_methods_access_app_module = 0;
  for (const auto& pair : m_stores_method_uses_map) {
    auto reflective_references =
        m_stores_method_uses_reflectively_map.at(pair.first);
    if (!pair.second.empty() || !reflective_references.empty()) {
      num_methods_access_app_module++;
    }
  }
  mgr.set_metric("num_methods_access_app_module",
                 num_methods_access_app_module);
}

void AppModuleUsagePass::analyze_direct_app_module_usage(const Scope& scope) {
  unsigned int root_store = 0;
  walk::parallel::opcodes(scope, [&](DexMethod* method, IRInstruction* insn) {
    std::unordered_set<DexType*> types_referenced;
    auto method_class = method->get_class();
    always_assert_log(m_type_store_map.count(method_class) > 0,
                      "%s is missing from m_type_store_map",
                      SHOW(method_class));
    const auto method_store = m_type_store_map.at(method_class);
    if (insn->has_method()) {
      types_referenced.emplace(insn->get_method()->get_class());
    }
    if (insn->has_field()) {
      types_referenced.emplace(insn->get_field()->get_class());
    }
    if (insn->has_type()) {
      types_referenced.emplace(insn->get_type());
    }
    for (DexType* type : types_referenced) {
      if (m_type_store_map.count(type) > 0) {
        const auto store = m_type_store_map.at(type);
        if (store != root_store && store != method_store) {
          // App module reference!
          // add the store for the referenced type to the map
          m_stores_method_uses_map.update(
              method,
              [store](DexMethod* /* method */,
                      std::unordered_set<unsigned int>& stores_used,
                      bool /* exists */) { stores_used.emplace(store); });
          m_stores_use_count.update(store,
                                    [](const unsigned int& /*store*/,
                                       AppModuleUsage::UseCount& count,
                                       bool /* exists */) {
                                      count.direct_count =
                                          count.direct_count + 1;
                                    });
        }
      }
    }
  });
}

void AppModuleUsagePass::analyze_reflective_app_module_usage(
    const Scope& scope) {
  unsigned int root_store = 0;
  // Reflective Reference
  reflection::MetadataCache refl_metadata_cache;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    const auto method_store = m_type_store_map.at(method->get_class());
    std::unique_ptr<reflection::ReflectionAnalysis> analysis =
        std::make_unique<reflection::ReflectionAnalysis>(
            /* dex_method */ method,
            /* context (interprocedural only) */ nullptr,
            /* summary_query_fn (interprocedural only) */ nullptr,
            /* metadata_cache */ &refl_metadata_cache);
    for (auto& mie : InstructionIterable(code)) {
      IRInstruction* insn = mie.insn;
      boost::optional<DexType*> type = boost::none;
      if (!opcode::is_an_invoke(insn->opcode())) {
        TRACE(APP_MOD_USE, 6, "Investigating reflection \n");
        // If an object type is from refletion it will be in the RESULT_REGISTER
        // for some instruction
        const auto& o = analysis->get_abstract_object(RESULT_REGISTER, insn);
        if (o &&
            (o.get().obj_kind != reflection::CLASS ||
             (analysis->get_class_source(RESULT_REGISTER, insn).has_value() &&
              analysis->get_class_source(RESULT_REGISTER, insn).get() ==
                  reflection::REFLECTION))) {
          // If the obj is a CLASS then it must have a class source of
          // REFLECTION
          TRACE(APP_MOD_USE, 6, "Found an abstract object \n");
          type = type_used(o.get());
        }
      }
      if (type.has_value() && m_type_store_map.count(type.get()) > 0) {
        const auto store = m_type_store_map.at(type.get());
        if (store != root_store && store != method_store) {
          // App module reference!
          // add the store for the referenced type to the map
          m_stores_method_uses_reflectively_map.update(
              method,
              [store](DexMethod* /* method */,
                      std::unordered_set<unsigned int>& stores_used,
                      bool /* exists */) { stores_used.emplace(store); });
          TRACE(APP_MOD_USE,
                5,
                "%s used reflectively by %s\n",
                SHOW(type.get()),
                SHOW(method));
          m_stores_use_count.update(store,
                                    [](const unsigned int& /*store*/,
                                       AppModuleUsage::UseCount& count,
                                       bool /* exists */) {
                                      count.reflective_count =
                                          count.reflective_count + 1;
                                    });
        }
      }
    }
  });
}

template <typename T>
std::unordered_set<std::string> AppModuleUsagePass::get_modules_used(
    T* entrypoint, DexType* annotation_type) {
  std::unordered_set<std::string> modules = {};
  auto anno_set = entrypoint->get_anno_set();
  if (anno_set) {
    for (DexAnnotation* annotation : anno_set->get_annotations()) {
      if (annotation->type() == annotation_type) {
        for (const DexAnnotationElement& anno_elem : annotation->anno_elems()) {
          always_assert(anno_elem.string->str() == "value");
          always_assert(anno_elem.encoded_value->evtype() == DEVT_ARRAY);
          const auto* array =
              static_cast<const DexEncodedValueArray*>(anno_elem.encoded_value);
          for (const auto* value : *(array->evalues())) {
            always_assert(value->evtype() == DEVT_STRING);
            modules.emplace(((DexEncodedValueString*)value)->string()->str());
          }
        }
        break;
      }
    }
  }
  return modules;
}

template std::unordered_set<std::string>
AppModuleUsagePass::get_modules_used<DexMethod>(DexMethod*, DexType*);

template std::unordered_set<std::string>
AppModuleUsagePass::get_modules_used<DexField>(DexField*, DexType*);

template std::unordered_set<std::string>
AppModuleUsagePass::get_modules_used<DexClass>(DexClass*, DexType*);

void AppModuleUsagePass::generate_report(const DexStoresVector& stores,
                                         const ConfigFiles& conf,
                                         PassManager& mgr) {
  unsigned int violation_count = 0;
  auto annotation_type =
      DexType::make_type(m_uses_app_module_annotation_descriptor.c_str());
  auto path = conf.metafile(USES_AM_ANNO_VIOLATIONS_FILENAME);
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  // Method violations
  for (const auto& pair : m_stores_method_uses_map) {
    auto method = pair.first;
    bool print_name = true;
    auto annotated_module_names = get_modules_used(method, annotation_type);
    // combine annotations from class
    annotated_module_names.merge(
        get_modules_used(type_class(method->get_class()), annotation_type));
    // check for violations
    auto violation_check = [&](const auto& store) {
      const auto& used_module_name = stores.at(store).get_name();
      if (annotated_module_names.count(used_module_name) == 0) {
        violation(method, used_module_name, ofs, print_name);
        print_name = false;
        violation_count++;
      }
    };
    std::for_each(pair.second.begin(), pair.second.end(), violation_check);
    std::for_each(m_stores_method_uses_reflectively_map.at(method).begin(),
                  m_stores_method_uses_reflectively_map.at(method).end(),
                  [&](const auto& store) {
                    if (pair.second.count(store) == 0) {
                      violation_check(store);
                    }
                  });
    if (!print_name) {
      ofs << "\n";
    }
  }
  // Field violations
  walk::fields(build_class_scope(stores), [&](DexField* field) {
    auto annotated_module_names = get_modules_used(field, annotation_type);
    // combine annotations from class
    annotated_module_names.merge(
        get_modules_used(type_class(field->get_class()), annotation_type));
    bool print_name = true;
    if (m_type_store_map.count(field->get_type()) > 0 &&
        m_type_store_map.count(field->get_class()) > 0) {
      // get_type is the type of the field, the app module that class is from is
      // referenced by the field
      const auto& store_used =
          stores.at(m_type_store_map.at(field->get_type()));
      // get_class is the contatining class of the field, the app module that
      // class is in is the module the field is in
      const auto& store_from =
          stores.at(m_type_store_map.at(field->get_class()));
      if (!store_used.is_root_store() &&
          store_used.get_name() != store_from.get_name() &&
          annotated_module_names.count(store_used.get_name()) == 0) {
        violation(field, store_used.get_name(), ofs, print_name);
        print_name = false;
        violation_count++;
      }
    }
    if (!print_name) {
      ofs << "\n";
    }
  });
  mgr.set_metric("num_violations", violation_count);
}

template <typename T>
void AppModuleUsagePass::violation(T* entrypoint,
                                   const std::string& module,
                                   std::ofstream& ofs,
                                   bool print_name) {
  if (print_name) {
    ofs << SHOW(entrypoint);
  }
  ofs << ", " << module;
  TRACE(APP_MOD_USE,
        4,
        "%s uses app module \"%s\" without annotation\n",
        SHOW(entrypoint),
        module.c_str());
  always_assert_log(!m_crash_with_violations,
                    "%s uses app module \"%s\" without "
                    "@UsesAppModule annotation.\n",
                    SHOW(entrypoint), module.c_str());
}
template void AppModuleUsagePass::violation(DexMethod*,
                                            const std::string&,
                                            std::ofstream&,
                                            bool);
template void AppModuleUsagePass::violation(DexField*,
                                            const std::string&,
                                            std::ofstream&,
                                            bool);

void AppModuleUsagePass::output_usages(const DexStoresVector& stores,
                                       const ConfigFiles& conf) {
  auto path = conf.metafile(APP_MODULE_USAGE_OUTPUT_FILENAME);
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  for (const auto& pair : m_stores_method_uses_map) {
    auto reflective_references =
        m_stores_method_uses_reflectively_map.at(pair.first);
    if (!pair.second.empty() || !reflective_references.empty()) {
      ofs << "\"" << SHOW(pair.first) << "\"";
      for (unsigned int store_id : pair.second) {
        if (reflective_references.count(store_id) > 0) {
          ofs << ", \"(d&r)" << stores.at(store_id).get_name().c_str() << "\"";
        } else {
          ofs << ", \"" << stores.at(store_id).get_name() << "\"";
        }
      }
      for (unsigned int store_id : reflective_references) {
        if (pair.second.count(store_id) == 0) {
          ofs << ", \"(r)" << stores.at(store_id).get_name().c_str() << "\"";
        }
      }
      ofs << "\n";
    }
  }
}

void AppModuleUsagePass::output_use_count(const DexStoresVector& stores,
                                          const ConfigFiles& conf) {
  auto path = conf.metafile(APP_MODULE_COUNT_OUTPUT_FILENAME);
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  for (const auto& pair : m_stores_use_count) {
    ofs << "\"" << stores.at(pair.first).get_name() << "\", "
        << pair.second.direct_count << ", " << pair.second.reflective_count
        << "\n";
  }
}

static AppModuleUsagePass s_pass;
