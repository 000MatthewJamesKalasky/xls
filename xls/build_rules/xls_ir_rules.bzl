# Copyright 2021 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""This module contains IR-related build rules for XLS."""

load("@bazel_skylib//lib:dicts.bzl", "dicts")
load(
    "//xls/build_rules:xls_common_rules.bzl",
    "append_cmd_line_args_to",
    "append_default_to_args",
    "args_to_string",
    "get_output_filename_value",
    "get_runfiles_for_xls",
    "get_transitive_built_files_for_xls",
    "is_args_valid",
)
load("//xls/build_rules:xls_config_rules.bzl", "CONFIG")
load(
    "//xls/build_rules:xls_dslx_rules.bzl",
    "get_DslxInfo_from_dslx_library_as_input",
    "get_src_files_from_dslx_library_as_input",
    "xls_dslx_library_as_input_attrs",
)
load(
    "//xls/build_rules:xls_providers.bzl",
    "ConvIRInfo",
    "DslxInfo",
    "OptIRInfo",
)
load(
    "//xls/build_rules:xls_toolchains.bzl",
    "get_executable_from",
    "get_runfiles_from",
    "get_xls_toolchain_info",
    "xls_toolchain_attr",
)
load(
    "//xls/build_rules:xls_type_check_helpers.bzl",
    "bool_type_check",
    "tuple_type_check",
)

_DEFAULT_IR_EVAL_TEST_ARGS = {
    "random_inputs": "100",
    "optimize_ir": "true",
}

_DEFAULT_BENCHMARK_IR_ARGS = {}

_IR_FILE_EXTENSION = ".ir"

_OPT_IR_FILE_EXTENSION = ".opt.ir"

def append_xls_dslx_ir_generated_files(args, basename):
    """Returns a dictionary of arguments appended with filenames generated by the 'xls_dslx_ir' rule.

    Args:
      args: A dictionary of arguments.
      basename: The file basename.

    Returns:
      Returns a dictionary of arguments appended with filenames generated by the 'xls_dslx_ir' rule.
    """
    args.setdefault("ir_file", basename + _IR_FILE_EXTENSION)
    return args

def get_xls_dslx_ir_generated_files(args):
    """Returns a list of filenames generated by the 'xls_dslx_ir' rule found in 'args'.

    Args:
      args: A dictionary of arguments.

    Returns:
      Returns a list of files generated by the 'xls_dslx_ir' rule found in 'args'.
    """
    return [args.get("ir_file")]

def append_xls_ir_opt_ir_generated_files(args, basename):
    """Returns a dictionary of arguments appended with filenames generated by the 'xls_ir_opt_ir' rule.

    Args:
      args: A dictionary of arguments.
      basename: The file basename.

    Returns:
      Returns a dictionary of arguments appended with filenames generated by the 'xls_ir_opt_ir' rule.
    """
    args.setdefault("opt_ir_file", basename + _OPT_IR_FILE_EXTENSION)
    return args

def get_xls_ir_opt_ir_generated_files(args):
    """Returns a list of filenames generated by the 'xls_ir_opt_ir' rule found in 'args'.

    Args:
      args: A dictionary of arguments.

    Returns:
      Returns a list of files generated by the 'xls_ir_opt_ir' rule found in 'args'.
    """
    return [args.get("opt_ir_file")]

def _convert_to_ir(ctx, src):
    """Returns the runfiles and a File referencing the converted IR file.

    Creates an action in the context to convert a DSLX source file to an
    IR file.

    Args:
      ctx: The current rule's context object.
      src: The source file.
    Returns:
      A tuple with the following elements in the order presented:
        1. The runfiles to convert the IR file.
        1. The converted IR file.
    """
    ir_converter_tool = get_executable_from(
        get_xls_toolchain_info(ctx).ir_converter_tool,
    )
    IR_CONV_FLAGS = (
        "dslx_path",
        "emit_fail_as_assert",
        "warnings_as_errors",
        "disable_warnings",
    )

    # With runs outside a monorepo, the execution root for the workspace of
    # the binary can be different with the execroot, requiring to change
    # the dslx stdlib search path accordingly.
    # e.g., Label("@repo//pkg/xls:binary").workspace_root == "external/repo"
    wsroot = get_xls_toolchain_info(ctx).ir_converter_tool.label.workspace_root
    wsroot_dslx_path = ":{}".format(wsroot) if wsroot != "" else ""

    # Get workspaces for the source as well.
    # TODO(tedhong): 2023-06-07 - Grab the workspace from each dependency as well
    # to support dslx sources from different workspaces.
    dslx_srcs = [src]
    dslx_srcs_wsroot = ":".join([s.owner.workspace_root for s in dslx_srcs])
    dslx_srcs_wsroot_path = ":{}".format(dslx_srcs_wsroot) if dslx_srcs_wsroot != "" else ""

    ir_conv_args = dict(ctx.attr.ir_conv_args)
    ir_conv_args["dslx_path"] = (
        ir_conv_args.get("dslx_path", "") + ":${PWD}:" +
        ctx.genfiles_dir.path + ":" + ctx.bin_dir.path +
        dslx_srcs_wsroot_path + wsroot_dslx_path
    )

    is_args_valid(ir_conv_args, IR_CONV_FLAGS)

    ir_conv_args["top"] = ctx.attr.dslx_top
    my_args = args_to_string(ir_conv_args)

    ir_filename = get_output_filename_value(
        ctx,
        "ir_file",
        ctx.attr.name + _IR_FILE_EXTENSION,
    )
    ir_file = ctx.actions.declare_file(ir_filename)

    # Get runfiles
    ir_converter_tool_runfiles = get_runfiles_from(
        get_xls_toolchain_info(ctx).ir_converter_tool,
    )
    runfiles = get_runfiles_for_xls(ctx, [ir_converter_tool_runfiles], [src])

    ctx.actions.run_shell(
        outputs = [ir_file],
        # The IR converter executable is a tool needed by the action.
        tools = [ir_converter_tool],
        # The files required for converting the DSLX source file.
        inputs = runfiles.files,
        command = "{} {} {} > {}".format(
            ir_converter_tool.path,
            my_args,
            src.path,
            ir_file.path,
        ),
        mnemonic = "ConvertDSLX",
        progress_message = "Converting DSLX file to XLS IR: %s" % (src.path),
    )
    return runfiles, ir_file

def _optimize_ir(ctx, src):
    """Returns the runfiles and a File referencing the optimized IR file.

    Creates an action in the context to optimize an IR file.

    Args:
      ctx: The current rule's context object.
      src: The source file.
    Returns:
      A tuple with the following elements in the order presented:
        1. The runfiles to optimize the IR file.
        1. The optimized IR file.
    """
    opt_ir_tool = get_executable_from(get_xls_toolchain_info(ctx).opt_ir_tool)
    opt_ir_args = dict(ctx.attr.opt_ir_args)
    IR_OPT_FLAGS = (
        "ir_dump_path",
        "run_only_passes",
        "skip_passes",
        "opt_level",
        "convert_array_index_to_select",
        "inline_procs",
        "top",
    )

    is_args_valid(opt_ir_args, IR_OPT_FLAGS)

    if ctx.attr.top:
        opt_ir_args.setdefault("top", ctx.attr.top)

    my_args = args_to_string(opt_ir_args)
    if ctx.attr.ram_rewrites:
        ram_rewrites = "--ram_rewrites_pb=" + ",".join([",".join([file.path for file in ram_rewrite.files.to_list()]) for ram_rewrite in ctx.attr.ram_rewrites])
        my_args += " " + ram_rewrites

    opt_ir_filename = get_output_filename_value(
        ctx,
        "opt_ir_file",
        ctx.attr.name + _OPT_IR_FILE_EXTENSION,
    )
    opt_ir_file = ctx.actions.declare_file(opt_ir_filename)

    # Get runfiles
    opt_ir_tool_runfiles = get_runfiles_from(
        get_xls_toolchain_info(ctx).opt_ir_tool,
    )
    ram_rewrite_files = []
    for rewrite in ctx.attr.ram_rewrites:
        ram_rewrite_files.extend(rewrite.files.to_list())

    debug_src_files = []
    for debug_src in ctx.attr.debug_srcs:
        debug_src_files.extend(debug_src.files.to_list())

    runfiles = get_runfiles_for_xls(ctx, [opt_ir_tool_runfiles], [src] + ram_rewrite_files + debug_src_files)
    ctx.actions.run_shell(
        outputs = [opt_ir_file],
        # The IR optimization executable is a tool needed by the action.
        tools = [opt_ir_tool],
        # The files required for optimizing the IR file.
        inputs = runfiles.files,
        command = "{} {} {} > {}".format(
            opt_ir_tool.path,
            src.path,
            my_args,
            opt_ir_file.path,
        ),
        mnemonic = "OptimizeIR",
        progress_message = "Optimizing IR file: %s" % (src.path),
    )
    return runfiles, opt_ir_file

def get_ir_equivalence_test_cmd(
        ctx,
        src_0,
        src_1,
        append_cmd_line_args = True):
    """
    Returns the runfiles and command that executes in the ir_equivalence_test rule.

    Args:
      ctx: The current rule's context object.
      src_0: A file for the test.
      src_1: A file for the test.
      append_cmd_line_args: Flag controlling appending the command-line
        arguments invoking the command generated by this function. When set to
        True, the command-line arguments invoking the command are appended.
        Otherwise, the command-line arguments are not appended.

    Returns:
      A tuple with the following elements in the order presented:
        1. The runfiles to execute the command.
        1. The command.
    """
    ir_equivalence_tool = get_executable_from(
        get_xls_toolchain_info(ctx).ir_equivalence_tool,
    )
    IR_EQUIVALENCE_FLAGS = (
        "timeout",
    )

    ir_equivalence_args = dict(ctx.attr.ir_equivalence_args)
    is_args_valid(ir_equivalence_args, IR_EQUIVALENCE_FLAGS)
    if ctx.attr.top:
        ir_equivalence_args.setdefault("top", ctx.attr.top)
    my_args = args_to_string(ir_equivalence_args)

    cmd = "{} {} {} {}\n".format(
        ir_equivalence_tool.short_path,
        src_0.short_path,
        src_1.short_path,
        my_args,
    )

    # Append command-line arguments.
    if append_cmd_line_args:
        cmd = append_cmd_line_args_to(cmd)

    # Get runfiles
    ir_equivalence_tool_runfiles = get_runfiles_from(
        get_xls_toolchain_info(ctx).ir_equivalence_tool,
    )
    runfiles = get_runfiles_for_xls(
        ctx,
        [ir_equivalence_tool_runfiles],
        [src_0, src_1],
    )
    return runfiles, cmd

def get_eval_ir_test_cmd(ctx, src, append_cmd_line_args = True):
    """Returns the runfiles and command that executes in the xls_eval_ir_test rule.

    Args:
      ctx: The current rule's context object.
      src: The file to test.
      append_cmd_line_args: Flag controlling appending the command-line
        arguments invoking the command generated by this function. When set to
        True, the command-line arguments invoking the command are appended.
        Otherwise, the command-line arguments are not appended.

    Returns:
      A tuple with the following elements in the order presented:
        1. The runfiles to execute the command.
        1. The command.
    """
    ir_eval_tool = get_executable_from(get_xls_toolchain_info(ctx).ir_eval_tool)
    IR_EVAL_FLAGS = (
        "input",
        "input_file",
        "random_inputs",
        "expected",
        "expected_file",
        "optimize_ir",
        "eval_after_each_pass",
        "use_llvm_jit",
        "test_llvm_jit",
        "llvm_opt_level",
        "test_only_inject_jit_result",
    )

    ir_eval_args = append_default_to_args(
        ctx.attr.ir_eval_args,
        _DEFAULT_IR_EVAL_TEST_ARGS,
    )

    my_runfiles = []

    is_args_valid(ir_eval_args, IR_EVAL_FLAGS)
    if ctx.attr.input_validator:
        validator_info = ctx.attr.input_validator[DslxInfo]
        src_files = validator_info.target_dslx_source_files
        if not src_files or len(src_files) != 1:
            fail(
                "The input validator library must have a single DSLX src file.",
            )
        dslx_source_file = src_files[0]
        ir_eval_args["input_validator_path"] = dslx_source_file.short_path
        my_runfiles.append(dslx_source_file)
        my_runfiles = my_runfiles + validator_info.dslx_source_files.to_list()
    elif ctx.attr.input_validator_expr:
        ir_eval_args["input_validator_expr"] = "\"" + ctx.attr.input_validator_expr + "\""
    if ctx.attr.top:
        ir_eval_args.setdefault("top", ctx.attr.top)
    my_args = args_to_string(ir_eval_args)

    cmd = "{} {} {}".format(
        ir_eval_tool.short_path,
        src.short_path,
        my_args,
    )

    # Append command-line arguments.
    if append_cmd_line_args:
        cmd = append_cmd_line_args_to(cmd)

    # Get runfiles
    ir_eval_tool_runfiles = get_runfiles_from(
        get_xls_toolchain_info(ctx).ir_eval_tool,
    )
    runfiles = get_runfiles_for_xls(
        ctx,
        [ir_eval_tool_runfiles],
        my_runfiles + [src],
    )

    return runfiles, cmd

def get_benchmark_ir_cmd(ctx, src, append_cmd_line_args = True):
    """Returns the runfiles and command that executes in the xls_benchmark_ir rule.

    Args:
      ctx: The current rule's context object.
      src: The file to benchmark.
      append_cmd_line_args: Flag controlling appending the command-line
        arguments invoking the command generated by this function. When set to
        True, the command-line arguments invoking the command are appended.
        Otherwise, the command-line arguments are not appended.

    Returns:
      A tuple with the following elements in the order presented:
        1. The runfiles to execute the command.
        1. The command.
    """
    benchmark_ir_tool = get_executable_from(
        get_xls_toolchain_info(ctx).benchmark_ir_tool,
    )
    BENCHMARK_IR_FLAGS = (
        "clock_period_ps",
        "pipeline_stages",
        "clock_margin_percent",
        "show_known_bits",
        "delay_model",
        "convert_array_index_to_select",
        "scheduling_options_proto",
    )

    benchmark_ir_args = append_default_to_args(
        ctx.attr.benchmark_ir_args,
        _DEFAULT_BENCHMARK_IR_ARGS,
    )

    is_args_valid(benchmark_ir_args, BENCHMARK_IR_FLAGS)
    if ctx.attr.top:
        benchmark_ir_args.setdefault("top", ctx.attr.top)
    my_args = args_to_string(benchmark_ir_args)

    cmd = "{} {} {}".format(
        benchmark_ir_tool.short_path,
        src.short_path,
        my_args,
    )

    # Append command-line arguments.
    if append_cmd_line_args:
        cmd = append_cmd_line_args_to(cmd)

    # Get runfiles
    benchmark_ir_tool_runfiles = get_runfiles_from(
        get_xls_toolchain_info(ctx).benchmark_ir_tool,
    )
    runfiles = get_runfiles_for_xls(ctx, [benchmark_ir_tool_runfiles], [src])
    return runfiles, cmd

def get_mangled_ir_symbol(
        module_name,
        function_name,
        parametric_values = None,
        is_implicit_token = False,
        is_proc_next = False):
    """Returns the mangled IR symbol for the module/function combination.

    "Mangling" is the process of turning nicely namedspaced symbols into
    "grosser" (mangled) flat (non hierarchical) symbol, e.g. that lives on a
    package after IR conversion. To retrieve/execute functions that have been IR
    converted, we use their mangled names to refer to them in the IR namespace.

    Args:
      module_name: The DSLX module name that the function is within.
      function_name: The DSLX function name within the module.
      parametric_values: Any parametric values used for instantiation (e.g. for
        a parametric entry point that is known to be instantiated in the IR
        converted module). This is generally for more advanced use cases like
        internals testing. The argument is mutually exclusive with argument
        'is_proc_next'.
      is_implicit_token: A boolean flag denoting whether the symbol contains an
        implicit token. The argument is mutually exclusive with argument
        'is_proc_next'.
      is_proc_next: A boolean flag denoting whether the symbol is a
        next proc function. The argument is mutually exclusive with arguments:
        'parametric_values' and 'is_implicit_token'.

    Returns:
      The "mangled" symbol string.
    """

    # Type validation for optional inputs.
    tuple_type_check("parametric_values", parametric_values, True)
    bool_type_check("is_implicit_token", is_implicit_token, True)
    bool_type_check("is_proc_next", is_proc_next, True)

    # Presence validation for optional inputs.
    if is_proc_next and (parametric_values or is_implicit_token):
        fail("Argument 'is_proc_next' is mutually exclusive with arguments: " +
             "'parametric_values' and 'is_implicit_token'.")

    prefix_str = ""
    if is_implicit_token:
        prefix_str = "itok__"

    suffix = ""

    if parametric_values:
        suffix = "__" + "_".join(
            [
                str(v)
                for v in parametric_values
            ],
        )

    mangled_name = "__{}{}__{}{}".format(
        prefix_str,
        module_name,
        function_name,
        suffix,
    )

    if is_proc_next:
        mangled_name = mangled_name.replace(":", "_")
        mangled_name = mangled_name.replace("->", "__")
        mangled_name = mangled_name + "next"

    return mangled_name

xls_ir_top_attrs = {
    "top": attr.string(
        doc = "The (*mangled*) name of the entry point. See " +
              "get_mangled_ir_symbol. Defines the 'top' argument of the " +
              "IR tool/application.",
    ),
}

xls_ir_common_attrs = {
    "src": attr.label(
        doc = "The IR source file for the rule. A single source file must be " +
              "provided. The file must have a '.ir' extension.",
        mandatory = True,
        allow_single_file = [_IR_FILE_EXTENSION],
    ),
}

xls_dslx_ir_attrs = dicts.add(
    xls_dslx_library_as_input_attrs,
    {
        "dslx_top": attr.string(
            doc = "Defines the 'top' argument of the" +
                  "//xls/dslx/ir_convert:ir_converter_main application.",
            mandatory = True,
        ),
        "ir_conv_args": attr.string_dict(
            doc = "Arguments of the IR conversion tool. For details on the " +
                  "arguments, refer to the ir_converter_main application at " +
                  "//xls/dslx/ir_convert/ir_converter_main.cc. Note the " +
                  "'top' argument is not assigned using this attribute.",
        ),
        "ir_file": attr.output(
            doc = "Filename of the generated IR. If not specified, the " +
                  "target name of the bazel rule followed by an " +
                  _IR_FILE_EXTENSION + " extension is used.",
        ),
    },
)

def xls_dslx_ir_impl(ctx):
    """The implementation of the 'xls_dslx_ir' rule.

    Converts a DSLX source file to an IR file.

    Args:
      ctx: The current rule's context object.

    Returns:
      A tuple with the following elements in the order presented:
        1. The DslxInfo provider
        1. The ConvIRInfo provider
        1. The list of built files.
        1. The runfiles.
    """
    srcs = get_src_files_from_dslx_library_as_input(ctx)

    if srcs and len(srcs) != 1:
        fail("A single source file must be specified.")

    src = srcs[0]

    runfiles, ir_file = _convert_to_ir(ctx, src)

    dslx_info = get_DslxInfo_from_dslx_library_as_input(ctx)
    return [
        dslx_info,
        ConvIRInfo(conv_ir_file = ir_file),
        [ir_file],
        runfiles,
    ]

def _xls_dslx_ir_impl_wrapper(ctx):
    """The implementation of the 'xls_dslx_ir' rule.

    Wrapper for xls_dslx_ir_impl. See: xls_dslx_ir_impl.

    Args:
      ctx: The current rule's context object.

    Returns:
      DslxInfo provider
      ConvIRInfo provider
      DefaultInfo provider
    """
    dslx_info, ir_conv_info, built_files, runfiles = (
        xls_dslx_ir_impl(ctx)
    )
    return [
        dslx_info,
        ir_conv_info,
        DefaultInfo(
            files = depset(
                direct = built_files,
                transitive = get_transitive_built_files_for_xls(ctx),
            ),
            runfiles = runfiles,
        ),
    ]

xls_dslx_ir = rule(
    doc = """
        A build rule that converts a DSLX source file to an IR file.

Example:

An IR conversion with a top entity defined.

    ```
    # Assume a xls_dslx_library target bc_dslx is present.
    xls_dslx_ir(
        name = "d_ir",
        srcs = ["d.x"],
        deps = [":bc_dslx"],
        dslx_top = "d",
    )
    ```
    """,
    implementation = _xls_dslx_ir_impl_wrapper,
    attrs = dicts.add(
        xls_dslx_ir_attrs,
        CONFIG["xls_outs_attrs"],
        xls_toolchain_attr,
    ),
)

def xls_ir_opt_ir_impl(ctx, src):
    """The implementation of the 'xls_ir_opt_ir' rule.

    Optimizes an IR file.

    Args:
      ctx: The current rule's context object.
      src: The source file.

    Returns:
      A tuple with the following elements in the order presented:
        1. The OptIRInfo provider
        1. The list of built files.
        1. The runfiles.
    """
    runfiles, opt_ir_file = _optimize_ir(ctx, src)
    return [
        OptIRInfo(
            input_ir_file = src,
            opt_ir_file = opt_ir_file,
            opt_ir_args = ctx.attr.opt_ir_args,
        ),
        [opt_ir_file],
        runfiles,
    ]

xls_ir_opt_ir_attrs = dicts.add(
    xls_ir_top_attrs,
    {
        "opt_ir_args": attr.string_dict(
            doc = "Arguments of the IR optimizer tool. For details on the" +
                  "arguments, refer to the opt_main application at" +
                  "//xls/tools/opt_main.cc. The 'top' " +
                  "argument is not assigned using this attribute.",
        ),
        "opt_ir_file": attr.output(
            doc = "Filename of the generated optimized IR. If not specified, " +
                  "the target name of the bazel rule followed by an " +
                  _OPT_IR_FILE_EXTENSION + " extension is used.",
        ),
        "ram_rewrites": attr.label_list(doc = "List of ram rewrite protos.", allow_files = True),
        "debug_srcs": attr.label_list(
            doc = "List of additional source files for debugging info.",
            allow_files = True,
        ),
    },
)

def _xls_ir_opt_ir_impl_wrapper(ctx):
    """The implementation of the 'xls_ir_opt_ir' rule.

    Wrapper for xls_ir_opt_ir_impl. See: xls_ir_opt_ir_impl.

    Args:
      ctx: The current rule's context object.

    Returns:
      OptIRInfo provider
      DefaultInfo provider
    """
    ir_opt_info, built_files_list, runfiles = xls_ir_opt_ir_impl(
        ctx,
        ctx.file.src,
    )

    return [
        ir_opt_info,
        DefaultInfo(
            files = depset(
                direct = built_files_list,
                transitive = get_transitive_built_files_for_xls(
                    ctx,
                    [ctx.attr.src],
                ),
            ),
            runfiles = runfiles,
        ),
    ]

xls_ir_opt_ir = rule(
    doc = """A build rule that optimizes an IR file.

Examples:

1. A simple example.

    ```
    xls_ir_opt_ir(
        name = "a_opt_ir",
        src = "a.ir",
    )
    ```

1. Optimizing an IR file with a top entity defined.

    ```
    xls_ir_opt_ir(
        name = "a_opt_ir",
        src = "a.ir",
        opt_ir_args = {
            "top" : "a",
        },
    )
    ```
    """,
    implementation = _xls_ir_opt_ir_impl_wrapper,
    attrs = dicts.add(
        xls_ir_common_attrs,
        xls_ir_opt_ir_attrs,
        CONFIG["xls_outs_attrs"],
        xls_toolchain_attr,
    ),
)

def _xls_ir_equivalence_test_impl(ctx):
    """The implementation of the 'xls_ir_equivalence_test' rule.

    Executes the equivalence tool on two IR files.

    Args:
      ctx: The current rule's context object.

    Returns:
      DefaultInfo provider
    """
    ir_file_a = ctx.file.src_0
    ir_file_b = ctx.file.src_1

    runfiles, cmd = get_ir_equivalence_test_cmd(ctx, ir_file_a, ir_file_b)
    executable_file = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = executable_file,
        content = "\n".join([
            "#!/usr/bin/env bash",
            "set -e",
            cmd,
            "exit 0",
        ]),
        is_executable = True,
    )

    return [
        DefaultInfo(
            runfiles = runfiles,
            files = depset(
                direct = [executable_file],
                transitive = get_transitive_built_files_for_xls(
                    ctx,
                    [ctx.attr.src_0, ctx.attr.src_1],
                ),
            ),
            executable = executable_file,
        ),
    ]

_two_ir_files_attrs = {
    "src_0": attr.label(
        doc = "An IR source file for the rule. A single source file must be " +
              "provided. The file must have a '.ir' extension.",
        mandatory = True,
        allow_single_file = [_IR_FILE_EXTENSION],
    ),
    "src_1": attr.label(
        doc = "An IR source file for the rule. A single source file must be " +
              "provided. The file must have a '.ir' extension.",
        mandatory = True,
        allow_single_file = [_IR_FILE_EXTENSION],
    ),
}

xls_ir_equivalence_test_attrs = {
    "ir_equivalence_args": attr.string_dict(
        doc = "Arguments of the IR equivalence tool. For details on the " +
              "arguments, refer to the check_ir_equivalence_main application " +
              "at //xls/tools/check_ir_equivalence_main.cc. " +
              "The 'function' argument is not assigned using this attribute.",
    ),
}

xls_ir_equivalence_test = rule(
    doc = """Executes the equivalence tool on two IR files.

Examples:

1. A file as the source.

    ```
    xls_ir_equivalence_test(
        name = "ab_ir_equivalence_test",
        src_0 = "a.ir",
        src_1 = "b.ir",
    )
    ```

1. A target as the source.

    ```
    xls_dslx_ir(
        name = "b_ir",
        srcs = ["b.x"],
    )

    xls_ir_equivalence_test(
        name = "ab_ir_equivalence_test",
        src_0 = "a.ir",
        src_1 = ":b_ir",
    )
    ```
    """,
    implementation = _xls_ir_equivalence_test_impl,
    attrs = dicts.add(
        _two_ir_files_attrs,
        xls_ir_equivalence_test_attrs,
        xls_ir_top_attrs,
        xls_toolchain_attr,
    ),
    test = True,
)

def _xls_eval_ir_test_impl(ctx):
    """The implementation of the 'xls_eval_ir_test' rule.

    Executes the IR Interpreter on an IR file.

    Args:
      ctx: The current rule's context object.
    Returns:
      DefaultInfo provider
    """
    if ctx.attr.input_validator and ctx.attr.input_validator_expr:
        fail(msg = "Only one of \"input_validator\" or \"input_validator_expr\" " +
                   "may be specified for a single \"xls_eval_ir_test\" rule.")
    src = ctx.file.src

    runfiles, cmd = get_eval_ir_test_cmd(ctx, src)
    executable_file = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = executable_file,
        content = "\n".join([
            "#!/usr/bin/env bash",
            "set -e",
            cmd,
            "exit 0",
        ]),
        is_executable = True,
    )

    return [
        DefaultInfo(
            runfiles = runfiles,
            files = depset(
                direct = [executable_file],
                transitive = get_transitive_built_files_for_xls(
                    ctx,
                    [ctx.attr.src],
                ),
            ),
            executable = executable_file,
        ),
    ]

xls_eval_ir_test_attrs = {
    "input_validator": attr.label(
        doc = "The DSLX library defining the input validator for this test. " +
              "Mutually exclusive with \"input_validator_expr\".",
        providers = [DslxInfo],
        allow_files = True,
    ),
    "input_validator_expr": attr.string(
        doc = "The expression to validate an input for the test function. " +
              "Mutually exclusive with \"input_validator\".",
    ),
    "ir_eval_args": attr.string_dict(
        doc = "Arguments of the IR interpreter. For details on the " +
              "arguments, refer to the eval_ir_main application at " +
              "//xls/tools/eval_ir_main.cc." +
              "The 'top' argument is not assigned using this attribute.",
        default = _DEFAULT_IR_EVAL_TEST_ARGS,
    ),
}

xls_eval_ir_test = rule(
    doc = """Executes the IR interpreter on an IR file.

Examples:

1. A file as the source.

    ```
    xls_eval_ir_test(
        name = "a_eval_ir_test",
        src = "a.ir",
    )
    ```

1. An xls_ir_opt_ir target as the source.

    ```
    xls_ir_opt_ir(
        name = "a_opt_ir",
        src = "a.ir",
    )


    xls_eval_ir_test(
        name = "a_eval_ir_test",
        src = ":a_opt_ir",
    )
    ```
    """,
    implementation = _xls_eval_ir_test_impl,
    attrs = dicts.add(
        xls_ir_common_attrs,
        xls_eval_ir_test_attrs,
        xls_ir_top_attrs,
        xls_toolchain_attr,
    ),
    test = True,
)

def _xls_benchmark_ir_impl(ctx):
    """The implementation of the 'xls_benchmark_ir' rule.

    Executes the benchmark tool on an IR file.

    Args:
      ctx: The current rule's context object.
    Returns:
      DefaultInfo provider
    """
    src = ctx.file.src

    runfiles, cmd = get_benchmark_ir_cmd(ctx, src)
    executable_file = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = executable_file,
        content = "\n".join([
            "#!/usr/bin/env bash",
            "set -e",
            cmd,
            "exit 0",
        ]),
        is_executable = True,
    )

    return [
        DefaultInfo(
            runfiles = runfiles,
            files = depset(
                direct = [executable_file],
                transitive = get_transitive_built_files_for_xls(
                    ctx,
                    [ctx.attr.src],
                ),
            ),
            executable = executable_file,
        ),
    ]

xls_benchmark_ir_attrs = {
    "benchmark_ir_args": attr.string_dict(
        doc = "Arguments of the benchmark IR tool. For details on the " +
              "arguments, refer to the benchmark_main application at " +
              "//xls/tools/benchmark_main.cc.",
    ),
    "scheduling_options_proto": attr.label(
        allow_single_file = True,
        default = None,
        doc = "Protobuf filename of scheduling arguments to the benchmark IR tool. " +
              "For details on the arguments, refer to the benchmark_main application at " +
              "//xls/tools/benchmark_main.cc.",
    ),
}

xls_benchmark_ir = rule(
    doc = """Executes the benchmark tool on an IR file.

Examples:

1. A file as the source.

    ```
    xls_benchmark_ir(
        name = "a_benchmark",
        src = "a.ir",
    )
    ```

1. An xls_ir_opt_ir target as the source.

    ```
    xls_ir_opt_ir(
        name = "a_opt_ir",
        src = "a.ir",
    )


    xls_benchmark_ir(
        name = "a_benchmark",
        src = ":a_opt_ir",
    )
    ```
    """,
    implementation = _xls_benchmark_ir_impl,
    attrs = dicts.add(
        xls_ir_common_attrs,
        xls_benchmark_ir_attrs,
        xls_ir_top_attrs,
        xls_toolchain_attr,
    ),
    executable = True,
)

def _xls_ir_cc_library_impl(ctx):
    """The implementation of the 'xls_ir_cc_library' rule.

    Executes the AOT compiler on the specified IR.

    Args:
      ctx: The current rule's context object.
    Returns:
      DefaultInfo provider
    """
    src = ctx.file.src

    # Source files (.h and .cc) files are first generated unformatted, then
    # formatted with clangformat.
    object_file = ctx.actions.declare_file(ctx.outputs.object_file.basename)
    unformatted_header_file = ctx.actions.declare_file(
        ctx.outputs.header_file.basename + ".unformatted",
    )
    unformatted_source_file = ctx.actions.declare_file(
        ctx.outputs.source_file.basename + ".unformatted",
    )

    header_file = ctx.actions.declare_file(ctx.outputs.header_file.basename)
    source_file = ctx.actions.declare_file(ctx.outputs.source_file.basename)

    aot_compiler_args = ctx.actions.args()
    aot_compiler_args.add("-input", src)
    aot_compiler_args.add("-output_header", unformatted_header_file.path)
    aot_compiler_args.add("-output_object", object_file.path)
    aot_compiler_args.add("-output_source", unformatted_source_file.path)
    aot_compiler_args.add("-header_include_path", header_file.short_path)
    if ctx.attr.namespaces:
        aot_compiler_args.add("-namespaces", ctx.attr.namespaces)

    aot_compiler_tool = get_executable_from(
        get_xls_toolchain_info(ctx).aot_compiler_tool,
    )

    aot_compiler_tool_runfiles = get_runfiles_from(
        get_xls_toolchain_info(ctx).aot_compiler_tool,
    )
    runfiles = get_runfiles_for_xls(ctx, [aot_compiler_tool_runfiles], [src])

    ctx.actions.run(
        outputs = [object_file, unformatted_header_file, unformatted_source_file],
        tools = [aot_compiler_tool],
        inputs = runfiles.files,
        arguments = [aot_compiler_args],
        executable = aot_compiler_tool.path,
        mnemonic = "AOTCompile",
        progress_message = "AOT compiling %s" % src.path,
    )

    ctx.actions.run_shell(
        inputs = [unformatted_header_file],
        outputs = [header_file],
        tools = [ctx.executable._clang_format],
        progress_message = "Formatting %s" % header_file.basename,
        command = "{clang_format} {unformatted} > {formatted}".format(
            clang_format = ctx.executable._clang_format.path,
            unformatted = unformatted_header_file.path,
            formatted = header_file.path,
        ),
    )

    ctx.actions.run_shell(
        inputs = [unformatted_source_file],
        outputs = [source_file],
        tools = [ctx.executable._clang_format],
        progress_message = "Formatting %s" % source_file.basename,
        command = "{clang_format} {unformatted} > {formatted}".format(
            clang_format = ctx.executable._clang_format.path,
            unformatted = unformatted_source_file.path,
            formatted = source_file.path,
        ),
    )

    return [
        DefaultInfo(
            files = depset(
                direct = [object_file, header_file, source_file],
                transitive = get_transitive_built_files_for_xls(
                    ctx,
                    [ctx.attr.src],
                ),
            ),
            runfiles = runfiles,
        ),
    ]

xls_ir_cc_library = rule(
    doc = """Creates a native cc_library from an IR file.

    Not meant to be directly instantiated; use xls_ir_cc_library_macro (in
    xls_ir_macros.bzl) instead.
    """,
    implementation = _xls_ir_cc_library_impl,
    attrs = dicts.add(
        xls_ir_common_attrs,
        xls_ir_top_attrs,
        xls_toolchain_attr,
        {
            "header_file": attr.output(
                doc = "Name of the generated header file.",
                mandatory = True,
            ),
            "object_file": attr.output(
                doc = "Name of the generated object file.",
                mandatory = True,
            ),
            "source_file": attr.output(
                doc = "Name of the generated source file.",
                mandatory = True,
            ),
            "namespaces": attr.string(
                doc = "Comma-separated list of nested namespaces in which to " +
                      "place the generated function.",
            ),
            "_clang_format": attr.label(
                executable = True,
                allow_files = True,
                cfg = "exec",
                default = Label("@llvm_toolchain_llvm//:bin/clang-format"),
            ),
        },
    ),
)
