# Sorry for using pytest, these tests are mostly just for me.
# Use pytest -vv for best results.

import tempfile

import generate_cases
from parser import StackEffect


def test_effect_sizes():
    input_effects = [
        x := StackEffect("x", "", ""),
        y := StackEffect("y", "", "oparg"),
        z := StackEffect("z", "", "oparg*2"),
    ]
    output_effects = [
        a := StackEffect("a", "", ""),
        b := StackEffect("b", "", "oparg*4"),
        c := StackEffect("c", "", ""),
    ]
    other_effects = [
        p := StackEffect("p", "", "oparg<<1"),
        q := StackEffect("q", "", ""),
        r := StackEffect("r", "", ""),
    ]
    assert generate_cases.effect_size(x) == (1, "")
    assert generate_cases.effect_size(y) == (0, "oparg")
    assert generate_cases.effect_size(z) == (0, "oparg*2")

    assert generate_cases.list_effect_size(input_effects) == (1, "oparg + oparg*2")
    assert generate_cases.list_effect_size(output_effects) == (2, "oparg*4")
    assert generate_cases.list_effect_size(other_effects) == (2, "(oparg<<1)")

    assert generate_cases.string_effect_size(generate_cases.list_effect_size(input_effects)) == "1 + oparg + oparg*2"
    assert generate_cases.string_effect_size(generate_cases.list_effect_size(output_effects)) == "2 + oparg*4"
    assert generate_cases.string_effect_size(generate_cases.list_effect_size(other_effects)) == "2 + (oparg<<1)"


def run_cases_test(input: str, expected: str):
    temp_input = tempfile.NamedTemporaryFile("w+")
    temp_input.write(generate_cases.BEGIN_MARKER)
    temp_input.write(input)
    temp_input.write(generate_cases.END_MARKER)
    temp_input.flush()
    temp_output = tempfile.NamedTemporaryFile("w+")
    a = generate_cases.Analyzer(temp_input.name, temp_output.name)
    a.parse()
    a.analyze()
    if a.errors:
        raise RuntimeError(f"Found {a.errors} errors")
    a.write_instructions()
    temp_output.seek(0)
    lines = temp_output.readlines()
    while lines and lines[0].startswith("// "):
        lines.pop(0)
    actual = "".join(lines)
    assert actual.rstrip() == expected.rstrip()

def test_legacy():
    input = """
        inst(OP) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            spam();
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_inst_no_args():
    input = """
        inst(OP, (--)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            spam();
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_inst_one_pop():
    input = """
        inst(OP, (value --)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject *value = PEEK(1);
            spam();
            STACK_SHRINK(1);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_inst_one_push():
    input = """
        inst(OP, (-- res)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject *res;
            spam();
            STACK_GROW(1);
            POKE(1, res);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_inst_one_push_one_pop():
    input = """
        inst(OP, (value -- res)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject *value = PEEK(1);
            PyObject *res;
            spam();
            POKE(1, res);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_binary_op():
    input = """
        inst(OP, (left, right -- res)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject *right = PEEK(1);
            PyObject *left = PEEK(2);
            PyObject *res;
            spam();
            STACK_SHRINK(1);
            POKE(1, res);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_overlap():
    input = """
        inst(OP, (left, right -- left, result)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject *right = PEEK(1);
            PyObject *left = PEEK(2);
            PyObject *result;
            spam();
            POKE(1, result);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_predictions():
    input = """
        inst(OP1, (--)) {
        }
        inst(OP2, (--)) {
        }
        inst(OP3, (--)) {
            DEOPT_IF(xxx, OP1);
            PREDICT(OP2);
        }
    """
    output = """
        TARGET(OP1) {
            PREDICTED(OP1);
            DISPATCH();
        }

        TARGET(OP2) {
            PREDICTED(OP2);
            DISPATCH();
        }

        TARGET(OP3) {
            DEOPT_IF(xxx, OP1);
            PREDICT(OP2);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_error_if_plain():
    input = """
        inst(OP, (--)) {
            ERROR_IF(cond, label);
        }
    """
    output = """
        TARGET(OP) {
            if (cond) goto label;
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_error_if_plain_with_comment():
    input = """
        inst(OP, (--)) {
            ERROR_IF(cond, label);  // Comment is ok
        }
    """
    output = """
        TARGET(OP) {
            if (cond) goto label;
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_error_if_pop():
    input = """
        inst(OP, (left, right -- res)) {
            ERROR_IF(cond, label);
        }
    """
    output = """
        TARGET(OP) {
            PyObject *right = PEEK(1);
            PyObject *left = PEEK(2);
            PyObject *res;
            if (cond) goto pop_2_label;
            STACK_SHRINK(1);
            POKE(1, res);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_cache_effect():
    input = """
        inst(OP, (counter/1, extra/2, value --)) {
        }
    """
    output = """
        TARGET(OP) {
            PyObject *value = PEEK(1);
            uint16_t counter = read_u16(&next_instr[0].cache);
            uint32_t extra = read_u32(&next_instr[1].cache);
            STACK_SHRINK(1);
            JUMPBY(3);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_suppress_dispatch():
    input = """
        inst(OP, (--)) {
            goto somewhere;
        }
    """
    output = """
        TARGET(OP) {
            goto somewhere;
        }
    """
    run_cases_test(input, output)

def test_super_instruction():
    # TODO: Test cache effect
    input = """
        inst(OP1, (counter/1, arg --)) {
            op1();
        }
        inst(OP2, (extra/2, arg --)) {
            op2();
        }
        super(OP) = OP1 + OP2;
    """
    output = """
        TARGET(OP1) {
            PyObject *arg = PEEK(1);
            uint16_t counter = read_u16(&next_instr[0].cache);
            op1();
            STACK_SHRINK(1);
            JUMPBY(1);
            DISPATCH();
        }

        TARGET(OP2) {
            PyObject *arg = PEEK(1);
            uint32_t extra = read_u32(&next_instr[0].cache);
            op2();
            STACK_SHRINK(1);
            JUMPBY(2);
            DISPATCH();
        }

        TARGET(OP) {
            PyObject *_tmp_1 = PEEK(1);
            PyObject *_tmp_2 = PEEK(2);
            {
                PyObject *arg = _tmp_1;
                uint16_t counter = read_u16(&next_instr[0].cache);
                op1();
            }
            JUMPBY(1);
            NEXTOPARG();
            JUMPBY(1);
            {
                PyObject *arg = _tmp_2;
                uint32_t extra = read_u32(&next_instr[0].cache);
                op2();
            }
            JUMPBY(2);
            STACK_SHRINK(2);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_macro_instruction():
    input = """
        inst(OP1, (counter/1, arg1 -- interim)) {
            interim = op1(arg1);
        }
        op(OP2, (extra/2, arg2, interim -- res)) {
            res = op2(arg2, interim);
        }
        macro(OP) = OP1 + cache/2 + OP2;
    """
    output = """
        TARGET(OP1) {
            PyObject *arg1 = PEEK(1);
            PyObject *interim;
            uint16_t counter = read_u16(&next_instr[0].cache);
            interim = op1(arg1);
            POKE(1, interim);
            JUMPBY(1);
            DISPATCH();
        }

        TARGET(OP) {
            PyObject *_tmp_1 = PEEK(1);
            PyObject *_tmp_2 = PEEK(2);
            {
                PyObject *arg1 = _tmp_1;
                PyObject *interim;
                uint16_t counter = read_u16(&next_instr[0].cache);
                interim = op1(arg1);
                _tmp_1 = interim;
            }
            {
                PyObject *interim = _tmp_1;
                PyObject *arg2 = _tmp_2;
                PyObject *res;
                uint32_t extra = read_u32(&next_instr[3].cache);
                res = op2(arg2, interim);
                _tmp_2 = res;
            }
            JUMPBY(5);
            STACK_SHRINK(1);
            POKE(1, _tmp_2);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_array_input():
    input = """
        inst(OP, (below, values[oparg*2], above --)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject *above = PEEK(1);
            PyObject **values = &PEEK(1 + oparg*2);
            PyObject *below = PEEK(2 + oparg*2);
            spam();
            STACK_SHRINK(oparg*2);
            STACK_SHRINK(2);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_array_output():
    input = """
        inst(OP, (-- below, values[oparg*3], above)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject *below;
            PyObject **values;
            PyObject *above;
            spam();
            STACK_GROW(2);
            STACK_GROW(oparg*3);
            POKE(1, above);
            MOVE_ITEMS(&PEEK(1 + oparg*3), values, oparg*3);
            POKE(2 + oparg*3, below);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_array_input_output():
    input = """
        inst(OP, (below, values[oparg] -- values[oparg], above)) {
            spam();
        }
    """
    output = """
        TARGET(OP) {
            PyObject **values = &PEEK(oparg);
            PyObject *below = PEEK(1 + oparg);
            PyObject *above;
            spam();
            POKE(1, above);
            MOVE_ITEMS(&PEEK(1 + oparg), values, oparg);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_array_error_if():
    input = """
        inst(OP, (extra, values[oparg] --)) {
            ERROR_IF(oparg == 0, somewhere);
        }
    """
    output = """
        TARGET(OP) {
            PyObject **values = &PEEK(oparg);
            PyObject *extra = PEEK(1 + oparg);
            if (oparg == 0) { STACK_SHRINK(oparg); goto pop_1_somewhere; }
            STACK_SHRINK(oparg);
            STACK_SHRINK(1);
            DISPATCH();
        }
    """
    run_cases_test(input, output)

def test_register():
    input = """
        register inst(OP, (counter/1, left, right -- result)) {
            result = op(left, right);
        }
    """
    output = """
        TARGET(OP) {
            PyObject *left = REG(oparg1);
            PyObject *right = REG(oparg2);
            PyObject *result;
            uint16_t counter = read_u16(&next_instr[0].cache);
            result = op(left, right);
            Py_XSETREF(REG(oparg3), result);
            JUMPBY(1);
            DISPATCH();
        }
    """
    run_cases_test(input, output)
