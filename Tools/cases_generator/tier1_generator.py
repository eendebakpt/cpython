"""Generate the main interpreter switch.
Reads the instruction definitions from bytecodes.c.
Writes the cases to generated_cases.c.h, which is #included in ceval.c.
"""

import argparse

from analyzer import (
    CodeSection,
    Analysis,
    Instruction,
    Uop,
    Part,
    analyze_files,
    Skip,
    Flush,
    analysis_error,
    StackItem,
)
from generators_common import (
    DEFAULT_INPUT,
    ROOT,
    write_header,
    type_and_null,
    Emitter,
    TokenIterator,
    emit_to,
    always_true,
)
from lexer import Token
from cwriter import CWriter
from typing import TextIO
from stack import Local, Stack, StackError, get_stack_effect, Storage

DEFAULT_OUTPUT = ROOT / "Python/generated_cases.c.h"


FOOTER = "#undef TIER_ONE\n"
INSTRUCTION_START_MARKER = "/* BEGIN INSTRUCTIONS */"
INSTRUCTION_END_MARKER = "/* END INSTRUCTIONS */"
LABEL_START_MARKER = "/* BEGIN LABELS */"
LABEL_END_MARKER = "/* END LABELS */"


def declare_variable(var: StackItem, out: CWriter) -> None:
    type, null = type_and_null(var)
    space = " " if type[-1].isalnum() else ""
    out.emit(f"{type}{space}{var.name};\n")


def declare_variables(inst: Instruction, out: CWriter) -> None:
    try:
        stack = get_stack_effect(inst)
    except StackError as ex:
        raise analysis_error(ex.args[0], inst.where) from None
    seen = {"unused"}
    for part in inst.parts:
        if not isinstance(part, Uop) or part.properties.records_value:
            continue
        for var in part.stack.inputs:
            if var.used and var.name not in seen:
                seen.add(var.name)
                declare_variable(var, out)
        for var in part.stack.outputs:
            if var.used and var.name not in seen:
                seen.add(var.name)
                declare_variable(var, out)


def write_uop(
    uop: Part,
    emitter: Emitter,
    offset: int,
    stack: Stack,
    inst: Instruction,
    braces: bool,
) -> tuple[bool, int, Stack]:
    # out.emit(stack.as_comment() + "\n")
    if isinstance(uop, Skip):
        entries = "entries" if uop.size > 1 else "entry"
        emitter.emit(f"/* Skip {uop.size} cache {entries} */\n")
        return True, (offset + uop.size), stack
    if isinstance(uop, Flush):
        emitter.emit(f"// flush\n")
        stack.flush(emitter.out)
        return True, offset, stack
    locals: dict[str, Local] = {}
    emitter.out.start_line()
    if braces:
        emitter.out.emit(f"// {uop.name}\n")
        emitter.emit("{\n")
        stack._print(emitter.out)
    storage = Storage.for_uop(stack, uop, emitter.out)

    for cache in uop.caches:
        if cache.name != "unused":
            if cache.size == 4:
                type = "PyObject *"
                reader = "read_obj"
            else:
                type = f"uint{cache.size*16}_t "
                reader = f"read_u{cache.size*16}"
            emitter.emit(
                f"{type}{cache.name} = {reader}(&this_instr[{offset}].cache);\n"
            )
            if inst.family is None:
                emitter.emit(f"(void){cache.name};\n")
        offset += cache.size

    reachable, storage = emitter.emit_tokens(uop, storage, inst, False)
    if braces:
        emitter.out.start_line()
        emitter.emit("}\n")
    # emitter.emit(stack.as_comment() + "\n")
    return reachable, offset, storage.stack


def uses_this(inst: Instruction) -> bool:
    if inst.properties.needs_this:
        return True
    for uop in inst.parts:
        if not isinstance(uop, Uop):
            continue
        for cache in uop.caches:
            if cache.name != "unused":
                return True
    # Can't be merged into the loop above, because
    # this must strictly be performed at the end.
    for uop in inst.parts:
        if not isinstance(uop, Uop):
            continue
        for tkn in uop.body.tokens():
            if (tkn.kind == "IDENTIFIER"
                    and (tkn.text in {"DEOPT_IF", "EXIT_IF", "AT_END_EXIT_IF"})):
                return True
    return False


UNKNOWN_OPCODE_HANDLER ="""\
_PyErr_Format(tstate, PyExc_SystemError,
              "%U:%d: unknown opcode %d",
              _PyFrame_GetCode(frame)->co_filename,
              PyUnstable_InterpreterFrame_GetLine(frame),
              opcode);
JUMP_TO_LABEL(error);
"""

def generate_switch_op_file(
    filenames: list[str], analysis: Analysis, outfile: TextIO, lines: bool
) -> None:
    """The file that holds _PyEval_SwitchOp; included by ceval.c outside the
    interpreter loop."""
    write_header(__file__, filenames, outfile)
    outfile.write("""
#ifdef TIER_TWO
    #error "This file is for Tier 1 only"
#endif
#ifndef TIER_ONE
#define TIER_ONE 1
#endif
""")
    out = CWriter(outfile, 0, lines)
    write_switch_op_function(analysis, out)


def generate_tier1(
    filenames: list[str], analysis: Analysis, outfile: TextIO, lines: bool,
    switch_outfile: TextIO | None = None,
) -> None:
    if switch_outfile is not None:
        generate_switch_op_file(filenames, analysis, switch_outfile, lines)
    write_header(__file__, filenames, outfile)
    outfile.write("""
#ifdef TIER_TWO
    #error "This file is for Tier 1 only"
#endif
#define TIER_ONE 1
""")
    outfile.write(f"""
#if !_Py_TAIL_CALL_INTERP
#if !USE_COMPUTED_GOTOS
    dispatch_opcode:
        switch (dispatch_code)
#endif
        {{
#endif /* _Py_TAIL_CALL_INTERP */
            {INSTRUCTION_START_MARKER}
"""
    )
    generate_tier1_cases(analysis, outfile, lines, switch_outfile is not None)
    outfile.write(f"""
            {INSTRUCTION_END_MARKER}
#if !_Py_TAIL_CALL_INTERP
#if USE_COMPUTED_GOTOS
        _unknown_opcode:
#else
        EXTRA_CASES  // From pycore_opcode_metadata.h, a 'case' for each unused opcode
#endif
            /* Tell C compilers not to hold the opcode variable in the loop.
               next_instr points the current instruction without TARGET(). */
            opcode = next_instr->op.code;
            {UNKNOWN_OPCODE_HANDLER}

        }}

        /* This should never be reached. Every opcode should end with DISPATCH()
           or goto error. */
        Py_UNREACHABLE();
#endif /* _Py_TAIL_CALL_INTERP */
        {LABEL_START_MARKER}
""")
    out = CWriter(outfile, 2, lines)
    emitter = Emitter(out, analysis.labels)
    generate_tier1_labels(analysis, emitter)
    outfile.write(f"{LABEL_END_MARKER}\n")
    outfile.write(FOOTER)



def generate_tier1_labels(
    analysis: Analysis, emitter: Emitter
) -> None:
    emitter.emit("\n")
    # Emit tail-callable labels as function defintions
    for name, label in analysis.labels.items():
        if name == 'stop_tracing':
            emitter.emit("#if _Py_TAIL_CALL_INTERP && !defined(_Py_TIER2)\n")
            emitter.emit("Py_GCC_ATTRIBUTE((unused))\n")
            emitter.emit("#endif\n")
        emitter.emit(f"LABEL({name})\n")
        storage = Storage(Stack(), [], [], 0, False)
        if label.spilled:
            storage.spilled = 1
            storage.stack.physical_sp = None
        emitter.emit_tokens(label, storage, None)
        emitter.emit("\n\n")

class SwitchEmitter(Emitter):
    """Emitter for specializations folded into SWITCH_OP: a failed guard
    re-dispatches to the family's generic instruction instead of jumping to
    its PREDICTED label."""

    deopt_macro = "SWITCH_OP_DEOPT"

    def deopt_if(
        self,
        tkn: Token,
        tkn_iter: TokenIterator,
        uop: Uop,
        storage: Storage,
        inst: Instruction | None,
    ) -> bool:
        self.out.start_line()
        self.out.emit("if (")
        lparen = next(tkn_iter)
        assert lparen.kind == "LPAREN"
        first_tkn = tkn_iter.peek()
        emit_to(self.out, tkn_iter, "RPAREN")
        self.emit(") {\n")
        next(tkn_iter)  # Semi colon
        assert inst is not None
        assert inst.family is not None
        self.emit(f"{self.deopt_macro}({inst.family.name});\n")
        self.emit("}\n")
        return not always_true(first_tkn)

    exit_if = deopt_if


class SwitchFunctionEmitter(SwitchEmitter):
    """Emitter for the cases of SWITCH_OP when they live in _PyEval_SwitchOp,
    a function outside the interpreter loop. Control leaves a case by
    returning (see SWITCH_OP_RETURN in ceval_macros.h), so every jump to a
    label of the interpreter loop becomes a return."""

    deopt_macro = "SWITCH_OP_DEOPT_RETURN"

    def goto_error(self, offset: int, storage: Storage) -> str:
        if offset > 0:
            # What the pop_N_error labels do before falling into 'error'.
            return f"stack_pointer += -{offset};\nSWITCH_OP_RETURN(NULL);"
        if offset < 0:
            storage.copy().flush(self.out)
        return "SWITCH_OP_RETURN(NULL);"

    def goto_label(self, goto: Token, label: Token, storage: Storage) -> None:
        if label.text != "error":
            raise analysis_error(
                f"A SWITCH_OP case cannot jump to label '{label.text}'", goto
            )
        if storage.spilled:
            raise analysis_error("Cannot jump to error with a spilled stack", goto)
        self.out.start_line()
        self.out.emit("SWITCH_OP_RETURN(NULL)")

    def dispatch(
        self,
        tkn: Token,
        tkn_iter: TokenIterator,
        uop: CodeSection,
        storage: Storage,
        inst: Instruction | None,
    ) -> bool:
        if storage.spilled:
            raise analysis_error("stack_pointer needs reloading before dispatch", tkn)
        storage.stack.flush(self.out)
        self.out.emit_at("SWITCH_OP_DISPATCH", tkn)
        return False


def write_instruction_body(
    inst: Instruction, out: CWriter, emitter: Emitter, end: str = "DISPATCH();"
) -> None:
    """The uops of an instruction, from the cache size check to DISPATCH()."""
    if inst.family is not None:
        out.emit(
            f"static_assert({inst.family.size} == {inst.size-1}"
            ', "incorrect cache size");\n'
        )
    declare_variables(inst, out)
    offset = 1  # The instruction itself
    stack = Stack()
    reachable = True
    for part in inst.parts:
        if part.properties.records_value:
            continue
        # Only emit braces if more than one uop
        insert_braces = len([p for p in inst.parts if isinstance(p, Uop)]) > 1
        reachable, offset, stack = write_uop(part, emitter, offset, stack, inst, insert_braces)
    out.start_line()
    if reachable:
        stack.flush(out)
        out.emit(f"{end}\n")


def write_switch_op_prologue(switch_op: Instruction, out: CWriter) -> None:
    out.emit("\n")
    out.emit("TARGET(SWITCH_OP) {\n")
    out.emit("#if _Py_TAIL_CALL_INTERP\n")
    out.emit("int opcode = SWITCH_OP;\n")
    out.emit("(void)(opcode);\n")
    out.emit("#endif\n")
    out.emit("_Py_CODEUNIT* const this_instr = next_instr;\n")
    out.emit("(void)this_instr;\n")
    out.emit("frame->instr_ptr = next_instr;\n")
    out.emit(f"next_instr += {switch_op.size};\n")
    out.emit("INSTRUCTION_STATS(SWITCH_OP);\n")
    out.emit("uint16_t switch_case = read_u16(&this_instr[1].cache) >> SWITCH_OP_CASE_SHIFT;\n")
    out.emit("assert(switch_case < SWITCH_OP_NCASES);\n")


def write_switch_op_call(analysis: Analysis, out: CWriter) -> None:
    """SWITCH_OP as a call of the case's function (see
    write_switch_op_function). The function returns the next
    instruction, this_instr to re-dispatch to the family's generic
    instruction after a failed guard, or NULL on error."""
    switch_op = analysis.instructions["SWITCH_OP"]
    write_switch_op_prologue(switch_op, out)
    out.emit("_PySwitchOpResult result = _PySwitchOp_Func[switch_case](tstate, frame, stack_pointer, this_instr, oparg);\n")
    out.emit("stack_pointer = result.stack_pointer;\n")
    out.emit("_Py_CODEUNIT *target = result.next_instr;\n")
    out.emit("if (target == NULL) {\n")
    out.emit("// Point into the instruction, as the generic instruction would.\n")
    out.emit("next_instr = this_instr + 1 + _PyOpcode_Caches[_PySwitchOp_Deopt[switch_case]];\n")
    out.emit("JUMP_TO_LABEL(error);\n")
    out.emit("}\n")
    out.emit("if (target == this_instr) {\n")
    out.emit("opcode = _PySwitchOp_Deopt[switch_case];\n")
    out.emit("next_instr = this_instr;\n")
    out.emit("PRE_DISPATCH_GOTO();\n")
    out.emit("DISPATCH_GOTO();\n")
    out.emit("}\n")
    out.emit("next_instr = target;\n")
    out.emit("DISPATCH();\n")
    out.start_line()
    out.emit("}")
    out.emit("\n")


def write_switch_op_function(analysis: Analysis, out: CWriter) -> None:
    """The cases of SWITCH_OP as functions outside the interpreter loop, so
    that folding specializations into SWITCH_OP does not grow the loop. One
    function per case keeps each prologue minimal; _PySwitchOp_Func maps the
    case number to its function."""
    emitter = SwitchFunctionEmitter(out, analysis.labels)
    for inst in analysis.switch_cases:
        assert inst.family is not None
        out.emit("\n")
        out.emit("static Py_NO_INLINE _PySwitchOpResult\n")
        out.emit(f"_PyEval_SwitchOp_{inst.name}(PyThreadState *tstate, _PyInterpreterFrame *frame,\n")
        out.emit("    _PyStackRef *stack_pointer, _Py_CODEUNIT *this_instr, int oparg)\n")
        out.emit("{\n")
        out.emit("(void)tstate;\n")
        out.emit("(void)oparg;\n")
        out.emit("// The instruction is as long as its family's generic instruction.\n")
        out.emit(f"_Py_CODEUNIT *next_instr = this_instr + 1 + INLINE_CACHE_ENTRIES_{inst.family.name};\n")
        write_instruction_body(inst, out, emitter, end="SWITCH_OP_DISPATCH();")
        out.start_line()
        out.emit("}\n")
    out.emit("\n")
    out.emit("static _PySwitchOpResult (*const _PySwitchOp_Func[SWITCH_OP_NCASES])(\n")
    out.emit("    PyThreadState *, _PyInterpreterFrame *, _PyStackRef *, _Py_CODEUNIT *, int) = {\n")
    for inst in analysis.switch_cases:
        out.emit(f"[SWITCH_CASE_{inst.name}] = _PyEval_SwitchOp_{inst.name},\n")
    out.emit("};\n")


def write_switch_op(analysis: Analysis, out: CWriter) -> None:
    """SWITCH_OP: one instruction whose cases are the folded specializations.
    The case number replaces the adaptive counter in the first cache entry; the
    instruction otherwise keeps the cache layout of its family. This is the
    inline form, with the cases in the interpreter loop."""
    emitter = SwitchEmitter(out, analysis.labels)
    switch_op = analysis.instructions["SWITCH_OP"]
    write_switch_op_prologue(switch_op, out)
    out.emit("switch (switch_case) {\n")
    for inst in analysis.switch_cases:
        assert inst.family is not None
        out.emit(f"case SWITCH_CASE_{inst.name}: {{\n")
        out.emit("// The instruction is as long as its family's generic instruction.\n")
        out.emit(f"next_instr = this_instr + 1 + INLINE_CACHE_ENTRIES_{inst.family.name};\n")
        write_instruction_body(inst, out, emitter)
        out.start_line()
        out.emit("}\n")
    out.emit("default:\n")
    out.emit("Py_UNREACHABLE();\n")
    out.emit("}\n")
    out.start_line()
    out.emit("}")
    out.emit("\n")


def get_popped(inst: Instruction, analysis: Analysis) -> str:
    stack = get_stack_effect(inst)
    return (-stack.base_offset).to_c()

def generate_tier1_cases(
    analysis: Analysis, outfile: TextIO, lines: bool, outlined_switch: bool = False
) -> None:
    out = CWriter(outfile, 2, lines)
    emitter = Emitter(out, analysis.labels)
    out.emit("\n")
    for name, inst in sorted(analysis.instructions.items()):
        if name == "SWITCH_OP" and analysis.switch_cases:
            if outlined_switch:
                write_switch_op_call(analysis, out)
            else:
                write_switch_op(analysis, out)
            continue
        out.emit("\n")
        out.emit(f"TARGET({name}) {{\n")
        popped = get_popped(inst, analysis)
        # We need to ifdef it because this breaks platforms
        # without computed gotos/tail calling.
        out.emit(f"#if _Py_TAIL_CALL_INTERP\n")
        out.emit(f"int opcode = {name};\n")
        out.emit(f"(void)(opcode);\n")
        out.emit(f"#endif\n")
        needs_this = uses_this(inst)
        unused_guard = "(void)this_instr;\n"
        if inst.properties.needs_prev:
            out.emit(f"_Py_CODEUNIT* const prev_instr = frame->instr_ptr;\n")

        if needs_this and not inst.is_target:
            out.emit(f"_Py_CODEUNIT* const this_instr = next_instr;\n")
            out.emit(unused_guard)
        if not inst.properties.no_save_ip:
            out.emit(f"frame->instr_ptr = next_instr;\n")

        out.emit(f"next_instr += {inst.size};\n")
        out.emit(f"INSTRUCTION_STATS({name});\n")
        if inst.is_target:
            out.emit(f"PREDICTED_{name}:;\n")
            if needs_this:
                out.emit(f"_Py_CODEUNIT* const this_instr = next_instr - {inst.size};\n")
                out.emit(unused_guard)
        if inst.properties.uses_opcode:
            out.emit(f"opcode = {name};\n")
        if inst.family is not None:
            out.emit(
                f"static_assert({inst.family.size} == {inst.size-1}"
                ', "incorrect cache size");\n'
            )
        declare_variables(inst, out)
        offset = 1  # The instruction itself
        stack = Stack()
        for part in inst.parts:
            if part.properties.records_value:
                continue
            # Only emit braces if more than one uop
            insert_braces = len([p for p in inst.parts if isinstance(p, Uop)]) > 1
            reachable, offset, stack = write_uop(part, emitter, offset, stack, inst, insert_braces)
        out.start_line()
        if reachable: # type: ignore[possibly-undefined]
            stack.flush(out)
            out.emit("DISPATCH();\n")
        out.start_line()
        out.emit("}")
        out.emit("\n")


arg_parser = argparse.ArgumentParser(
    description="Generate the code for the interpreter switch.",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)

arg_parser.add_argument(
    "-o", "--output", type=str, help="Generated code", default=DEFAULT_OUTPUT
)

arg_parser.add_argument(
    "-s", "--switch-output", type=str, default=None,
    help="Emit the cases of SWITCH_OP as a function into this file, "
         "instead of inline in the interpreter loop",
)

arg_parser.add_argument(
    "-l", "--emit-line-directives", help="Emit #line directives", action="store_true"
)

arg_parser.add_argument(
    "input", nargs=argparse.REMAINDER, help="Instruction definition file(s)"
)


def generate_tier1_from_files(
    filenames: list[str], outfilename: str, lines: bool
) -> None:
    data = analyze_files(filenames)
    with open(outfilename, "w") as outfile:
        generate_tier1(filenames, data, outfile, lines)


if __name__ == "__main__":
    args = arg_parser.parse_args()
    if len(args.input) == 0:
        args.input.append(DEFAULT_INPUT)
    data = analyze_files(args.input)
    with open(args.output, "w") as outfile:
        if args.switch_output is None:
            generate_tier1(args.input, data, outfile, args.emit_line_directives)
        else:
            with open(args.switch_output, "w") as switch_outfile:
                generate_tier1(args.input, data, outfile, args.emit_line_directives,
                               switch_outfile)
