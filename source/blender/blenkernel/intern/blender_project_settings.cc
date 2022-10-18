/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <fstream>
#include <memory>
#include <string>

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_util.h"
#include "BLI_serialize.hh"

#include "BKE_asset_library_custom.h"
#include "BKE_blender_project.hh"

#include "DNA_asset_types.h"

namespace serialize = blender::io::serialize;

namespace blender::bke {

/* ---------------------------------------------------------------------- */

ProjectSettings::ProjectSettings(StringRef project_root_path)
    : project_root_path_(project_root_path)
{
}

bool ProjectSettings::create_settings_directory(StringRef project_root_path)
{
  std::string project_root_path_native = project_root_path;
  BLI_path_slash_native(project_root_path_native.data());

  return BLI_dir_create_recursive(
      std::string(project_root_path_native + SEP + SETTINGS_DIRNAME).c_str());
}

bool ProjectSettings::delete_settings_directory()
{
  BLI_assert(project_root_path_[0] == SEP);
  std::string dot_blender_project_dir_path = project_root_path_ + SETTINGS_DIRNAME;

  /* Returns 0 on success. */
  if (BLI_delete(dot_blender_project_dir_path.c_str(), true, true)) {
    return false;
  }

  has_unsaved_changes_ = true;
  return true;
}

/* ---------------------------------------------------------------------- */

CustomAssetLibraries::CustomAssetLibraries(ListBase asset_libraries)
    : asset_libraries(asset_libraries)
{
}

CustomAssetLibraries::CustomAssetLibraries(CustomAssetLibraries &&other)
{
  *this = std::move(other);
}

CustomAssetLibraries &CustomAssetLibraries::operator=(CustomAssetLibraries &&other)
{
  asset_libraries = other.asset_libraries;
  BLI_listbase_clear(&other.asset_libraries);
  return *this;
}

CustomAssetLibraries::~CustomAssetLibraries()
{
  LISTBASE_FOREACH_MUTABLE (CustomAssetLibraryDefinition *, library, &asset_libraries) {
    BKE_asset_library_custom_remove(&asset_libraries, library);
  }
}

/* ---------------------------------------------------------------------- */
/** \name settings.json Reading (Deserializing)
 * \{ */

struct ExtractedSettings {
  std::string project_name;
  ListBase asset_libraries = {nullptr, nullptr}; /* CustomAssetLibraryDefinition */
};

static std::unique_ptr<serialize::Value> read_settings_file(StringRef settings_filepath)
{
  std::ifstream is;
  is.open(settings_filepath);
  if (is.fail()) {
    return nullptr;
  }

  serialize::JsonFormatter formatter;
  /* Will not be a dictionary in case of error (corrupted file). */
  std::unique_ptr<serialize::Value> deserialized_values = formatter.deserialize(is);
  is.close();

  if (deserialized_values->type() != serialize::eValueType::Dictionary) {
    return nullptr;
  }

  return deserialized_values;
}

static std::unique_ptr<ExtractedSettings> extract_settings(
    const serialize::DictionaryValue &dictionary)
{
  using namespace serialize;

  std::unique_ptr extracted_settings = std::make_unique<ExtractedSettings>();

  const DictionaryValue::Lookup attributes = dictionary.create_lookup();

  /* "project": */ {
    const DictionaryValue::LookupValue *project_value = attributes.lookup_ptr("project");
    BLI_assert(project_value != nullptr);

    const DictionaryValue *project_dict = (*project_value)->as_dictionary_value();
    const StringValue *project_name_value =
        project_dict->create_lookup().lookup("name")->as_string_value();
    if (project_name_value) {
      extracted_settings->project_name = project_name_value->value();
    }
  }
  /* "asset_libraries": */ {
    const DictionaryValue::LookupValue *asset_libraries_value = attributes.lookup_ptr(
        "asset_libraries");
    if (asset_libraries_value) {
      const ArrayValue *asset_libraries_array = (*asset_libraries_value)->as_array_value();
      if (!asset_libraries_array) {
        throw std::runtime_error(
            "Unexpected asset_library format in settings.json, expected array");
      }

      for (const ArrayValue::Item &element : asset_libraries_array->elements()) {
        const DictionaryValue *object_value = element->as_dictionary_value();
        if (!object_value) {
          throw std::runtime_error(
              "Unexpected asset_library entry in settings.json, expected dictionary entries only");
        }
        const DictionaryValue::Lookup element_lookup = object_value->create_lookup();
        const DictionaryValue::LookupValue *name_value = element_lookup.lookup_ptr("name");
        if (name_value && (*name_value)->type() != eValueType::String) {
          throw std::runtime_error(
              "Unexpected asset_library entry in settings.json, expected name to be string");
        }
        const DictionaryValue::LookupValue *path_value = element_lookup.lookup_ptr("path");
        if (path_value && (*path_value)->type() != eValueType::String) {
          throw std::runtime_error(
              "Unexpected asset_library entry in settings.json, expected path to be string");
        }

        /* TODO this isn't really extracting, should be creating data from the settings be a
         * separate step? */
        CustomAssetLibraryDefinition *library = BKE_asset_library_custom_add(
            &extracted_settings->asset_libraries);
        /* Name or path may not be set, this is fine. */
        if (name_value) {
          std::string name = (*name_value)->as_string_value()->value();
          BKE_asset_library_custom_name_set(
              &extracted_settings->asset_libraries, library, name.c_str());
        }
        if (path_value) {
          std::string path = (*path_value)->as_string_value()->value();
          BKE_asset_library_custom_path_set(library, path.c_str());
        }
      }
    }
  }

  return extracted_settings;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name settings.json Writing (Serializing)
 * \{ */

std::unique_ptr<serialize::DictionaryValue> ProjectSettings::to_dictionary() const
{
  using namespace serialize;

  std::unique_ptr<DictionaryValue> root = std::make_unique<DictionaryValue>();
  DictionaryValue::Items &root_attributes = root->elements();

  /* "project": */ {
    std::unique_ptr<DictionaryValue> project_dict = std::make_unique<DictionaryValue>();
    DictionaryValue::Items &project_attributes = project_dict->elements();
    project_attributes.append_as("name", new StringValue(project_name_));
    root_attributes.append_as("project", std::move(project_dict));
  }
  /* "asset_libraries": */ {
    if (!BLI_listbase_is_empty(&asset_libraries_.asset_libraries)) {
      std::unique_ptr<ArrayValue> asset_libs_array = std::make_unique<ArrayValue>();
      ArrayValue::Items &asset_libs_elements = asset_libs_array->elements();
      LISTBASE_FOREACH (
          const CustomAssetLibraryDefinition *, library, &asset_libraries_.asset_libraries) {
        std::unique_ptr<DictionaryValue> library_dict = std::make_unique<DictionaryValue>();
        DictionaryValue::Items &library_attributes = library_dict->elements();

        library_attributes.append_as("name", new StringValue(library->name));
        library_attributes.append_as("path", new StringValue(library->path));
        asset_libs_elements.append_as(std::move(library_dict));
      }
      root_attributes.append_as("asset_libraries", std::move(asset_libs_array));
    }
  }

  return root;
}

static void write_settings_file(StringRef settings_filepath,
                                std::unique_ptr<serialize::DictionaryValue> dictionary)
{
  using namespace serialize;

  JsonFormatter formatter;

  std::ofstream os;
  os.open(settings_filepath, std::ios::out | std::ios::trunc);
  formatter.serialize(os, *dictionary);
  os.close();
}

/** \} */

/* ---------------------------------------------------------------------- */

struct ResolvedPaths {
  std::string settings_filepath;
  std::string project_root_path;
};

static StringRef path_strip_trailing_native_slash(StringRef path)
{
  const int64_t pos_before_trailing_slash = path.find_last_not_of(SEP);
  return (pos_before_trailing_slash == StringRef::not_found) ?
             path :
             path.substr(0, pos_before_trailing_slash + 1);
}

/**
 * Returned paths can be assumed to use native slashes.
 */
static ResolvedPaths resolve_paths_from_project_path(StringRef project_path)
{
  std::string project_path_native = project_path;
  BLI_path_slash_native(project_path_native.data());

  ResolvedPaths resolved_paths{};

  const StringRef path_no_trailing_slashes = path_strip_trailing_native_slash(project_path_native);
  if (path_no_trailing_slashes.endswith(ProjectSettings::SETTINGS_DIRNAME)) {
    resolved_paths.project_root_path =
        StringRef(path_no_trailing_slashes).drop_suffix(ProjectSettings::SETTINGS_DIRNAME.size());
  }
  else {
    resolved_paths.project_root_path = std::string(path_no_trailing_slashes) + SEP;
  }
  resolved_paths.settings_filepath = resolved_paths.project_root_path +
                                     ProjectSettings::SETTINGS_DIRNAME + SEP +
                                     ProjectSettings::SETTINGS_FILENAME;

  return resolved_paths;
}

std::unique_ptr<ProjectSettings> ProjectSettings::load_from_disk(StringRef project_path)
{
  ResolvedPaths paths = resolve_paths_from_project_path(project_path);

  if (!BLI_exists(paths.project_root_path.c_str())) {
    return nullptr;
  }
  if (!BlenderProject::path_is_project_root(paths.project_root_path.c_str())) {
    return nullptr;
  }

  std::unique_ptr<serialize::Value> values = read_settings_file(paths.settings_filepath);
  std::unique_ptr<ExtractedSettings> extracted_settings = nullptr;
  if (values) {
    BLI_assert(values->as_dictionary_value() != nullptr);
    extracted_settings = extract_settings(*values->as_dictionary_value());
  }

  std::unique_ptr loaded_settings = std::make_unique<ProjectSettings>(paths.project_root_path);
  if (extracted_settings) {
    loaded_settings->project_name_ = extracted_settings->project_name;
    /* Moves ownership. */
    loaded_settings->asset_libraries_ = CustomAssetLibraries(extracted_settings->asset_libraries);
  }

  return loaded_settings;
}

std::unique_ptr<ProjectSettings> ProjectSettings::load_from_path(StringRef path)
{
  StringRef project_root = BlenderProject::project_root_path_find_from_path(path);
  if (project_root.is_empty()) {
    return nullptr;
  }

  return bke::ProjectSettings::load_from_disk(project_root);
}

bool ProjectSettings::save_to_disk(StringRef project_path)
{
  ResolvedPaths paths = resolve_paths_from_project_path(project_path);

  if (!BLI_exists(paths.project_root_path.c_str())) {
    return false;
  }
  if (!BlenderProject::path_is_project_root(paths.project_root_path.c_str())) {
    return false;
  }

  std::unique_ptr settings_as_dict = to_dictionary();
  write_settings_file(paths.settings_filepath, std::move(settings_as_dict));

  has_unsaved_changes_ = false;

  return true;
}

/* ---------------------------------------------------------------------- */

StringRefNull ProjectSettings::project_root_path() const
{
  return project_root_path_;
}

void ProjectSettings::project_name(StringRef new_name)
{
  project_name_ = new_name;
  has_unsaved_changes_ = true;
}

StringRefNull ProjectSettings::project_name() const
{
  return project_name_;
}

const ListBase &ProjectSettings::asset_library_definitions() const
{
  return asset_libraries_.asset_libraries;
}

ListBase &ProjectSettings::asset_library_definitions()
{
  return asset_libraries_.asset_libraries;
}

void ProjectSettings::tag_has_unsaved_changes()
{
  has_unsaved_changes_ = true;
}

bool ProjectSettings::has_unsaved_changes() const
{
  return has_unsaved_changes_;
}

}  // namespace blender::bke