// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/jit_flag_processor.h"

#include "cinderx/Common/log.h"

#include <fmt/format.h>

#include <memory>
#include <sstream>
#include <string>

namespace cinderx::jit {

constexpr std::string_view indent1 = "         ";
constexpr std::string_view indent2 = "             ";
constexpr size_t line_length = 80 - indent1.size();

bool FlagProcessor::hasOptions() {
  return options_.size() > 0;
}

Option& FlagProcessor::addOption(
    const std::string& cmdline_flag,
    const std::string& environment_variable,
    const std::function<void()>& callback,
    const std::string& flag_description) {
  return addOption(
      cmdline_flag,
      environment_variable,
      [=](const std::string&) { callback(); },
      flag_description);
}

Option& FlagProcessor::addOption(
    const std::string& cmdline_flag,
    const std::string& environment_variable,
    const std::function<void(int)>& callback,
    const std::string& flag_description) {
  assert(!cmdline_flag.empty());
  assert(!flag_description.empty());

  std::function<void(const std::string&)> int_callback =
      [=](const std::string& flag_value) {
        try {
          // The callback only gets called for empty X-options, not empty
          // environment variables. This makes `-X foo` equivalent to `-X
          // foo=1`, but `PYTHONFOO=` is not equivalent to `PYTHONFOO=1`.
          callback(flag_value == "" ? 1 : std::stoi(flag_value));
        } catch (std::exception const&) {
          JIT_LOG(
              "Invalid int value for {}/{}: {}",
              cmdline_flag,
              environment_variable,
              flag_value);
        }
      };

  return addOption(
      cmdline_flag, environment_variable, int_callback, flag_description);
}

Option& FlagProcessor::addOption(
    const std::string& cmdline_flag,
    const std::string& environment_variable,
    const std::function<void(const std::string&)>& callback,
    const std::string& flag_description) {
  assert(!cmdline_flag.empty());
  assert(!flag_description.empty());

  auto option = std::make_unique<Option>(
      cmdline_flag, environment_variable, callback, flag_description);
  Option& optref = *option;

  options_.push_back(std::move(option));

  return optref;
}

Option& FlagProcessor::addOption(
    const std::string& cmdline_flag,
    const std::string& environment_variable,
    std::string& variable_to_bind_to,
    const std::string& flag_description) {
  std::function<void(const std::string&)> setter =
      [&variable_to_bind_to](const std::string& flag_value) {
        variable_to_bind_to = flag_value;
      };
  return addOption(
      cmdline_flag, environment_variable, setter, flag_description);
}

Option& FlagProcessor::addOption(
    const std::string& cmdline_flag,
    const std::string& environment_variable,
    bool& variable_to_bind_to,
    const std::string& flag_description) {
  std::function<void(int)> setter = [&variable_to_bind_to](int flag_value) {
    variable_to_bind_to = static_cast<bool>(flag_value);
  };
  return addOption(
      cmdline_flag, environment_variable, setter, flag_description);
}

Option& FlagProcessor::addOption(
    const std::string& cmdline_flag,
    const std::string& environment_variable,
    int& variable_to_bind_to,
    const std::string& flag_description) {
  std::function<void(int)> setter = [&variable_to_bind_to](int flag_value) {
    variable_to_bind_to = flag_value;
  };
  return addOption(
      cmdline_flag, environment_variable, setter, flag_description);
}

Option& FlagProcessor::addOption(
    const std::string& cmdline_flag,
    const std::string& environment_variable,
    size_t& variable_to_bind_to,
    const std::string& flag_description) {
  std::function<void(const std::string&)> setter =
      [=, &variable_to_bind_to](const std::string& flag_value) {
        try {
          // The callback only gets called for empty X-options, not empty
          // environment variables. This makes `-X foo` equivalent to `-X
          // foo=1`, but `PYTHONFOO=` is not equivalent to `PYTHONFOO=1`.
          variable_to_bind_to = flag_value == "" ? 1 : std::stoull(flag_value);
        } catch (std::exception const&) {
          JIT_LOG(
              "Invalid unsigned long value for {}/{}: {}",
              cmdline_flag,
              environment_variable,
              flag_value);
        }
      };
  return addOption(
      cmdline_flag, environment_variable, setter, flag_description);
}

bool FlagProcessor::canHandle(std::string_view option_name) {
  for (auto const& option : options_) {
    if (option->cmdline_flag == option_name) {
      return true;
    }
  }
  return false;
}

bool FlagProcessor::hasHandled(std::string_view option_name) {
  for (auto const& option : options_) {
    if (option->cmdline_flag == option_name) {
      return option->handled;
    }
  }
  return false;
}

void FlagProcessor::setFlags(PyObject* cmdline_args) {
  for (auto& option : options_) {
    option->handled = false;

    // Check if option is specified in the command line arguments.
    std::string found;
    if (handleCliFlag(*option, cmdline_args)) {
      found = option->cmdline_flag;
    }

    // Otherwise check to see if it can be found via an environment variable.
    if (found.empty() && handleEnvVar(*option)) {
      found = option->environment_variable;
    }

    if (!found.empty()) {
      option->handled = true;

      // Log everything but skip the debug option itself.
      if (option->cmdline_flag != "cinderx-jit-debug") {
        // Use overridden debug message if it's been defined.
        std::string_view msg = option->debug_message.empty()
            ? option->flag_description
            : option->debug_message;
        JIT_DLOG("{} has been specified - {}", found, msg);
      }
    }
  }

  // Check for unrecognized "-X cinderx-jit..." options.
  PyObject* key;
  PyObject* value;
  for (Py_ssize_t pos = 0; PyDict_Next(cmdline_args, &pos, &key, &value);) {
    const char* raw_option = PyUnicode_AsUTF8(key);
    JIT_CHECK(
        raw_option != nullptr,
        "Failed to convert command line key of type '{}' to a string",
        Py_TYPE(key)->tp_name);
    std::string_view option = raw_option;
    if (option.starts_with("cinderx-jit") && !canHandle(option)) {
      JIT_LOG("Warning: JIT cannot handle X-option {}", option);
    }
  }
}

bool FlagProcessor::handleCliFlag(
    const Option& option,
    BorrowedRef<> cmdline_args) {
  const char* flag = option.cmdline_flag.c_str();
  auto key = Ref<>::steal(PyUnicode_FromString(flag));
  JIT_CHECK(key != nullptr, "Failed to allocate Python string for '{}'", flag);

  // Check if option is specified in the command line arguments.
  std::string found;
  BorrowedRef<> resolves_to = PyDict_GetItem(cmdline_args, key);

  // If it wasn't found, try the old school "-X jit-..." scheme.
  if (resolves_to == nullptr) {
    constexpr std::string_view prefix = "cinderx-";
    std::string_view flag_view = flag;
    if (flag_view.starts_with(prefix)) {
      flag_view.remove_prefix(prefix.size());
      // This is safe because the string view is still NUL-terminated.
      key = Ref<>::steal(PyUnicode_FromString(flag_view.data()));
      JIT_CHECK(
          key != nullptr,
          "Failed to allocate Python string for '{}'",
          flag_view);
      resolves_to = PyDict_GetItem(cmdline_args, key);
    }
  }

  if (resolves_to != nullptr) {
    const char* resolved =
        PyUnicode_Check(resolves_to) ? PyUnicode_AsUTF8(resolves_to) : "";
    option.callback_on_match(resolved);
    return true;
  }

  return false;
}

bool FlagProcessor::handleEnvVar(const Option& option) {
  const std::string& var_name = option.environment_variable;
  if (var_name.empty()) {
    return false;
  }

  const char* env_val = Py_GETENV(var_name.c_str());
  if (env_val != nullptr && env_val[0] != '\0') {
    option.callback_on_match(env_val);
    return true;
  }

  // Didn't find it under the "CINDERX_JIT_..." naming scheme, try the
  // old-school "PYTHONJIT..." scheme.
  std::string old_var_name = option.environment_variable;
  const std::string_view prefix = "CINDERX_JIT";
  if (old_var_name.starts_with(prefix)) {
    old_var_name.replace(0, prefix.size(), "PYTHONJIT");
  }
  std::erase(old_var_name, '_');

  env_val = Py_GETENV(old_var_name.c_str());
  if (env_val != nullptr && env_val[0] != '\0') {
    option.callback_on_match(env_val);
    return true;
  }

  return false;
}

// split long lines into many, but only cut on whitespace
static std::string multi_line_split_(const std::string& src_string) {
  std::vector<std::string> temp_result(1);

  std::stringstream stm(src_string);

  std::string word;
  bool addIndent = false;
  while (stm >> word) {
    if (addIndent) {
      temp_result.emplace_back(indent2);
    }

    if ((temp_result.back().size() + word.size()) <= line_length) {
      temp_result.back() += word + ' ';
      addIndent = false;
    } else {
      temp_result.push_back(word + "\n");
      addIndent = true;
    }
  }

  temp_result.back().pop_back();
  std::string result;
  for (const auto& item : temp_result) {
    result += item;
  }
  return result;
}

std::string Option::getFormatted(std::string left_hand_side) {
  if (!flag_param_name.empty()) {
    return fmt::format("{}=<{}>", left_hand_side, flag_param_name);
  }
  return left_hand_side;
}

std::string Option::getFormattedCmdlineFlag() {
  return getFormatted(cmdline_flag);
}

std::string Option::getFormattedEnvironmentVariable() {
  return environment_variable.empty() ? "" : getFormatted(environment_variable);
}

std::string FlagProcessor::jitXOptionHelpMessage() {
  std::string ret =
      "-X opt : set Cinder JIT-specific option. The following options are "
      "available:\n\n";
  for (auto const& option : options_) {
    if (!option->hidden_flag) {
      std::string fmt_env_var =
          option->getFormattedEnvironmentVariable().empty()
          ? ""
          : fmt::format("; also {}", option->getFormattedEnvironmentVariable());
      ret += indent1;
      ret += multi_line_split_(
                 fmt::format(
                     "-X {}: {}{}\n",
                     option->getFormattedCmdlineFlag(),
                     option->flag_description,
                     fmt_env_var)) +
          "\n";
    }
  }
  return ret;
}

} // namespace cinderx::jit
