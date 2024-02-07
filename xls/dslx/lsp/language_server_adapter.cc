// Copyright 2023 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/lsp/language_server_adapter.h"

#include <cstdint>
#include <filesystem>  // NOLINT
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "external/verible/common/lsp/lsp-file-utils.h"
#include "external/verible/common/lsp/lsp-protocol.h"
#include "xls/common/indent.h"
#include "xls/common/logging/logging.h"
#include "xls/common/logging/vlog_is_on.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/extract_module_name.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/frontend/bindings.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/lsp/document_symbols.h"
#include "xls/dslx/lsp/find_definition.h"
#include "xls/dslx/lsp/lsp_type_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/warning_collector.h"
#include "xls/dslx/warning_kind.h"

namespace xls::dslx {
namespace {

static const char kSource[] = "DSLX";

// Convert error included in status message to LSP Diagnostic
void AppendDiagnosticFromStatus(
    const absl::Status& status,
    std::vector<verible::lsp::Diagnostic>* diagnostic_sink) {
  absl::StatusOr<PositionalErrorData> extracted_error_or =
      GetPositionalErrorData(status, std::nullopt);
  if (!extracted_error_or.ok()) {
    LspLog() << extracted_error_or.status() << "\n";
    return;  // best effort. Ignore.
  }
  const PositionalErrorData& err = *extracted_error_or;
  diagnostic_sink->push_back(
      verible::lsp::Diagnostic{.range = ConvertSpanToLspRange(err.span),
                               .source = kSource,
                               .message = err.message});
}

void AppendDiagnosticFromTypecheck(
    const TypecheckedModule& module,
    std::vector<verible::lsp::Diagnostic>* diagnostic_sink) {
  for (const WarningCollector::Entry& warning : module.warnings.warnings()) {
    diagnostic_sink->push_back(
        verible::lsp::Diagnostic{.range = ConvertSpanToLspRange(warning.span),
                                 .source = kSource,
                                 .message = warning.message});
  }
}

}  // namespace

LanguageServerAdapter::LanguageServerAdapter(
    std::string_view stdlib,
    const std::vector<std::filesystem::path>& dslx_paths)
    : stdlib_(stdlib), dslx_paths_(dslx_paths) {}

const LanguageServerAdapter::ParseData* LanguageServerAdapter::FindParsedForUri(
    std::string_view uri) const {
  if (auto found = uri_parse_data_.find(uri); found != uri_parse_data_.end()) {
    return found->second.get();
  }
  return nullptr;
}

absl::Status LanguageServerAdapter::Update(std::string_view file_uri,
                                           std::string_view dslx_code) {
  const absl::Time start = absl::Now();
  absl::StatusOr<std::string> module_name_or = ExtractModuleName(file_uri);
  if (!module_name_or.ok()) {
    LspLog() << "Could not determine module name from file URI: " << file_uri
             << " status: " << module_name_or.status() << "\n";
    return absl::OkStatus();
  }

  auto inserted = uri_parse_data_.emplace(file_uri, nullptr);
  std::unique_ptr<ParseData>& insert_value = inserted.first->second;

  ImportData import_data =
      CreateImportData(stdlib_, dslx_paths_, kAllWarningsSet);
  const std::string& module_name = module_name_or.value();
  absl::StatusOr<TypecheckedModule> typechecked_module = ParseAndTypecheck(
      dslx_code, /*path=*/file_uri, /*module_name=*/module_name, &import_data);

  insert_value.reset(
      new ParseData({.import_data = std::move(import_data),
                     .typechecked_module = std::move(typechecked_module)}));

  const absl::Duration duration = absl::Now() - start;
  if (duration > absl::Milliseconds(200)) {
    LspLog() << "Parsing " << file_uri << " took " << duration << "\n";
  }

  return insert_value->status();
}

std::vector<verible::lsp::Diagnostic>
LanguageServerAdapter::GenerateParseDiagnostics(std::string_view uri) const {
  std::vector<verible::lsp::Diagnostic> result;
  if (const ParseData* parsed = FindParsedForUri(uri)) {
    if (parsed->ok()) {
      const TypecheckedModule& tm = *parsed->typechecked_module;
      AppendDiagnosticFromTypecheck(tm, &result);
    } else {
      AppendDiagnosticFromStatus(parsed->status(), &result);
    }
  }
  return result;
}

std::vector<verible::lsp::DocumentSymbol>
LanguageServerAdapter::GenerateDocumentSymbols(std::string_view uri) const {
  XLS_VLOG(1) << "GenerateDocumentSymbols; uri: " << uri;
  if (const ParseData* parsed = FindParsedForUri(uri); parsed && parsed->ok()) {
    return ToDocumentSymbols(parsed->module());
  }
  return {};
}

std::vector<verible::lsp::Location> LanguageServerAdapter::FindDefinitions(
    std::string_view uri, const verible::lsp::Position& position) const {
  const Pos pos = ConvertLspPositionToPos(uri, position);
  XLS_VLOG(1) << "FindDefinition; uri: " << uri << " pos: " << pos;
  if (const ParseData* parsed = FindParsedForUri(uri); parsed && parsed->ok()) {
    std::optional<Span> maybe_definition_span =
        xls::dslx::FindDefinition(parsed->module(), pos);
    if (maybe_definition_span.has_value()) {
      verible::lsp::Location location =
          ConvertSpanToLspLocation(maybe_definition_span.value());
      location.uri = uri;
      return {location};
    }
  }
  return {};
}

absl::StatusOr<std::vector<verible::lsp::TextEdit>>
LanguageServerAdapter::FormatRange(std::string_view uri,
                                   const verible::lsp::Range& range) const {
  // TODO(cdleary): 2023-05-25 We start simple, formatting only when the
  // requested range exactly intercepts a block.
  //
  // Note: At least in vim the visual range selected is an exclusive limit in
  // `:LspDocumentRangeFormat`, so if you want the last character in a line to
  // be included it's not clear what you can do. This is annoying!
  const Span target = ConvertLspRangeToSpan(uri, range);
  if (const ParseData* parsed = FindParsedForUri(uri); parsed && parsed->ok()) {
    const Module& module = parsed->module();
    const AstNode* intercepting_block =
        module.FindNode(AstNodeKind::kBlock, target);
    if (intercepting_block == nullptr) {
      if (XLS_VLOG_IS_ON(5)) {
        std::vector<const AstNode*> intercepting_start =
            module.FindIntercepting(target.start());
        for (const AstNode* node : intercepting_start) {
          XLS_VLOG(5) << node->GetSpan().value() << " :: " << node->ToString();
        }
      }
      return absl::NotFoundError(absl::StrCat(
          "Could not find a formattable AST node with the target range: " +
              target.ToString(),
          " -- note that currently only single blocks are supported"));
    }
    std::string new_text = intercepting_block->ToString();
    int64_t indent_level = DetermineIndentLevel(*intercepting_block->parent());
    new_text = Indent(new_text, indent_level * kRustSpacesPerIndent);
    return std::vector<verible::lsp::TextEdit>{
        verible::lsp::TextEdit{.range = range, .newText = new_text}};
  }
  return absl::FailedPreconditionError(
      "Language server did not have a successful prior parse to format.");
}

std::vector<verible::lsp::DocumentLink>
LanguageServerAdapter::ProvideImportLinks(std::string_view uri) const {
  std::vector<verible::lsp::DocumentLink> result;
  if (const ParseData* parsed = FindParsedForUri(uri); parsed && parsed->ok()) {
    const Module& module = parsed->module();
    for (const auto& [_, import_node] : module.GetImportByName()) {
      absl::StatusOr<ImportTokens> tok =
          ImportTokens::FromString(import_node->identifier());
      if (!tok.ok()) {
        continue;
      }
      absl::StatusOr<ModuleInfo*> info = parsed->import_data.Get(tok.value());
      if (!info.ok()) {
        continue;
      }
      verible::lsp::DocumentLink link = {
          .range = ConvertSpanToLspRange(import_node->name_def().span()),
          .target = verible::lsp::PathToLSPUri(info.value()->path().string()),
          .has_target = true,
      };
      result.emplace_back(link);
    }
  }
  return result;
}

}  // namespace xls::dslx
