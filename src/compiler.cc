// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "bootstrapper.h"
#include "codegen-inl.h"
#include "compilation-cache.h"
#include "compiler.h"
#include "data-flow.h"
#include "debug.h"
#include "fast-codegen.h"
#include "flow-graph.h"
#include "full-codegen.h"
#include "liveedit.h"
#include "oprofile-agent.h"
#include "rewriter.h"
#include "scopes.h"

namespace v8 {
namespace internal {


static Handle<Code> MakeCode(Handle<Context> context, CompilationInfo* info) {
  FunctionLiteral* function = info->function();
  ASSERT(function != NULL);
  // Rewrite the AST by introducing .result assignments where needed.
  if (!Rewriter::Process(function)) {
    // Signal a stack overflow by returning a null handle.  The stack
    // overflow exception will be thrown by the caller.
    return Handle<Code>::null();
  }

  {
    // Compute top scope and allocate variables. For lazy compilation
    // the top scope only contains the single lazily compiled function,
    // so this doesn't re-allocate variables repeatedly.
    HistogramTimerScope timer(&Counters::variable_allocation);
    Scope* top = info->scope();
    while (top->outer_scope() != NULL) top = top->outer_scope();
    top->AllocateVariables(context);
  }

#ifdef DEBUG
  if (Bootstrapper::IsActive() ?
      FLAG_print_builtin_scopes :
      FLAG_print_scopes) {
    info->scope()->Print();
  }
#endif

  // Optimize the AST.
  if (!Rewriter::Optimize(function)) {
    // Signal a stack overflow by returning a null handle.  The stack
    // overflow exception will be thrown by the caller.
    return Handle<Code>::null();
  }

  if (function->scope()->num_parameters() > 0 ||
      function->scope()->num_stack_slots()) {
    AssignedVariablesAnalyzer ava(function);
    ava.Analyze();
    if (ava.HasStackOverflow()) {
      return Handle<Code>::null();
    }
  }

  if (FLAG_use_flow_graph) {
    FlowGraphBuilder builder;
    FlowGraph* graph = builder.Build(function);
    USE(graph);

#ifdef DEBUG
    if (FLAG_print_graph_text && !builder.HasStackOverflow()) {
      graph->PrintAsText(function->name());
    }
#endif
  }

  // Generate code and return it.  Code generator selection is governed by
  // which backends are enabled and whether the function is considered
  // run-once code or not:
  //
  //  --full-compiler enables the dedicated backend for code we expect to be
  //    run once
  //  --fast-compiler enables a speculative optimizing backend (for
  //    non-run-once code)
  //
  // The normal choice of backend can be overridden with the flags
  // --always-full-compiler and --always-fast-compiler, which are mutually
  // incompatible.
  CHECK(!FLAG_always_full_compiler || !FLAG_always_fast_compiler);

  Handle<SharedFunctionInfo> shared = info->shared_info();
  bool is_run_once = (shared.is_null())
      ? info->scope()->is_global_scope()
      : (shared->is_toplevel() || shared->try_full_codegen());

  bool force_full_compiler = false;
#if defined(V8_TARGET_ARCH_IA32) || defined(V8_TARGET_ARCH_X64)
  // On ia32 the full compiler can compile all code whereas the other platforms
  // the constructs supported is checked by the associated syntax checker. When
  // --always-full-compiler is used on ia32 the syntax checker is still in
  // effect, but there is a special flag --force-full-compiler to ignore the
  // syntax checker completely and use the full compiler for all code. Also
  // when debugging on ia32 the full compiler will be used for all code.
  force_full_compiler =
      Debugger::IsDebuggerActive() || FLAG_force_full_compiler;
#endif

  if (force_full_compiler) {
    return FullCodeGenerator::MakeCode(info);
  } else if (FLAG_always_full_compiler || (FLAG_full_compiler && is_run_once)) {
    FullCodeGenSyntaxChecker checker;
    checker.Check(function);
    if (checker.has_supported_syntax()) {
      return FullCodeGenerator::MakeCode(info);
    }
  } else if (FLAG_always_fast_compiler ||
             (FLAG_fast_compiler && !is_run_once)) {
    FastCodeGenSyntaxChecker checker;
    checker.Check(info);
    if (checker.has_supported_syntax()) {
      return FastCodeGenerator::MakeCode(info);
    }
  }

  return CodeGenerator::MakeCode(info);
}


#ifdef ENABLE_DEBUGGER_SUPPORT
Handle<Code> MakeCodeForLiveEdit(CompilationInfo* info) {
  Handle<Context> context = Handle<Context>::null();
  return MakeCode(context, info);
}
#endif


static Handle<SharedFunctionInfo> MakeFunctionInfo(bool is_global,
    bool is_eval,
    Compiler::ValidationState validate,
    Handle<Script> script,
    Handle<Context> context,
    v8::Extension* extension,
    ScriptDataImpl* pre_data) {
  CompilationZoneScope zone_scope(DELETE_ON_EXIT);

  PostponeInterruptsScope postpone;

  ASSERT(!i::Top::global_context().is_null());
  script->set_context_data((*i::Top::global_context())->data());

  bool is_json = (validate == Compiler::VALIDATE_JSON);
#ifdef ENABLE_DEBUGGER_SUPPORT
  if (is_eval || is_json) {
    script->set_compilation_type(
        is_json ? Smi::FromInt(Script::COMPILATION_TYPE_JSON) :
                               Smi::FromInt(Script::COMPILATION_TYPE_EVAL));
    // For eval scripts add information on the function from which eval was
    // called.
    if (is_eval) {
      StackTraceFrameIterator it;
      if (!it.done()) {
        script->set_eval_from_shared(
            JSFunction::cast(it.frame()->function())->shared());
        int offset = static_cast<int>(
            it.frame()->pc() - it.frame()->code()->instruction_start());
        script->set_eval_from_instructions_offset(Smi::FromInt(offset));
      }
    }
  }

  // Notify debugger
  Debugger::OnBeforeCompile(script);
#endif

  // Only allow non-global compiles for eval.
  ASSERT(is_eval || is_global);

  // Build AST.
  FunctionLiteral* lit =
      MakeAST(is_global, script, extension, pre_data, is_json);

  LiveEditFunctionTracker live_edit_tracker(lit);

  // Check for parse errors.
  if (lit == NULL) {
    ASSERT(Top::has_pending_exception());
    return Handle<SharedFunctionInfo>::null();
  }

  // Measure how long it takes to do the compilation; only take the
  // rest of the function into account to avoid overlap with the
  // parsing statistics.
  HistogramTimer* rate = is_eval
      ? &Counters::compile_eval
      : &Counters::compile;
  HistogramTimerScope timer(rate);

  // Compile the code.
  CompilationInfo info(lit, script, is_eval);
  Handle<Code> code = MakeCode(context, &info);

  // Check for stack-overflow exceptions.
  if (code.is_null()) {
    Top::StackOverflow();
    return Handle<SharedFunctionInfo>::null();
  }

  if (script->name()->IsString()) {
    PROFILE(CodeCreateEvent(
        is_eval ? Logger::EVAL_TAG :
            Logger::ToNativeByScript(Logger::SCRIPT_TAG, *script),
        *code, String::cast(script->name())));
    OPROFILE(CreateNativeCodeRegion(String::cast(script->name()),
                                    code->instruction_start(),
                                    code->instruction_size()));
  } else {
    PROFILE(CodeCreateEvent(
        is_eval ? Logger::EVAL_TAG :
            Logger::ToNativeByScript(Logger::SCRIPT_TAG, *script),
        *code, ""));
    OPROFILE(CreateNativeCodeRegion(is_eval ? "Eval" : "Script",
                                    code->instruction_start(),
                                    code->instruction_size()));
  }

  // Allocate function.
  Handle<SharedFunctionInfo> result =
      Factory::NewSharedFunctionInfo(lit->name(),
                                     lit->materialized_literal_count(),
                                     code);

  ASSERT_EQ(RelocInfo::kNoPosition, lit->function_token_position());
  Compiler::SetFunctionInfo(result, lit, true, script);

  // Hint to the runtime system used when allocating space for initial
  // property space by setting the expected number of properties for
  // the instances of the function.
  SetExpectedNofPropertiesFromEstimate(result, lit->expected_property_count());

#ifdef ENABLE_DEBUGGER_SUPPORT
  // Notify debugger
  Debugger::OnAfterCompile(script, Debugger::NO_AFTER_COMPILE_FLAGS);
#endif

  live_edit_tracker.RecordFunctionInfo(result, lit);

  return result;
}


static StaticResource<SafeStringInputBuffer> safe_string_input_buffer;


Handle<SharedFunctionInfo> Compiler::Compile(Handle<String> source,
                                             Handle<Object> script_name,
                                             int line_offset,
                                             int column_offset,
                                             v8::Extension* extension,
                                             ScriptDataImpl* input_pre_data,
                                             Handle<Object> script_data,
                                             NativesFlag natives) {
  int source_length = source->length();
  Counters::total_load_size.Increment(source_length);
  Counters::total_compile_size.Increment(source_length);

  // The VM is in the COMPILER state until exiting this function.
  VMState state(COMPILER);

  // Do a lookup in the compilation cache but not for extensions.
  Handle<SharedFunctionInfo> result;
  if (extension == NULL) {
    result = CompilationCache::LookupScript(source,
                                            script_name,
                                            line_offset,
                                            column_offset);
  }

  if (result.is_null()) {
    // No cache entry found. Do pre-parsing and compile the script.
    ScriptDataImpl* pre_data = input_pre_data;
    if (pre_data == NULL && source_length >= FLAG_min_preparse_length) {
      Access<SafeStringInputBuffer> buf(&safe_string_input_buffer);
      buf->Reset(source.location());
      pre_data = PreParse(source, buf.value(), extension);
    }

    // Create a script object describing the script to be compiled.
    Handle<Script> script = Factory::NewScript(source);
    if (natives == NATIVES_CODE) {
      script->set_type(Smi::FromInt(Script::TYPE_NATIVE));
    }
    if (!script_name.is_null()) {
      script->set_name(*script_name);
      script->set_line_offset(Smi::FromInt(line_offset));
      script->set_column_offset(Smi::FromInt(column_offset));
    }

    script->set_data(script_data.is_null() ? Heap::undefined_value()
                                           : *script_data);

    // Compile the function and add it to the cache.
    result = MakeFunctionInfo(true,
                              false,
                              DONT_VALIDATE_JSON,
                              script,
                              Handle<Context>::null(),
                              extension,
                              pre_data);
    if (extension == NULL && !result.is_null()) {
      CompilationCache::PutScript(source, result);
    }

    // Get rid of the pre-parsing data (if necessary).
    if (input_pre_data == NULL && pre_data != NULL) {
      delete pre_data;
    }
  }

  if (result.is_null()) Top::ReportPendingMessages();
  return result;
}


Handle<SharedFunctionInfo> Compiler::CompileEval(Handle<String> source,
                                                 Handle<Context> context,
                                                 bool is_global,
                                                 ValidationState validate) {
  // Note that if validation is required then no path through this
  // function is allowed to return a value without validating that
  // the input is legal json.

  int source_length = source->length();
  Counters::total_eval_size.Increment(source_length);
  Counters::total_compile_size.Increment(source_length);

  // The VM is in the COMPILER state until exiting this function.
  VMState state(COMPILER);

  // Do a lookup in the compilation cache; if the entry is not there,
  // invoke the compiler and add the result to the cache.  If we're
  // evaluating json we bypass the cache since we can't be sure a
  // potential value in the cache has been validated.
  Handle<SharedFunctionInfo> result;
  if (validate == DONT_VALIDATE_JSON)
    result = CompilationCache::LookupEval(source, context, is_global);

  if (result.is_null()) {
    // Create a script object describing the script to be compiled.
    Handle<Script> script = Factory::NewScript(source);
    result = MakeFunctionInfo(is_global,
                              true,
                              validate,
                              script,
                              context,
                              NULL,
                              NULL);
    if (!result.is_null() && validate != VALIDATE_JSON) {
      // For json it's unlikely that we'll ever see exactly the same
      // string again so we don't use the compilation cache.
      CompilationCache::PutEval(source, context, is_global, result);
    }
  }

  return result;
}


bool Compiler::CompileLazy(CompilationInfo* info) {
  CompilationZoneScope zone_scope(DELETE_ON_EXIT);

  // The VM is in the COMPILER state until exiting this function.
  VMState state(COMPILER);

  PostponeInterruptsScope postpone;

  // Compute name, source code and script data.
  Handle<SharedFunctionInfo> shared = info->shared_info();
  Handle<String> name(String::cast(shared->name()));

  int start_position = shared->start_position();
  int end_position = shared->end_position();
  bool is_expression = shared->is_expression();
  Counters::total_compile_size.Increment(end_position - start_position);

  // Generate the AST for the lazily compiled function. The AST may be
  // NULL in case of parser stack overflow.
  FunctionLiteral* lit = MakeLazyAST(info->script(),
                                     name,
                                     start_position,
                                     end_position,
                                     is_expression);

  // Check for parse errors.
  if (lit == NULL) {
    ASSERT(Top::has_pending_exception());
    return false;
  }
  info->set_function(lit);

  // Measure how long it takes to do the lazy compilation; only take
  // the rest of the function into account to avoid overlap with the
  // lazy parsing statistics.
  HistogramTimerScope timer(&Counters::compile_lazy);

  // Compile the code.
  Handle<Code> code = MakeCode(Handle<Context>::null(), info);

  // Check for stack-overflow exception.
  if (code.is_null()) {
    Top::StackOverflow();
    return false;
  }

  RecordFunctionCompilation(Logger::LAZY_COMPILE_TAG,
                            name,
                            Handle<String>(shared->inferred_name()),
                            start_position,
                            info->script(),
                            code);

  // Update the shared function info with the compiled code.
  shared->set_code(*code);

  // Set the expected number of properties for instances.
  SetExpectedNofPropertiesFromEstimate(shared, lit->expected_property_count());

  // Set the optimication hints after performing lazy compilation, as these are
  // not set when the function is set up as a lazily compiled function.
  shared->SetThisPropertyAssignmentsInfo(
      lit->has_only_simple_this_property_assignments(),
      *lit->this_property_assignments());

  // Check the function has compiled code.
  ASSERT(shared->is_compiled());
  return true;
}


Handle<SharedFunctionInfo> Compiler::BuildFunctionInfo(FunctionLiteral* literal,
                                                       Handle<Script> script,
                                                       AstVisitor* caller) {
  LiveEditFunctionTracker live_edit_tracker(literal);
#ifdef DEBUG
  // We should not try to compile the same function literal more than
  // once.
  literal->mark_as_compiled();
#endif

  // Determine if the function can be lazily compiled. This is
  // necessary to allow some of our builtin JS files to be lazily
  // compiled. These builtins cannot be handled lazily by the parser,
  // since we have to know if a function uses the special natives
  // syntax, which is something the parser records.
  bool allow_lazy = literal->AllowsLazyCompilation() &&
      !LiveEditFunctionTracker::IsActive();

  // Generate code
  Handle<Code> code;
  if (FLAG_lazy && allow_lazy) {
    code = ComputeLazyCompile(literal->num_parameters());
  } else {
    // The bodies of function literals have not yet been visited by
    // the AST optimizer/analyzer.
    if (!Rewriter::Optimize(literal)) {
      return Handle<SharedFunctionInfo>::null();
    }

    if (literal->scope()->num_parameters() > 0 ||
        literal->scope()->num_stack_slots()) {
      AssignedVariablesAnalyzer ava(literal);
      ava.Analyze();
      if (ava.HasStackOverflow()) {
        return Handle<SharedFunctionInfo>::null();
      }
    }

    if (FLAG_use_flow_graph) {
      FlowGraphBuilder builder;
      FlowGraph* graph = builder.Build(literal);
      USE(graph);

#ifdef DEBUG
      if (FLAG_print_graph_text && !builder.HasStackOverflow()) {
        graph->PrintAsText(literal->name());
      }
#endif
    }

    // Generate code and return it.  The way that the compilation mode
    // is controlled by the command-line flags is described in
    // the static helper function MakeCode.
    CompilationInfo info(literal, script, false);

    CHECK(!FLAG_always_full_compiler || !FLAG_always_fast_compiler);
    bool is_run_once = literal->try_full_codegen();
    bool is_compiled = false;
    if (FLAG_always_full_compiler || (FLAG_full_compiler && is_run_once)) {
      FullCodeGenSyntaxChecker checker;
      checker.Check(literal);
      if (checker.has_supported_syntax()) {
        code = FullCodeGenerator::MakeCode(&info);
        is_compiled = true;
      }
    } else if (FLAG_always_fast_compiler ||
               (FLAG_fast_compiler && !is_run_once)) {
      // Since we are not lazily compiling we do not have a receiver to
      // specialize for.
      FastCodeGenSyntaxChecker checker;
      checker.Check(&info);
      if (checker.has_supported_syntax()) {
        code = FastCodeGenerator::MakeCode(&info);
        is_compiled = true;
      }
    }

    if (!is_compiled) {
      // We fall back to the classic V8 code generator.
      code = CodeGenerator::MakeCode(&info);
    }

    // Check for stack-overflow exception.
    if (code.is_null()) {
      caller->SetStackOverflow();
      return Handle<SharedFunctionInfo>::null();
    }

    // Function compilation complete.
    RecordFunctionCompilation(Logger::FUNCTION_TAG,
                              literal->name(),
                              literal->inferred_name(),
                              literal->start_position(),
                              script,
                              code);
  }

  // Create a shared function info object.
  Handle<SharedFunctionInfo> result =
      Factory::NewSharedFunctionInfo(literal->name(),
                                     literal->materialized_literal_count(),
                                     code);
  SetFunctionInfo(result, literal, false, script);

  // Set the expected number of properties for instances and return
  // the resulting function.
  SetExpectedNofPropertiesFromEstimate(result,
                                       literal->expected_property_count());
  live_edit_tracker.RecordFunctionInfo(result, literal);
  return result;
}


// Sets the function info on a function.
// The start_position points to the first '(' character after the function name
// in the full script source. When counting characters in the script source the
// the first character is number 0 (not 1).
void Compiler::SetFunctionInfo(Handle<SharedFunctionInfo> function_info,
                               FunctionLiteral* lit,
                               bool is_toplevel,
                               Handle<Script> script) {
  function_info->set_length(lit->num_parameters());
  function_info->set_formal_parameter_count(lit->num_parameters());
  function_info->set_script(*script);
  function_info->set_function_token_position(lit->function_token_position());
  function_info->set_start_position(lit->start_position());
  function_info->set_end_position(lit->end_position());
  function_info->set_is_expression(lit->is_expression());
  function_info->set_is_toplevel(is_toplevel);
  function_info->set_inferred_name(*lit->inferred_name());
  function_info->SetThisPropertyAssignmentsInfo(
      lit->has_only_simple_this_property_assignments(),
      *lit->this_property_assignments());
  function_info->set_try_full_codegen(lit->try_full_codegen());
}


void Compiler::RecordFunctionCompilation(Logger::LogEventsAndTags tag,
                                         Handle<String> name,
                                         Handle<String> inferred_name,
                                         int start_position,
                                         Handle<Script> script,
                                         Handle<Code> code) {
  // Log the code generation. If source information is available
  // include script name and line number. Check explicitly whether
  // logging is enabled as finding the line number is not free.
  if (Logger::is_logging()
      || OProfileAgent::is_enabled()
      || CpuProfiler::is_profiling()) {
    Handle<String> func_name(name->length() > 0 ? *name : *inferred_name);
    if (script->name()->IsString()) {
      int line_num = GetScriptLineNumber(script, start_position) + 1;
      USE(line_num);
      PROFILE(CodeCreateEvent(Logger::ToNativeByScript(tag, *script),
                              *code, *func_name,
                              String::cast(script->name()), line_num));
      OPROFILE(CreateNativeCodeRegion(*func_name,
                                      String::cast(script->name()),
                                      line_num,
                                      code->instruction_start(),
                                      code->instruction_size()));
    } else {
      PROFILE(CodeCreateEvent(Logger::ToNativeByScript(tag, *script),
                              *code, *func_name));
      OPROFILE(CreateNativeCodeRegion(*func_name,
                                      code->instruction_start(),
                                      code->instruction_size()));
    }
  }
}

} }  // namespace v8::internal
