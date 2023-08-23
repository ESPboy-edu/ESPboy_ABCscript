#include "ards_compiler.hpp"

#include <sstream>
#include <assert.h>

namespace ards
{

compiler_lvalue_t compiler_t::resolve_lvalue(ast_node_t const& n, compiler_frame_t const& frame)
{
    assert(n.type == AST::IDENT);
    std::string name(n.data);
    for(auto it = frame.scopes.rbegin(); it != frame.scopes.rend(); ++it)
    {
        auto jt = it->locals.find(name);
        if(jt != it->locals.end())
        {
            size_t offset = frame.size - jt->second.frame_offset;
            if(offset >= 256)
                errs.push_back({ "Stack frame exceeded 256 bytes", n.line_info });
            return { jt->second.type.prim_size, false, uint8_t(offset) };
        }
    }
    auto it = globals.find(name);
    if(it != globals.end())
        return { it->second.type.prim_size, true, 0, name };
    errs.push_back({ "Undefined variable \"" + name + "\"", n.line_info });
    return {};
}

compiler_lvalue_t compiler_t::return_lvalue(compiler_func_t const& f, compiler_frame_t const& frame)
{
    compiler_lvalue_t t{};
    t.size = f.decl.return_type.prim_size;
    t.stack_index = frame.size + t.size;
    return t;
}

void compiler_t::codegen_return(compiler_func_t& f, compiler_frame_t& frame, ast_node_t const& n)
{
    // store return value
    if(!n.children.empty())
    {
        codegen_expr(f, frame, n.children[0]);
        codegen_convert(f, frame, f.decl.return_type, n.children[0].comp_type);
        auto lvalue = return_lvalue(f, frame);
        codegen_store_lvalue(f, lvalue);
        frame.size -= lvalue.size;
    }
    else if(f.decl.return_type.prim_size != 0)
    {
        errs.push_back({ "Missing return value", n.line_info });
        return;
    }

    // pop remaining func args
    for(size_t i = 0; i < frame.size; ++i)
        f.instrs.push_back({ I_POP });

    f.instrs.push_back({ I_RET });
}

void compiler_t::codegen_function(compiler_func_t& f)
{
    compiler_frame_t frame{};

    frame.push();

    // add func args to scope here
    assert(f.arg_names.size() == f.decl.arg_types.size());
    auto& scope = frame.scopes.back();
    for(size_t i = 0; i < f.arg_names.size(); ++i)
    {
        auto const& name = f.arg_names[i];
        auto const& type = f.decl.arg_types[i];
        auto& local = scope.locals[name];
        size_t size = type.prim_size;
        local.frame_offset = frame.size;
        local.type = type;
        scope.size += size;
        frame.size += size;
    }

    frame.push();

    codegen(f, frame, f.block);

    // no need to call codegen_return here
    // all function blocks are guaranteed to end with a return statement
    assert(f.block.children.back().type == AST::RETURN_STMT);
}

std::string compiler_t::codegen_label(compiler_func_t& f)
{
    std::ostringstream ss;
    ss << f.label_count;
    f.label_count += 1;
    std::string label = "$L_" + f.name + "_" + ss.str();
    f.instrs.push_back({ I_NOP, 0, label, true });
    return label;
}

void compiler_t::codegen(compiler_func_t& f, compiler_frame_t& frame, ast_node_t& a)
{
    if(!errs.empty()) return;
    auto prev_size = frame.size;
    switch(a.type)
    {
    case AST::EMPTY_STMT:
        break;
    case AST::IF_STMT:
    {
        assert(a.children.size() == 3);
        type_annotate(a.children[0], frame);
        codegen_expr(f, frame, a.children[0]);
        // TODO: unnecessary for a.children[0].comp_type.prim_size == 1
        codegen_convert(f, frame, TYPE_BOOL, a.children[0].comp_type);
        size_t cond_index = f.instrs.size();
        f.instrs.push_back({ I_BZ });
        frame.size -= 1;
        codegen(f, frame, a.children[1]);
        size_t jmp_index = f.instrs.size();
        if(a.children[2].type != AST::EMPTY_STMT)
            f.instrs.push_back({ I_JMP });
        auto else_label = codegen_label(f);
        f.instrs[cond_index].label = else_label;
        if(a.children[2].type != AST::EMPTY_STMT)
        {
            codegen(f, frame, a.children[2]);
            auto end_label = codegen_label(f);
            f.instrs[jmp_index].label = end_label;
        }
        break;
    }
    case AST::WHILE_STMT:
    {
        assert(a.children.size() == 2);
        type_annotate(a.children[0], frame);
        auto start = codegen_label(f);
        codegen_expr(f, frame, a.children[0]);
        // TODO: unnecessary for a.children[0].comp_type.prim_size == 1
        codegen_convert(f, frame, TYPE_BOOL, a.children[0].comp_type);
        size_t cond_index = f.instrs.size();
        f.instrs.push_back({ I_BZ });
        frame.size -= 1;
        codegen(f, frame, a.children[1]);
        f.instrs.push_back({ I_JMP, 0, start });
        auto end = codegen_label(f);
        f.instrs[cond_index].label = end;
        break;
    }
    case AST::BLOCK:
    {
        size_t prev_size = frame.size;
        frame.push();
        for(auto& child : a.children)
            codegen(f, frame, child);
        for(size_t i = 0; i < frame.scopes.back().size; ++i)
            f.instrs.push_back({ I_POP });
        frame.pop();
        assert(!errs.empty() || frame.size == prev_size);
        break;
    }
    case AST::RETURN_STMT:
    {
        if(!a.children.empty())
            type_annotate(a.children[0], frame);
        codegen_return(f, frame, a);
        break;
    }
    case AST::EXPR_STMT:
    {
        size_t prev_size = frame.size;
        assert(a.children.size() == 1);
        type_annotate(a.children[0], frame);
        codegen_expr(f, frame, a.children[0]);
        for(size_t i = prev_size; i < frame.size; ++i)
            f.instrs.push_back({ I_POP });
        frame.size = prev_size;
        break;
    }
    case AST::DECL_STMT:
    {
        // add local var to frame
        assert(a.children.size() == 2 || a.children.size() == 3);
        assert(a.children[0].type == AST::TYPE);
        assert(a.children[1].type == AST::IDENT);
        auto type = resolve_type(a.children[0]);
        if(type.prim_size == 0)
        {
            errs.push_back({
                "Local variable \"" + std::string(a.children[1].data) + "\" has zero size",
                a.line_info });
            return;
        }
        auto& scope = frame.scopes.back();
        auto& local = scope.locals[std::string(a.children[1].data)];
        local.type = type;
        local.frame_offset = frame.size;
        if(a.children.size() == 3)
        {
            type_annotate(a.children[2], frame);
            codegen_expr(f, frame, a.children[2]);
            codegen_convert(f, frame, type, a.children[2].comp_type);
        }
        else
        {
            frame.size += type.prim_size;
            for(size_t i = 0; i < type.prim_size; ++i)
                f.instrs.push_back({ I_PUSH, 0 });
        }
        scope.size += type.prim_size;
        break;
    }
    default:
        assert(false);
        errs.push_back({ "(codegen) Unimplemented AST node", a.line_info });
        return;
    }
    if(a.type != AST::DECL_STMT && errs.empty())
        assert(frame.size == prev_size);
}

void compiler_t::codegen_store_lvalue(compiler_func_t& f, compiler_lvalue_t const& lvalue)
{
    if(!errs.empty()) return;
    if(lvalue.is_global)
    {
        assert(lvalue.size < 256);
        f.instrs.push_back({ I_PUSH, (uint8_t)lvalue.size });
        f.instrs.push_back({ I_SETGN, 0, lvalue.global_name });
    }
    else
    {
        f.instrs.push_back({ I_PUSH, (uint8_t)lvalue.size });
        f.instrs.push_back({ I_SETLN, (uint8_t)(lvalue.stack_index - lvalue.size) });
    }
}

void compiler_t::codegen_convert(
    compiler_func_t& f, compiler_frame_t& frame,
    compiler_type_t const& to, compiler_type_t const& from)
{
    if(!errs.empty()) return;
    assert(from.prim_size != 0);
    if(to == from) return;
    if(to.is_bool)
    {
        if(from.is_bool) return;
        int n = int(from.prim_size - 1);
        frame.size -= n;
        static_assert(I_BOOL2 == I_BOOL + 1);
        static_assert(I_BOOL3 == I_BOOL + 2);
        static_assert(I_BOOL4 == I_BOOL + 3);
        f.instrs.push_back({ instr_t(I_BOOL + n) });
        return;
    }
    if(to.prim_size == from.prim_size) return;
    if(to.prim_size < from.prim_size)
    {
        int n = from.prim_size - to.prim_size;
        frame.size -= n;
        for(int i = 0; i < n; ++i)
            f.instrs.push_back({ I_POP });
    }
    if(to.prim_size > from.prim_size)
    {
        int n = to.prim_size - from.prim_size;
        frame.size += n;
        compiler_instr_t instr;
        if(from.prim_signed)
            instr = { I_SEXT };
        else
            instr = { I_PUSH, 0 };
        for(int i = 0; i < n; ++i)
            f.instrs.push_back(instr);
    }
}

void compiler_t::codegen_expr(compiler_func_t& f, compiler_frame_t& frame, ast_node_t const& a)
{
    if(!errs.empty()) return;
    switch(a.type)
    {
    case AST::OP_CAST:
    {
        assert(a.children.size() == 2);
        codegen_expr(f, frame, a.children[1]);
        codegen_convert(f, frame, a.children[0].comp_type, a.children[1].comp_type);
        return;
    }
    case AST::OP_UNARY:
    {
        assert(a.children.size() == 2);
        auto op = a.children[0].data;
        if(op == "!")
        {
            codegen_expr(f, frame, a.children[1]);
            codegen_convert(f, frame, TYPE_BOOL, a.children[1].comp_type);
            f.instrs.push_back({ I_NOT });
        }
        else if(op == "-")
        {
            auto size = a.children[1].comp_type.prim_size;
            for(size_t i = 0; i < size; ++i)
                f.instrs.push_back({ I_PUSH, 0 });
            frame.size += size;
            codegen_expr(f, frame, a.children[1]);
            frame.size -= size;
            f.instrs.push_back({ instr_t(I_SUB + size - 1) });
        }
        else
        {
            assert(false);
        }
        return;
    }
    case AST::INT_CONST:
    {
        uint32_t x = (uint32_t)a.value;
        frame.size += a.comp_type.prim_size;
        for(size_t i = 0; i < a.comp_type.prim_size; ++i, x >>= 8)
            f.instrs.push_back({ I_PUSH, (uint8_t)x });
        return;
    }
    case AST::IDENT:
    {
        std::string name(a.data);
        for(auto it = frame.scopes.rbegin(); it != frame.scopes.rend(); ++it)
        {
            auto jt = it->locals.find(name);
            if(jt != it->locals.end())
            {
                f.instrs.push_back({ I_PUSH, (uint8_t)jt->second.type.prim_size });
                f.instrs.push_back({ I_GETLN, (uint8_t)(frame.size - jt->second.frame_offset) });
                frame.size += (uint8_t)jt->second.type.prim_size;
                return;
            }
        }
        auto it = globals.find(name);
        if(it != globals.end())
        {
            assert(it->second.type.prim_size < 256);
            frame.size += (uint8_t)it->second.type.prim_size;
            f.instrs.push_back({ I_PUSH, (uint8_t)it->second.type.prim_size });
            f.instrs.push_back({ I_GETGN, 0, it->second.name });
            return;
        }
        errs.push_back({ "Undefined variable \"" + name + "\"", a.line_info });
        return;
    }
    case AST::FUNC_CALL:
    {
        assert(a.children.size() == 2);
        assert(a.children[0].type == AST::IDENT);

        auto func = resolve_func(a.children[0]);
        
        // system functions don't need space reserved for return value
        if(!func.is_sys)
        {
            // reserve space for return value
            frame.size += func.decl.return_type.prim_size;
            for(size_t i = 0; i < func.decl.return_type.prim_size; ++i)
                f.instrs.push_back({ I_PUSH, 0 });
        }

        assert(a.children[1].type == AST::FUNC_ARGS);
        if(a.children[1].children.size() != func.decl.arg_types.size())
        {
            errs.push_back({
                "Incorrect number of arguments to function \"" + func.name + "\"",
                a.line_info });
            return;
        }
        size_t prev_size = frame.size;
        for(size_t i = 0; i < func.decl.arg_types.size(); ++i)
        {
            auto const& type = func.decl.arg_types[i];
            auto const& expr = a.children[1].children[i];
            codegen_expr(f, frame, expr);
            codegen_convert(f, frame, type, expr.comp_type);
        }
        // called function should pop stack
        frame.size = prev_size;

        if(func.is_sys)
            f.instrs.push_back({ I_SYS, func.sys });
        else
            f.instrs.push_back({ I_CALL, 0, std::string(a.children[0].data) });

        // system functions push return value onto stack
        if(func.is_sys)
            frame.size += func.decl.return_type.prim_size;

        return;
    }
    case AST::OP_ASSIGN:
    {
        assert(a.children.size() == 2);
        assert(a.children[0].comp_type.prim_size != 0);
        codegen_expr(f, frame, a.children[1]);
        codegen_convert(f, frame, a.children[0].comp_type, a.children[1].comp_type);

        // dup value if not the root op
        switch(a.parent->type)
        {
        case AST::EXPR_STMT:
        case AST::LIST:
            break;
        default:
            frame.size += (uint8_t)a.children[0].comp_type.prim_size;
            f.instrs.push_back({ I_PUSH, (uint8_t)a.children[0].comp_type.prim_size });
            f.instrs.push_back({ I_GETLN, (uint8_t)a.children[0].comp_type.prim_size });
            break;
        }

        auto lvalue = resolve_lvalue(a.children[0], frame);
        codegen_store_lvalue(f, lvalue);
        frame.size -= lvalue.size;
        return;
    }
    case AST::OP_EQUALITY:
    case AST::OP_RELATIONAL:
    {
        assert(a.children.size() == 2);
        assert(a.children[0].comp_type == a.children[1].comp_type);
        assert(a.comp_type == TYPE_BOOL);
        size_t i0 = 0, i1 = 1;
        if(a.data == ">" || a.data == ">=")
            std::swap(i0, i1);
        codegen_expr(f, frame, a.children[i0]);
        codegen_expr(f, frame, a.children[i1]);
        
        auto size = a.children[0].comp_type.prim_size;
        assert(size >= 1 && size <= 4);
        frame.size -= size;       // comparison
        frame.size -= (size - 1); // conversion to bool
        if(a.data == "==" || a.data == "!=")
        {
            f.instrs.push_back({ instr_t(I_SUB + size - 1) });
            f.instrs.push_back({ instr_t(I_BOOL + size - 1) });
            if(a.data == "==")
                f.instrs.push_back({ I_NOT });
        }
        else if(a.data == "<=" || a.data == ">=")
        {
            instr_t i = (a.comp_type.prim_signed ? I_CSLE : I_CULE);
            f.instrs.push_back({ instr_t(i + size - 1) });
        }
        else if(a.data == "<" || a.data == ">")
        {
            instr_t i = (a.comp_type.prim_signed ? I_CSLT : I_CULT);
            f.instrs.push_back({ instr_t(i + size - 1) });
        }
        else
            assert(false);
        break;
    }
    case AST::OP_ADDITIVE:
    {
        assert(a.data == "+" || a.data == "-");
        assert(a.children.size() == 2);
        assert(a.children[0].comp_type == a.comp_type);
        assert(a.children[1].comp_type == a.comp_type);
        codegen_expr(f, frame, a.children[0]);
        codegen_expr(f, frame, a.children[1]);
        static_assert(I_ADD2 == I_ADD + 1);
        static_assert(I_ADD3 == I_ADD + 2);
        static_assert(I_ADD4 == I_ADD + 3);
        static_assert(I_SUB2 == I_SUB + 1);
        static_assert(I_SUB3 == I_SUB + 2);
        static_assert(I_SUB4 == I_SUB + 3);
        auto size = a.comp_type.prim_size;
        assert(size >= 1 && size <= 4);
        frame.size -= size;
        f.instrs.push_back({ instr_t((a.data == "+" ? I_ADD : I_SUB) + size - 1) });
        return;
    }
    case AST::OP_MULTIPLICATIVE:
    {
        assert(a.children.size() == 2);
        assert(a.children[0].comp_type == a.comp_type);
        assert(a.children[1].comp_type == a.comp_type);
        codegen_expr(f, frame, a.children[0]);
        codegen_expr(f, frame, a.children[1]);
        static_assert(I_MUL2 == I_MUL + 1);
        static_assert(I_MUL3 == I_MUL + 2);
        static_assert(I_MUL4 == I_MUL + 3);
        auto size = a.comp_type.prim_size;
        assert(size >= 1 && size <= 4);
        frame.size -= size;
        if(a.data == "*")
            f.instrs.push_back({ instr_t(I_MUL + size - 1) });
        else
            assert(false);
        return;
    }
    default:
        assert(false);
        errs.push_back({ "(codegen_expr) Unimplemented AST node", a.line_info });
        return;
    }
}

}
